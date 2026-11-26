// Video receive path (04-studio-desktop §2.2 `rtp_video`): RTP parse, H.264 depacketiser,
// jitter buffer with reordering, NACK list and keyframe (PLI) requests.
// libdatachannel only gives us raw RTP, so all of this is ours (§2.4).
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace aircast {

struct RtpPacket {
	uint16_t seq = 0;
	uint32_t timestamp = 0;
	uint32_t ssrc = 0;
	uint8_t payload_type = 0;
	bool marker = false;
	std::vector<uint8_t> payload;
};

// RFC 3550. nullopt for anything that is not a valid RTP v2 packet (including muxed RTCP).
std::optional<RtpPacket> parse_rtp(const uint8_t *data, size_t len);

// RFC 4585 generic NACK (PT 205, FMT 1); seqs packed into PID + 16-bit BLP. Empty if seqs is.
std::vector<uint8_t> build_nack(uint32_t sender_ssrc, uint32_t media_ssrc, const std::vector<uint16_t> &seqs);

struct H264Frame {
	uint32_t timestamp = 0;
	bool keyframe = false;        // contains an IDR slice
	std::vector<uint8_t> annexb;  // 00 00 00 01 + NAL, ready for the decoder
};

// RFC 6184 packetization-mode=1 (single NAL, STAP-A, FU-A), packets of one frame in seq order.
// False when the frame is broken (missing FU-A start/end, truncated STAP-A, unsupported type).
bool depacketize_h264(const std::vector<const RtpPacket *> &packets, H264Frame &out);

class VideoJitterBuffer {
public:
	struct Config {
		int max_delay_ms = 150;   // give up on a hole after this (plan: 60 LAN / 150 internet)
		int nack_interval_ms = 40;
		int max_nack_tries = 3;
		size_t max_packets = 2048;
	};

	VideoJitterBuffer() : VideoJitterBuffer(Config{}) {}
	explicit VideoJitterBuffer(Config cfg) : cfg_(cfg) {}

	void push(RtpPacket pkt, int64_t now_ms);
	// Next decodable frame in order, or nullopt if we must wait. Call until nullopt.
	std::optional<H264Frame> pop(int64_t now_ms);
	// Sequence numbers to NACK now (RTCP generic NACK).
	std::vector<uint16_t> take_nacks(int64_t now_ms);
	// True once per loss event: send RTCP PLI (or `request_keyframe` on control).
	bool take_keyframe_request();

	void set_max_delay_ms(int ms) { cfg_.max_delay_ms = ms; }

private:
	struct Entry {
		RtpPacket pkt;
		int64_t arrival_ms;
	};
	struct Nack {
		int64_t last_ms;
		int tries;
	};

	int64_t unwrap(uint16_t seq) const;
	bool skip_to_next_frame();

	Config cfg_;
	std::map<int64_t, Entry> packets_;  // key: unwrapped seq
	std::map<int64_t, Nack> missing_;
	std::optional<int64_t> next_;       // next unwrapped seq to deliver
	int64_t highest_ = 0;
	bool need_keyframe_ = true;         // a decoder can only start (or resume after loss) on IDR
	bool delivered_ = false;
	bool pli_ = false;
};

} // namespace aircast
