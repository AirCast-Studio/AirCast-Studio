// FFmpeg decode (04-studio-desktop §2.2 `decoder`, `rtp_audio`).
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace aircast {

class Decoder {
public:
	enum class Codec { H264, Opus };

	explicit Decoder(Codec codec);
	~Decoder();
	Decoder(const Decoder &) = delete;
	Decoder &operator=(const Decoder &) = delete;

	// Feeds one Annex-B access unit (H264) or one Opus packet; calls on_frame for each output
	// (YUV420P/J video, or 48 kHz mono float-planar audio) on this thread.
	// False on a decode error: for video the caller should request a keyframe.
	bool decode(const uint8_t *data, size_t len, const std::function<void(const AVFrame *)> &on_frame);
	bool ok() const { return ctx_ != nullptr; }

private:
	AVCodecContext *ctx_ = nullptr;
	AVFrame *frame_ = nullptr;
	AVPacket *pkt_ = nullptr;
};

} // namespace aircast
