// One phone = one libdatachannel PeerConnection (04-studio-desktop §2.2 `peer_manager`, `bitrate`).
// Video: RTP -> VideoJitterBuffer -> H.264 decode; NACK/PLI/REMB back to the phone.
// Audio: RTP -> Opus decode. Data channels: `control` + `telemetry` (03-protocol §3-4).
#pragma once
#include <functional>
#include <memory>
#include <string>

struct AVFrame;

namespace aircast {

enum class Feed { Main, Face };  // Face = the camera track while the phone is in game mode
enum class Role { Program, Preview };

struct PeerSink {
	std::function<void(Feed, const AVFrame *)> video;  // network thread
	std::function<void(const AVFrame *)> audio;
	std::function<void(const std::string &channel, const std::string &json)> message;  // phone -> studio
	std::function<void(const std::string &state)> state;  // "connecting" | "connected" | "closed"
};

class Peer {
public:
	// Applies the phone's offer and gathers ICE (non-trickle, up to ~3 s). Throws on a bad offer.
	Peer(int camera_id, std::string session, const std::string &offer_sdp, Role role, PeerSink sink);
	~Peer();

	std::string answer() const;
	const std::string &session() const;
	void send_control(const std::string &json);  // a full 03-protocol envelope
	void set_role(Role role);                    // set_quality + REMB, no-op if unchanged
	void close();

private:
	struct Impl;
	std::shared_ptr<Impl> impl_;  // shared with libdatachannel callbacks via weak_ptr
};

} // namespace aircast
