// Loopback: encode real H.264 → RTP (single NAL + FU-A) → shuffle → jitter buffer → decode.
#include "decoder.h"
#include "h264_source.h"

#include <random>

using namespace aircast;

constexpr int FRAMES = 12;

int main()
{
	auto packets = test::encode_gradient(FRAMES, 65530);  // crosses the seq wrap too
	if (packets.empty()) {
		std::puts("decoder: no H.264 encoder in this FFmpeg build, skipping");
		return 77;
	}
	CHECK(packets.size() > size_t(FRAMES));

	// Network: local reordering within small windows.
	std::mt19937 rng(42);
	for (size_t i = 0; i + 4 <= packets.size(); i += 4)
		std::shuffle(packets.begin() + i, packets.begin() + i + 4, rng);

	VideoJitterBuffer jb;
	Decoder dec(Decoder::Codec::H264);
	CHECK(dec.ok());
	int decoded = 0, keyframes = 0;
	bool all_ok = true;
	int64_t now = 0;
	for (auto &p : packets) {
		jb.push(p, now);
		while (auto fr = jb.pop(now)) {
			keyframes += fr->keyframe;
			all_ok &= dec.decode(fr->annexb.data(), fr->annexb.size(), [&](const AVFrame *out) {
				CHECK(test::picture_matches(out, decoded));  // right picture, in order
				++decoded;
			});
		}
		now += 1;
	}
	CHECK(all_ok);
	CHECK(keyframes >= 1);  // x264 may add IDRs on its own
	CHECK(decoded == FRAMES);
	CHECK(!jb.take_keyframe_request());

	// Opus: a 20 ms packet from libopus decodes to 960 mono float samples.
	if (const AVCodec *oe = avcodec_find_encoder_by_name("libopus")) {
		AVCodecContext *oc = avcodec_alloc_context3(oe);
		oc->sample_rate = 48000;
		oc->sample_fmt = AV_SAMPLE_FMT_FLT;
		av_channel_layout_default(&oc->ch_layout, 1);
		oc->bit_rate = 32000;
		CHECK(avcodec_open2(oc, oe, nullptr) == 0);
		AVFrame *af = av_frame_alloc();
		af->format = oc->sample_fmt;
		af->nb_samples = oc->frame_size;
		af->sample_rate = 48000;
		av_channel_layout_copy(&af->ch_layout, &oc->ch_layout);
		av_frame_get_buffer(af, 0);
		Decoder adec(Decoder::Codec::Opus);
		CHECK(adec.ok());
		AVPacket *ap = av_packet_alloc();
		int samples = 0;
		for (int i = 0; i < 5; ++i) {
			av_frame_make_writable(af);
			auto *s = reinterpret_cast<float *>(af->data[0]);
			for (int n = 0; n < af->nb_samples; ++n)
				s[n] = 0.3f * float((n % 48) < 24 ? 1 : -1);
			af->pts = i * af->nb_samples;
			avcodec_send_frame(oc, af);
			while (avcodec_receive_packet(oc, ap) == 0) {
				CHECK(adec.decode(ap->data, ap->size, [&](const AVFrame *out) {
					CHECK(out->sample_rate == 48000 && out->ch_layout.nb_channels == 1);
					CHECK(out->format == AV_SAMPLE_FMT_FLTP || out->format == AV_SAMPLE_FMT_FLT);
					samples += out->nb_samples;
				}));
				av_packet_unref(ap);
			}
		}
		CHECK(samples >= 3 * 960);
		av_packet_free(&ap);
		av_frame_free(&af);
		avcodec_free_context(&oc);
	} else {
		std::puts("decoder: no libopus encoder, Opus part skipped");
	}

	if (failures)
		std::fprintf(stderr, "%d check(s) failed (decoded %d)\n", failures, decoded);
	else
		std::printf("decoder: all checks passed (%d frames, %zu packets)\n", decoded, packets.size());
	return failures ? 1 : 0;
}
