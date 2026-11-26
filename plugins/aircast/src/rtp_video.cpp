#include "rtp_video.h"

#include <algorithm>
#include <iterator>

namespace aircast {

namespace {
constexpr uint8_t kStartCode[] = {0, 0, 0, 1};
constexpr int kMaxMissingTracked = 512;  // bigger holes are not worth NACKing; wait for skip + PLI

uint16_t be16(const uint8_t *p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

void append_nal(H264Frame &out, const uint8_t *nal, size_t len)
{
	out.annexb.insert(out.annexb.end(), kStartCode, kStartCode + 4);
	out.annexb.insert(out.annexb.end(), nal, nal + len);
	if ((nal[0] & 0x1F) == 5)
		out.keyframe = true;
}
} // namespace

std::optional<RtpPacket> parse_rtp(const uint8_t *d, size_t len)
{
	if (len < 12 || (d[0] >> 6) != 2)
		return std::nullopt;
	if (d[1] >= 192 && d[1] <= 223)  // RTCP on a muxed port (RFC 5761)
		return std::nullopt;

	size_t off = 12 + 4 * size_t(d[0] & 0x0F);
	if (d[0] & 0x10) {  // header extension
		if (off + 4 > len)
			return std::nullopt;
		off += 4 + 4 * size_t(be16(d + off + 2));
	}
	size_t end = len;
	if (d[0] & 0x20) {  // padding
		uint8_t pad = d[len - 1];
		if (pad == 0 || pad > end)
			return std::nullopt;
		end -= pad;
	}
	if (off > end)
		return std::nullopt;

	RtpPacket p;
	p.marker = d[1] & 0x80;
	p.payload_type = d[1] & 0x7F;
	p.seq = be16(d + 2);
	p.timestamp = be32(d + 4);
	p.ssrc = be32(d + 8);
	p.payload.assign(d + off, d + end);
	return p;
}

std::vector<uint8_t> build_nack(uint32_t sender_ssrc, uint32_t media_ssrc, const std::vector<uint16_t> &seqs)
{
	std::vector<std::pair<uint16_t, uint16_t>> fci;  // PID, BLP
	for (uint16_t s : seqs) {
		uint16_t d = fci.empty() ? 0 : uint16_t(s - fci.back().first);
		if (!fci.empty() && d >= 1 && d <= 16)
			fci.back().second |= uint16_t(1 << (d - 1));
		else
			fci.push_back({s, 0});
	}
	if (fci.empty())
		return {};
	std::vector<uint8_t> out;
	auto put16 = [&](uint16_t v) { out.push_back(uint8_t(v >> 8)), out.push_back(uint8_t(v)); };
	auto put32 = [&](uint32_t v) { put16(uint16_t(v >> 16)), put16(uint16_t(v)); };
	out.push_back(0x81);  // V=2, FMT=1
	out.push_back(205);   // RTPFB
	put16(uint16_t(2 + fci.size()));  // length in words - 1
	put32(sender_ssrc);
	put32(media_ssrc);
	for (auto [pid, blp] : fci)
		put16(pid), put16(blp);
	return out;
}

bool depacketize_h264(const std::vector<const RtpPacket *> &packets, H264Frame &out)
{
	out = {};
	if (packets.empty())
		return false;
	out.timestamp = packets.front()->timestamp;
	bool in_fu = false;

	for (const RtpPacket *pkt : packets) {
		const auto &p = pkt->payload;
		if (p.empty())
			return false;
		uint8_t type = p[0] & 0x1F;

		if (type >= 1 && type <= 23) {
			if (in_fu)
				return false;
			append_nal(out, p.data(), p.size());
		} else if (type == 24) {  // STAP-A
			if (in_fu)
				return false;
			size_t o = 1;
			while (o < p.size()) {
				if (o + 2 > p.size())
					return false;
				size_t n = be16(&p[o]);
				o += 2;
				if (n == 0 || o + n > p.size())
					return false;
				append_nal(out, &p[o], n);
				o += n;
			}
		} else if (type == 28) {  // FU-A
			if (p.size() < 3)
				return false;
			bool start = p[1] & 0x80, end = p[1] & 0x40;
			if (start) {
				if (in_fu)
					return false;
				uint8_t header = uint8_t((p[0] & 0xE0) | (p[1] & 0x1F));
				append_nal(out, &header, 1);
				in_fu = true;
			} else if (!in_fu) {
				return false;
			}
			out.annexb.insert(out.annexb.end(), p.begin() + 2, p.end());
			if (end)
				in_fu = false;
		} else {
			return false;  // STAP-B/MTAP/FU-B are mode 2 only; we negotiate mode 1
		}
	}
	return !in_fu && !out.annexb.empty();
}

int64_t VideoJitterBuffer::unwrap(uint16_t seq) const
{
	return highest_ + int16_t(uint16_t(seq - uint16_t(highest_)));
}

void VideoJitterBuffer::push(RtpPacket pkt, int64_t now_ms)
{
	if (!next_) {
		highest_ = pkt.seq;
		next_ = highest_;
	}
	int64_t ext = unwrap(pkt.seq);
	if (!delivered_ && ext < *next_)
		next_ = ext;  // the stream's first packets can arrive reordered too
	if (ext < *next_ || packets_.count(ext))
		return;  // too late or duplicate

	if (ext > highest_) {
		for (int64_t s = std::max(highest_ + 1, ext - kMaxMissingTracked); s < ext; ++s)
			missing_.emplace(s, Nack{INT64_MIN / 2, 0});
		highest_ = ext;
	}
	missing_.erase(ext);
	packets_.emplace(ext, Entry{std::move(pkt), now_ms});
}

// Drop everything up to and including the first frame end we hold, so delivery restarts
// on a frame boundary. False when no frame end is buffered yet.
bool VideoJitterBuffer::skip_to_next_frame()
{
	for (auto it = packets_.begin(); it != packets_.end(); ++it) {
		if (!it->second.pkt.marker)
			continue;
		next_ = it->first + 1;
		packets_.erase(packets_.begin(), std::next(it));
		missing_.erase(missing_.begin(), missing_.lower_bound(*next_));
		need_keyframe_ = pli_ = true;
		return true;
	}
	return false;
}

std::optional<H264Frame> VideoJitterBuffer::pop(int64_t now_ms)
{
	while (next_ && !packets_.empty()) {
		// Contiguous run from next_ up to a marker = one complete frame.
		std::vector<const RtpPacket *> run;
		auto it = packets_.find(*next_);
		int64_t s = *next_;
		bool complete = false;
		for (; it != packets_.end() && it->first == s; ++it, ++s) {
			run.push_back(&it->second.pkt);
			if (it->second.pkt.marker) {
				complete = true;
				break;
			}
		}

		if (!complete) {
			bool late = now_ms - packets_.begin()->second.arrival_ms > cfg_.max_delay_ms;
			if ((late || packets_.size() > cfg_.max_packets) && skip_to_next_frame())
				continue;
			if (packets_.size() > cfg_.max_packets) {  // no frame end at all: start over
				packets_.clear();
				missing_.clear();
				next_ = highest_ + 1;
				need_keyframe_ = pli_ = true;
			}
			return std::nullopt;
		}

		H264Frame frame;
		bool ok = depacketize_h264(run, frame);
		next_ = s + 1;
		packets_.erase(packets_.begin(), std::next(it));
		missing_.erase(missing_.begin(), missing_.lower_bound(*next_));

		if (!ok || (need_keyframe_ && !frame.keyframe)) {
			need_keyframe_ = pli_ = true;
			continue;
		}
		need_keyframe_ = false;
		delivered_ = true;
		return frame;
	}
	return std::nullopt;
}

std::vector<uint16_t> VideoJitterBuffer::take_nacks(int64_t now_ms)
{
	std::vector<uint16_t> out;
	for (auto &[ext, n] : missing_) {
		if (n.tries >= cfg_.max_nack_tries || now_ms - n.last_ms < cfg_.nack_interval_ms)
			continue;
		n.last_ms = now_ms;
		++n.tries;
		out.push_back(uint16_t(ext));
	}
	return out;
}

bool VideoJitterBuffer::take_keyframe_request()
{
	bool r = pli_;
	pli_ = false;
	return r;
}

} // namespace aircast
