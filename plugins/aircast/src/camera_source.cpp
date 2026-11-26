#include "camera_source.h"

#include "peer.h"

#include <obs-module.h>
#include <util/platform.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
}

#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace aircast {

namespace {
struct Camera {
	std::shared_ptr<Peer> peer;
	std::string name;
	std::string state = "closed";
};

struct Src {
	obs_source_t *source;
	int camera_id;
	Feed feed;
};

// Lock order: g_m -> Peer's lock. Peer network threads take g_m only via the sink, never the reverse.
std::mutex g_m;
std::map<int, Camera> g_cams;
std::vector<Src *> g_srcs;
EventEmitter g_emit;

Role role_for(int camera_id)  // g_m held
{
	for (Src *s : g_srcs)
		if (s->camera_id == camera_id && obs_source_active(s->source))
			return Role::Program;
	return Role::Preview;
}

void update_role(int camera_id)  // g_m held
{
	auto it = g_cams.find(camera_id);
	if (it != g_cams.end() && it->second.peer)
		it->second.peer->set_role(role_for(camera_id));
}

void emit(const char *event, int camera_id, std::initializer_list<std::pair<const char *, std::string>> fields)
{
	EventEmitter e;
	{
		std::lock_guard l(g_m);
		e = g_emit;
	}
	if (!e)
		return;
	obs_data_t *d = obs_data_create();
	obs_data_set_int(d, "camera_id", camera_id);
	for (auto &[k, v] : fields)
		obs_data_set_string(d, k, v.c_str());
	e(event, d);
	obs_data_release(d);
}

void deliver_video(int camera_id, Feed feed, const AVFrame *fr)
{
	if (fr->format != AV_PIX_FMT_YUV420P && fr->format != AV_PIX_FMT_YUVJ420P)
		return;  // the software H.264 decoder only gives 4:2:0 8-bit for phone streams
	obs_source_frame f = {};
	for (int i = 0; i < 3; ++i) {
		f.data[i] = fr->data[i];
		f.linesize[i] = uint32_t(fr->linesize[i]);
	}
	f.width = uint32_t(fr->width);
	f.height = uint32_t(fr->height);
	f.format = VIDEO_FORMAT_I420;
	f.timestamp = os_gettime_ns();
	f.full_range = fr->color_range == AVCOL_RANGE_JPEG || fr->format == AV_PIX_FMT_YUVJ420P;
	video_format_get_parameters_for_format(fr->colorspace == AVCOL_SPC_BT709 ? VIDEO_CS_709 : VIDEO_CS_601,
	                                       f.full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL, f.format,
	                                       f.color_matrix, f.color_range_min, f.color_range_max);
	std::lock_guard l(g_m);
	for (Src *s : g_srcs)
		if (s->camera_id == camera_id && s->feed == feed)
			obs_source_output_video(s->source, &f);  // copies the frame
}

void deliver_audio(int camera_id, const AVFrame *fr)
{
	obs_source_audio a = {};
	bool planar = av_sample_fmt_is_planar(AVSampleFormat(fr->format));
	if (fr->format != AV_SAMPLE_FMT_FLTP && fr->format != AV_SAMPLE_FMT_FLT)
		return;
	a.data[0] = fr->data[0];
	a.frames = uint32_t(fr->nb_samples);
	a.speakers = fr->ch_layout.nb_channels == 2 ? SPEAKERS_STEREO : SPEAKERS_MONO;
	if (planar && fr->ch_layout.nb_channels == 2)
		a.data[1] = fr->data[1];
	a.format = planar ? AUDIO_FORMAT_FLOAT_PLANAR : AUDIO_FORMAT_FLOAT;
	a.samples_per_sec = uint32_t(fr->sample_rate);
	a.timestamp = os_gettime_ns();
	std::lock_guard l(g_m);
	for (Src *s : g_srcs)
		if (s->camera_id == camera_id && s->feed == Feed::Main)
			obs_source_output_audio(s->source, &a);
}

void clear_video(int camera_id)  // g_m held
{
	for (Src *s : g_srcs)
		if (s->camera_id == camera_id)
			obs_source_output_video(s->source, nullptr);
}

// ── OBS source callbacks ──────────────────────────────
const char *src_name(void *) { return "AirCast Camera"; }

void src_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "camera_id", 0);
	obs_data_set_default_int(settings, "feed", int(Feed::Main));
}

void src_update(void *data, obs_data_t *settings)
{
	auto *s = static_cast<Src *>(data);
	std::lock_guard l(g_m);
	int old = s->camera_id;
	s->camera_id = int(obs_data_get_int(settings, "camera_id"));
	s->feed = Feed(obs_data_get_int(settings, "feed"));
	update_role(old);
	update_role(s->camera_id);
}

void *src_create(obs_data_t *settings, obs_source_t *source)
{
	auto *s = new Src{source, 0, Feed::Main};
	obs_source_set_async_unbuffered(source, true);  // rtp_video already buffers; show frames as they decode
	{
		std::lock_guard l(g_m);
		g_srcs.push_back(s);
	}
	src_update(s, settings);
	return s;
}

void src_destroy(void *data)
{
	auto *s = static_cast<Src *>(data);
	std::lock_guard l(g_m);
	std::erase(g_srcs, s);
	update_role(s->camera_id);
	delete s;
}

