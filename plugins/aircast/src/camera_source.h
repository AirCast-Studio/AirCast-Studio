// "AirCast Camera" OBS source + the camera registry it reads from (04-studio-desktop §2.2 `camera_source`).
// A phone is Program while any of its sources is on the program output (OBS activate/deactivate),
// otherwise Preview; the registry sends set_quality + REMB on each change.
#pragma once
#include <functional>
#include <string>

struct obs_data;
typedef struct obs_data obs_data_t;

namespace aircast {

void register_camera_source();
void shutdown_cameras();

// Called from obs-websocket vendor requests (plugin-main.cpp).
using EventEmitter = std::function<void(const char *event, obs_data_t *data)>;
void set_event_emitter(EventEmitter emit);
std::string accept_offer(int camera_id, const std::string &name, const std::string &session, const std::string &sdp);
void close_camera(int camera_id, const std::string &session);  // empty session = whatever is open
bool send_to_camera(int camera_id, const std::string &json);
void list_cameras(obs_data_t *response);

} // namespace aircast
