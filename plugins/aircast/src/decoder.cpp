#include "decoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace aircast {

// shortcut: software H.264 decode with slice threads; plan wants NVDEC/VAAPI/D3D11VA/VideoToolbox
// via hw_device_ctx. Add it when the M1 gate measures CPU with 4 phones.
Decoder::Decoder(Codec codec)
{
	const AVCodec *c = avcodec_find_decoder(codec == Codec::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_OPUS);
	if (!c)
		return;
	ctx_ = avcodec_alloc_context3(c);
	if (codec == Codec::H264) {
		ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
		ctx_->thread_type = FF_THREAD_SLICE;  // frame threads add a frame of latency per thread
		ctx_->thread_count = 0;
	} else {
		ctx_->sample_rate = 48000;  // WebRTC Opus is always 48 kHz; the phone sends mono (03-protocol §2)
		av_channel_layout_default(&ctx_->ch_layout, 1);
	}
	if (avcodec_open2(ctx_, c, nullptr) < 0) {
		avcodec_free_context(&ctx_);
		return;
	}
	frame_ = av_frame_alloc();
	pkt_ = av_packet_alloc();
}

Decoder::~Decoder()
{
	av_packet_free(&pkt_);
	av_frame_free(&frame_);
	avcodec_free_context(&ctx_);
}

bool Decoder::decode(const uint8_t *data, size_t len, const std::function<void(const AVFrame *)> &on_frame)
{
	if (!ctx_)
		return false;
	pkt_->data = const_cast<uint8_t *>(data);  // no buf: avcodec copies what it keeps
	pkt_->size = int(len);
	int r = avcodec_send_packet(ctx_, pkt_);
	av_packet_unref(pkt_);
	if (r < 0 && r != AVERROR(EAGAIN))
		return false;

	bool ok = true;
	while ((r = avcodec_receive_frame(ctx_, frame_)) >= 0) {
		if (frame_->decode_error_flags || (frame_->flags & AV_FRAME_FLAG_CORRUPT))
			ok = false;  // concealed picture: still show it, but ask for a keyframe
		on_frame(frame_);
		av_frame_unref(frame_);
	}
	return ok && (r == AVERROR(EAGAIN) || r == AVERROR_EOF);
}

} // namespace aircast
