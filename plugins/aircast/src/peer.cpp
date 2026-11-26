#include "peer.h"

#include "decoder.h"
#include "rtp_video.h"
#include "sdp_filter.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <random>
#include <vector>

namespace aircast {

namespace {
int64_t now_ms()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Same numbers the Electron receiver used (src/services/peers.js QUALITY).
const char *quality_json(Role r)
{
	return r == Role::Program ? R"({"role":"program","maxBitrateKbps":6000,"maxWidth":1920,"maxFps":30})"
	                          : R"({"role":"preview","maxBitrateKbps":600,"maxWidth":640,"maxFps":15})";
}
// REMB is the phone's whole-connection estimate, so it sits above set_quality's cap plus the
// 400 kbps game-mode face track; set_quality does the real limiting.
unsigned remb_bps(Role r) { return r == Role::Program ? 8'000'000 : 1'500'000; }

// libdatachannel throws from RTCP sends when the track isn't open (yet, or any more).
void send_remb(rtc::Track &t, Role r)
{
	try {
		if (t.isOpen())
			t.requestBitrate(remb_bps(r));
	} catch (const std::exception &) {
	}
}

std::string envelope(const char *type, const char *payload_json)
{
	static std::mt19937_64 rng{std::random_device{}()};
	static std::mutex m;
	unsigned long long id;
	{
		std::lock_guard l(m);
		id = rng();
	}
	char buf[512];
	std::snprintf(buf, sizeof buf, R"({"v":1,"id":"%016llx","ts":%lld,"type":"%s","payload":%s})", id,
	              (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
	                      std::chrono::system_clock::now().time_since_epoch()).count(),
	              type, payload_json);
	return buf;
}

// Last handler in the chain: keeps libdatachannel's send callback so we can push our own RTCP
// (NACK) as Control messages. A recvonly track rejects anything else.
class RtcpOut : public rtc::MediaHandler {
public:
	void incoming(rtc::message_vector &, const rtc::message_callback &send) override
	{
		std::lock_guard l(m_);
		send_ = send;
	}
	void send(const std::vector<uint8_t> &bytes)
	{
		rtc::message_callback s;
		{
			std::lock_guard l(m_);
			s = send_;
		}
		auto *b = reinterpret_cast<const std::byte *>(bytes.data());
		try {
			if (s && !bytes.empty())
				s(rtc::make_message(b, b + bytes.size(), rtc::Message::Control));
		} catch (const std::exception &) {  // track closing
		}
	}

private:
	std::mutex m_;
	rtc::message_callback send_;
};

struct VideoPipe {
	std::shared_ptr<rtc::Track> track;
	std::shared_ptr<RtcpOut> rtcp = std::make_shared<RtcpOut>();
	Feed feed = Feed::Main;
	std::mutex m;
	VideoJitterBuffer jb;
	Decoder dec{Decoder::Codec::H264};
	bool pli_pending = false;
	int64_t last_pli = 0;
};

struct AudioPipe {
	std::shared_ptr<rtc::Track> track;
	std::mutex m;
	Decoder dec{Decoder::Codec::Opus};
};
} // namespace

struct Peer::Impl {
	int camera_id;
	std::string session;
	PeerSink sink;
	std::shared_ptr<rtc::PeerConnection> pc;
	std::map<std::string, std::string> stream_ids;
	bool game = false;  // offer has a `screen` track

	std::mutex m;
	std::vector<std::shared_ptr<VideoPipe>> video;
	std::vector<std::shared_ptr<AudioPipe>> audio;
	std::vector<std::shared_ptr<rtc::DataChannel>> channels;
	std::shared_ptr<rtc::DataChannel> control;
	Role role = Role::Preview;
	std::condition_variable gathered_cv;
	bool gathered = false;
	std::atomic<bool> closed{false};

	void on_video_rtp(VideoPipe &p, const rtc::binary &data)
	{
		auto pkt = parse_rtp(reinterpret_cast<const uint8_t *>(data.data()), data.size());
		if (!pkt)
			return;
		uint32_t ssrc = pkt->ssrc;
		int64_t t = now_ms();
		std::lock_guard l(p.m);
		p.jb.push(std::move(*pkt), t);
		while (auto f = p.jb.pop(t)) {
			if (!p.dec.decode(f->annexb.data(), f->annexb.size(), [&](const AVFrame *fr) { sink.video(p.feed, fr); }))
				p.pli_pending = true;
		}
		if (auto n = p.jb.take_nacks(t); !n.empty())
			p.rtcp->send(build_nack(1, ssrc, n));
		p.pli_pending |= p.jb.take_keyframe_request();
		if (p.pli_pending && t - p.last_pli > 300) {  // a keyframe takes a while to arrive; don't spam
			try {
				p.track->requestKeyframe();
			} catch (const std::exception &) {  // track closing
			}
			p.last_pli = t;
			p.pli_pending = false;
		}
	}

	void on_audio_rtp(AudioPipe &p, const rtc::binary &data)
	{
		// shortcut: no audio jitter buffer or RTCP-SR A/V sync yet (plan §2.2 rtp_audio); OBS
		// timestamps on arrival. Add both when the M1 gate shows audio drift on 4G.
		auto pkt = parse_rtp(reinterpret_cast<const uint8_t *>(data.data()), data.size());
		if (!pkt || pkt->payload.empty())
			return;
		std::lock_guard l(p.m);
		p.dec.decode(pkt->payload.data(), pkt->payload.size(), [&](const AVFrame *fr) { sink.audio(fr); });
	}

