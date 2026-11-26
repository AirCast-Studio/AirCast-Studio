// Module entry + obs-websocket vendor "aircast". Docks (and today the Electron app) do signalling
// with the backend and hand offers to us here; this replaces the localhost HTTP API of
// 04-studio-desktop §2.3 with obs-websocket's own authenticated socket.
//
// Requests (CallVendorRequest, vendorName "aircast"):
//   offer   { camera_id, session, sdp, name? } -> { sdp } | { error }
//   close   { camera_id, session? }
//   control { camera_id, json }  (a full 03-protocol envelope, sent on the `control` channel)
//   list    {} -> { cameras: [{ camera_id, name, state, session, role }] }
// Events (VendorEvent): message { camera_id, channel, json } · state { camera_id, session, state }
#include "camera_source.h"

#include <obs-module.h>
#include <obs-websocket-api.h>

#include <exception>

OBS_DECLARE_MODULE()

MODULE_EXPORT const char *obs_module_description(void) { return "AirCast: phones as cameras over WebRTC"; }

namespace {
obs_websocket_vendor g_vendor = nullptr;

void req_offer(obs_data_t *req, obs_data_t *res, void *)
{
	try {
		std::string answer = aircast::accept_offer(int(obs_data_get_int(req, "camera_id")), obs_data_get_string(req, "name"),
		                                           obs_data_get_string(req, "session"), obs_data_get_string(req, "sdp"));
		obs_data_set_string(res, "sdp", answer.c_str());
	} catch (const std::exception &e) {
		blog(LOG_WARNING, "[aircast] offer from camera %lld failed: %s", obs_data_get_int(req, "camera_id"), e.what());
		obs_data_set_string(res, "error", e.what());
	}
}

void req_close(obs_data_t *req, obs_data_t *, void *)
{
	aircast::close_camera(int(obs_data_get_int(req, "camera_id")), obs_data_get_string(req, "session"));
}

void req_control(obs_data_t *req, obs_data_t *res, void *)
{
	bool ok = aircast::send_to_camera(int(obs_data_get_int(req, "camera_id")), obs_data_get_string(req, "json"));
	obs_data_set_bool(res, "ok", ok);
}

void req_list(obs_data_t *, obs_data_t *res, void *) { aircast::list_cameras(res); }
} // namespace

bool obs_module_load(void)
{
	aircast::register_camera_source();
	blog(LOG_INFO, "[aircast] loaded");
	return true;
}

void obs_module_post_load(void)
{
	g_vendor = obs_websocket_register_vendor("aircast");
	if (!g_vendor) {
		blog(LOG_WARNING, "[aircast] obs-websocket not available: cameras can't be paired");
		return;
	}
	obs_websocket_vendor_register_request(g_vendor, "offer", req_offer, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "close", req_close, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "control", req_control, nullptr);
	obs_websocket_vendor_register_request(g_vendor, "list", req_list, nullptr);
	aircast::set_event_emitter([](const char *event, obs_data_t *data) { obs_websocket_vendor_emit_event(g_vendor, event, data); });
}

void obs_module_unload(void)
{
	aircast::shutdown_cameras();
}
