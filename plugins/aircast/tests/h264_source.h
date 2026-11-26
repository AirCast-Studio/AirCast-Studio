// Test helpers: CHECK, and a real H.264 stream (x264 / openh264) packetised like a phone would.
#pragma once
#include "rtp_video.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
}

static int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

namespace test {

constexpr int W = 320, H = 240;

// Split Annex-B into NAL units (start codes removed).
inline std::vector<std::vector<uint8_t>> nals(const uint8_t *d, size_t n)
{
	std::vector<std::vector<uint8_t>> out;
	size_t i = 0, start = SIZE_MAX;
	while (i + 3 <= n) {
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
			if (start != SIZE_MAX) {
				size_t end = i;
				while (end > start && d[end - 1] == 0)
					--end;  // 4-byte start code's leading zero
				out.emplace_back(d + start, d + end);
			}
			start = i += 3;
		} else {
			++i;
		}
	}
	if (start != SIZE_MAX && start < n)
		out.emplace_back(d + start, d + n);
	return out;
}

inline void packetize(const std::vector<std::vector<uint8_t>> &units, uint32_t ts, uint16_t &seq, std::vector<aircast::RtpPacket> &out)
{
	constexpr size_t MTU = 600;  // small, so the IDR needs FU-A
	for (size_t u = 0; u < units.size(); ++u) {
		const auto &nal = units[u];
		bool last_nal = u + 1 == units.size();
		if (nal.size() <= MTU) {
			out.push_back({seq++, ts, 0, 102, last_nal, nal});
			continue;
		}
		for (size_t off = 1; off < nal.size(); off += MTU) {
			size_t len = std::min(MTU, nal.size() - off);
			bool s = off == 1, e = off + len == nal.size();
			aircast::RtpPacket p{seq++, ts, 0, 102, e && last_nal, {}};
			p.payload = {uint8_t((nal[0] & 0xE0) | 28), uint8_t((s ? 0x80 : 0) | (e ? 0x40 : 0) | (nal[0] & 0x1F))};
			p.payload.insert(p.payload.end(), nal.begin() + off, nal.begin() + off + len);
			out.push_back(p);
		}
	}
}

// `frames` of a moving gradient (luma at x = x + i*4), one GOP. Empty if FFmpeg has no H.264 encoder.
inline std::vector<aircast::RtpPacket> encode_gradient(int frames, uint16_t first_seq)
{
	const AVCodec *enc = nullptr;
	for (const char *name : {"libx264", "libopenh264"})
		if ((enc = avcodec_find_encoder_by_name(name)))
			break;
	if (!enc)
		return {};
	AVCodecContext *ec = avcodec_alloc_context3(enc);
	ec->width = W;
	ec->height = H;
	ec->pix_fmt = AV_PIX_FMT_YUV420P;
	ec->time_base = {1, 30};
	ec->gop_size = frames;
	ec->max_b_frames = 0;
	ec->bit_rate = 400000;
	av_opt_set(ec->priv_data, "tune", "zerolatency", 0);
	if (avcodec_open2(ec, enc, nullptr) < 0) {
		avcodec_free_context(&ec);
		return {};
	}
	std::vector<aircast::RtpPacket> packets;
	uint16_t seq = first_seq;
	AVFrame *f = av_frame_alloc();
	f->format = AV_PIX_FMT_YUV420P;
	f->width = W;
	f->height = H;
	av_frame_get_buffer(f, 0);
	AVPacket *pkt = av_packet_alloc();
	auto drain = [&] {
		while (avcodec_receive_packet(ec, pkt) == 0) {
			packetize(nals(pkt->data, pkt->size), uint32_t(pkt->pts * 3000), seq, packets);
			av_packet_unref(pkt);
		}
	};
	for (int i = 0; i < frames; ++i) {
		av_frame_make_writable(f);
		for (int y = 0; y < H; ++y)
			for (int x = 0; x < W; ++x)
				f->data[0][y * f->linesize[0] + x] = uint8_t(x + i * 4);
		for (int p = 1; p < 3; ++p)
			for (int y = 0; y < H / 2; ++y)
				std::fill_n(f->data[p] + y * f->linesize[p], W / 2, 128);
		f->pts = i;
		avcodec_send_frame(ec, f);
		drain();
	}
	avcodec_send_frame(ec, nullptr);
	drain();
	av_packet_free(&pkt);
	av_frame_free(&f);
	avcodec_free_context(&ec);
	return packets;
}

// Luma at (100, 10) of decoded frame i should be about 100 + i*4.
inline bool picture_matches(const AVFrame *out, int i)
{
	int y = out->data[0][10 * out->linesize[0] + 100];
	return out->width == W && out->height == H && std::abs(y - (100 + i * 4)) < 12;
}

inline std::vector<uint8_t> rtp_bytes(const aircast::RtpPacket &p, uint32_t ssrc)
{
	std::vector<uint8_t> b = {0x80, uint8_t((p.marker ? 0x80 : 0) | p.payload_type), uint8_t(p.seq >> 8), uint8_t(p.seq),
	                          uint8_t(p.timestamp >> 24), uint8_t(p.timestamp >> 16), uint8_t(p.timestamp >> 8), uint8_t(p.timestamp),
	                          uint8_t(ssrc >> 24), uint8_t(ssrc >> 16), uint8_t(ssrc >> 8), uint8_t(ssrc)};
	b.insert(b.end(), p.payload.begin(), p.payload.end());
	return b;
}

} // namespace test