	void apply_role()  // with m held
	{
		try {
			if (control && control->isOpen())
				control->send(envelope("set_quality", quality_json(role)));
		} catch (const std::exception &) {  // channel closing; the next open re-sends
		}
		for (auto &v : video)
			send_remb(*v->track, role);
	}
};

Peer::Peer(int camera_id, std::string session, const std::string &offer_sdp, Role role, PeerSink sink)
	: impl_(std::make_shared<Impl>())
{
	auto &I = *impl_;
	I.camera_id = camera_id;
	I.session = std::move(session);
	I.sink = std::move(sink);
	I.role = role;

	std::string sdp = filter_offer_sdp(offer_sdp);
	I.stream_ids = stream_ids_by_mid(sdp);
	for (auto &[mid, id] : I.stream_ids)
		I.game |= id == "screen";

	rtc::Configuration cfg;
	cfg.iceServers.emplace_back("stun:stun.l.google.com:19302");
	cfg.iceServers.emplace_back("stun:stun1.l.google.com:19302");
	I.pc = std::make_shared<rtc::PeerConnection>(cfg);
	std::weak_ptr<Impl> weak = impl_;

	I.pc->onTrack([weak](std::shared_ptr<rtc::Track> track) {
		auto self = weak.lock();
		if (!self)
			return;
		// No lock held while attaching callbacks: onMessage() can flush queued packets synchronously
		// into the sink, which takes the source registry lock (lock order: registry -> peer).
		auto session = std::make_shared<rtc::RtcpReceivingSession>();  // RR, REMB, PLI
		auto type = track->description().type();
		if (type == "video") {
			auto p = std::make_shared<VideoPipe>();
			p->track = track;
			auto it = self->stream_ids.find(track->mid());
			// Game mode: `screen` is the main picture and `camera` is the face (03-protocol §2.1).
			p->feed = self->game && it != self->stream_ids.end() && it->second != "screen" ? Feed::Face : Feed::Main;
			session->addToChain(p->rtcp);
			track->setMediaHandler(session);
			{
				std::lock_guard l(self->m);
				self->video.push_back(p);  // held before packets flow, so set_role reaches it
			}
			std::weak_ptr<VideoPipe> wp = p;
			track->onOpen([weak, wp] {  // libdatachannel throws on RTCP sent before the track opens
				auto s = weak.lock();
				auto pipe = wp.lock();
				if (!s || !pipe)
					return;
				std::lock_guard l(s->m);
				send_remb(*pipe->track, s->role);
			});
			track->onMessage(
				[weak, wp](rtc::binary data) {
					auto s = weak.lock();
					auto pipe = wp.lock();
					if (s && pipe && !s->closed)
						s->on_video_rtp(*pipe, data);
				},
				nullptr);
		} else if (type == "audio") {
			auto p = std::make_shared<AudioPipe>();
			p->track = track;
			track->setMediaHandler(session);
			{
				std::lock_guard l(self->m);
				self->audio.push_back(p);
			}
			std::weak_ptr<AudioPipe> wp = p;
			track->onMessage(
				[weak, wp](rtc::binary data) {
					auto s = weak.lock();
					auto pipe = wp.lock();
					if (s && pipe && !s->closed)
						s->on_audio_rtp(*pipe, data);
				},
				nullptr);
		}
	});

	I.pc->onDataChannel([weak](std::shared_ptr<rtc::DataChannel> dc) {
		auto self = weak.lock();
		if (!self)
			return;
		std::string label = dc->label();
		dc->onMessage(nullptr, [weak, label](rtc::string msg) {
			auto s = weak.lock();
			if (s && !s->closed)
				s->sink.message(label, msg);
		});
		{
			std::lock_guard l(self->m);
			self->channels.push_back(dc);  // libdatachannel closes a channel nobody holds
			if (label == "control")
				self->control = dc;
		}
		// Outside the lock: a stored open event is replayed synchronously inside onOpen().
		if (label == "control")
			dc->onOpen([weak] {
				if (auto s = weak.lock()) {
					std::lock_guard l(s->m);
					s->apply_role();
				}
			});
	});

	I.pc->onStateChange([weak](rtc::PeerConnection::State st) {
		auto s = weak.lock();
		if (!s || s->closed)
			return;
		using S = rtc::PeerConnection::State;
		if (st == S::Connected)
			s->sink.state("connected");
		else if (st == S::Disconnected)
			s->sink.state("reconnecting");
		else if (st == S::Failed || st == S::Closed)
			s->sink.state("closed");
	});

	I.pc->onGatheringStateChange([weak](rtc::PeerConnection::GatheringState st) {
		auto s = weak.lock();
		if (!s || st != rtc::PeerConnection::GatheringState::Complete)
			return;
		std::lock_guard l(s->m);
		s->gathered = true;
		s->gathered_cv.notify_all();
	});

	I.sink.state("connecting");
	I.pc->setRemoteDescription(rtc::Description(sdp, "offer"));  // auto-creates the answer

	// Non-trickle (the phone expects candidates inside the answer). Host/srflx found by then are
	// enough; STUN to a far server can be slow.
	std::unique_lock l(I.m);
	I.gathered_cv.wait_for(l, std::chrono::seconds(3), [&] { return I.gathered; });
}

Peer::~Peer() { close(); }

std::string Peer::answer() const
{
	auto d = impl_->pc->localDescription();
	return d ? std::string(*d) : std::string();
}

const std::string &Peer::session() const { return impl_->session; }

void Peer::send_control(const std::string &json)
{
	std::lock_guard l(impl_->m);
	try {
		if (impl_->control && impl_->control->isOpen())
			impl_->control->send(json);
	} catch (const std::exception &) {
	}
}

void Peer::set_role(Role role)
{
	std::lock_guard l(impl_->m);
	if (impl_->role == role)
		return;
	impl_->role = role;
	impl_->apply_role();
}

void Peer::close()
{
	if (impl_->closed.exchange(true))
		return;
	impl_->pc->close();
}

} // namespace aircast