void src_active_changed(void *data)
{
	auto *s = static_cast<Src *>(data);
	std::lock_guard l(g_m);
	update_role(s->camera_id);
}

obs_properties_t *src_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *cam = obs_properties_add_list(props, "camera_id", "Phone", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	{
		std::lock_guard l(g_m);
		int current = data ? static_cast<Src *>(data)->camera_id : 0;
		bool listed = false;
		for (auto &[id, c] : g_cams) {
			std::string label = (c.name.empty() ? "Camera" : c.name) + " (#" + std::to_string(id) + ")";
			if (c.state != "connected")
				label += " — " + c.state;
			obs_property_list_add_int(cam, label.c_str(), id);
			listed |= id == current;
		}
		if (current && !listed)  // keep an offline phone selectable instead of blanking the field
			obs_property_list_add_int(cam, ("Camera #" + std::to_string(current) + " — offline").c_str(), current);
	}
	obs_property_t *feed = obs_properties_add_list(props, "feed", "Feed", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(feed, "Camera / phone screen", int(Feed::Main));
	obs_property_list_add_int(feed, "Face camera (game mode)", int(Feed::Face));
	return props;
}
} // namespace

void register_camera_source()
{
	obs_source_info info = {};
	info.id = "aircast_camera";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	info.get_name = src_name;
	info.create = src_create;
	info.destroy = src_destroy;
	info.get_defaults = src_defaults;
	info.get_properties = src_properties;
	info.update = src_update;
	info.activate = src_active_changed;
	info.deactivate = src_active_changed;
	obs_register_source(&info);
}

void set_event_emitter(EventEmitter e)
{
	std::lock_guard l(g_m);
	g_emit = std::move(e);
}

std::string accept_offer(int camera_id, const std::string &name, const std::string &session, const std::string &sdp)
{
	Role role;
	{
		std::lock_guard l(g_m);
		role = role_for(camera_id);
	}
	PeerSink sink{
		[camera_id](Feed f, const AVFrame *fr) { deliver_video(camera_id, f, fr); },
		[camera_id](const AVFrame *fr) { deliver_audio(camera_id, fr); },
		[camera_id](const std::string &ch, const std::string &json) {
			emit("message", camera_id, {{"channel", ch}, {"json", json}});
		},
		[camera_id, session](const std::string &st) {
			{
				std::lock_guard l(g_m);
				auto it = g_cams.find(camera_id);
				if (it != g_cams.end() && it->second.peer && it->second.peer->session() == session) {
					it->second.state = st;
					if (st == "closed")
						clear_video(camera_id);
				}
			}
			emit("state", camera_id, {{"session", session}, {"state", st}});
		},
	};
	// Built outside g_m: ICE gathering takes up to 3 s and the sink needs g_m meanwhile.
	auto peer = std::make_shared<Peer>(camera_id, session, sdp, role, std::move(sink));
	std::string answer = peer->answer();

	std::shared_ptr<Peer> old;
	{
		std::lock_guard l(g_m);
		Camera &c = g_cams[camera_id];
		old = std::move(c.peer);  // a new offer replaces the old session (phone reconnected)
		c.peer = peer;
		c.state = "connecting";
		if (!name.empty())
			c.name = name;
		update_role(camera_id);  // a source may have gone live while we gathered
	}
	old.reset();  // closes outside g_m
	return answer;
}

void close_camera(int camera_id, const std::string &session)
{
	std::shared_ptr<Peer> old;
	{
		std::lock_guard l(g_m);
		auto it = g_cams.find(camera_id);
		if (it == g_cams.end() || !it->second.peer || (!session.empty() && it->second.peer->session() != session))
			return;
		old = std::move(it->second.peer);
		it->second.state = "closed";
		clear_video(camera_id);
	}
	old.reset();
	emit("state", camera_id, {{"session", session}, {"state", "closed"}});
}

bool send_to_camera(int camera_id, const std::string &json)
{
	std::shared_ptr<Peer> p;
	{
		std::lock_guard l(g_m);
		auto it = g_cams.find(camera_id);
		if (it == g_cams.end() || !it->second.peer)
			return false;
		p = it->second.peer;
	}
	p->send_control(json);
	return true;
}

void list_cameras(obs_data_t *response)
{
	obs_data_array_t *arr = obs_data_array_create();
	{
		std::lock_guard l(g_m);
		for (auto &[id, c] : g_cams) {
			obs_data_t *d = obs_data_create();
			obs_data_set_int(d, "camera_id", id);
			obs_data_set_string(d, "name", c.name.c_str());
			obs_data_set_string(d, "state", c.state.c_str());
			obs_data_set_string(d, "session", c.peer ? c.peer->session().c_str() : "");
			obs_data_set_string(d, "role", role_for(id) == Role::Program ? "program" : "preview");
			obs_data_array_push_back(arr, d);
			obs_data_release(d);
		}
	}
	obs_data_set_array(response, "cameras", arr);
	obs_data_array_release(arr);
}

void shutdown_cameras()
{
	std::map<int, Camera> cams;
	{
		std::lock_guard l(g_m);
		g_emit = nullptr;
		cams.swap(g_cams);
	}
	cams.clear();  // closes every peer outside g_m
}

} // namespace aircast
