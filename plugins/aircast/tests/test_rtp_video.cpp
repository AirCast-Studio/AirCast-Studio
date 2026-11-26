// ctest for rtp_video: parser, H.264 depacketiser, jitter buffer.
#include "rtp_video.h"

#include <cstdio>

using namespace aircast;

static int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

using Bytes = std::vector<uint8_t>;

static RtpPacket pkt(uint16_t seq, uint32_t ts, bool marker, Bytes payload)
{
	RtpPacket p;
	p.seq = seq;
	p.timestamp = ts;
	p.marker = marker;
	p.payload = std::move(payload);
	return p;
}

// Frame helpers. IDR = NAL type 5 (0x65), P slice = type 1 (0x41).
static RtpPacket single(uint16_t seq, uint32_t ts, uint8_t nal) { return pkt(seq, ts, true, {nal, 0xAA, 0xBB}); }
static RtpPacket fu(uint16_t seq, uint32_t ts, uint8_t type, bool start, bool end)
{
	return pkt(seq, ts, end, {uint8_t(0x60 | 28), uint8_t((start ? 0x80 : 0) | (end ? 0x40 : 0) | type), 0xCC});
}

static void test_parse()
{
	Bytes basic = {0x80, 0xE0, 0x12, 0x34, 0, 0, 0x01, 0x00, 0xDE, 0xAD, 0xBE, 0xEF, 0x65, 0x01};
	auto p = parse_rtp(basic.data(), basic.size());
	CHECK(p && p->seq == 0x1234 && p->timestamp == 256 && p->ssrc == 0xDEADBEEF && p->marker && p->payload_type == 96);
	CHECK(p && p->payload == Bytes({0x65, 0x01}));

	// 1 CSRC + 1-word extension + 2 bytes padding
	Bytes full = {0xB1, 0x60, 0, 1, 0, 0, 0, 1, 0, 0, 0, 2, /*csrc*/ 9, 9, 9, 9,
	              /*ext*/ 0xBE, 0xDE, 0, 1, 1, 2, 3, 4, /*payload*/ 0x41, 0x99, /*pad*/ 0, 2};
	p = parse_rtp(full.data(), full.size());
	CHECK(p && !p->marker && p->payload == Bytes({0x41, 0x99}));

	Bytes v1 = basic;
	v1[0] = 0x40;
	CHECK(!parse_rtp(v1.data(), v1.size()));
	Bytes rtcp = {0x80, 200, 0, 6, 0, 0, 0, 0, 0, 0, 0, 0};
	CHECK(!parse_rtp(rtcp.data(), rtcp.size()));
	CHECK(!parse_rtp(basic.data(), 11));
	Bytes badpad = basic;
	badpad[0] |= 0x20;
	badpad.back() = 200;
	CHECK(!parse_rtp(badpad.data(), badpad.size()));
}

static void test_depacketize()
{
	H264Frame f;
	auto a = single(1, 90, 0x41);
	CHECK(depacketize_h264({&a}, f) && !f.keyframe && f.annexb == Bytes({0, 0, 0, 1, 0x41, 0xAA, 0xBB}));

	// STAP-A with SPS (0x67) + PPS (0x68), then an IDR in FU-A over 3 packets
	auto stap = pkt(1, 90, false, {0x78, 0, 2, 0x67, 0x01, 0, 1, 0x68});
	auto f1 = fu(2, 90, 5, true, false), f2 = fu(3, 90, 5, false, false), f3 = fu(4, 90, 5, false, true);
	CHECK(depacketize_h264({&stap, &f1, &f2, &f3}, f) && f.keyframe);
	CHECK(f.annexb == Bytes({0, 0, 0, 1, 0x67, 0x01, 0, 0, 0, 1, 0x68, 0, 0, 0, 1, 0x65, 0xCC, 0xCC, 0xCC}));

	CHECK(!depacketize_h264({&f2, &f3}, f));        // no FU-A start
	CHECK(!depacketize_h264({&f1, &f2}, f));        // no FU-A end
	auto bad_stap = pkt(1, 90, true, {0x78, 0, 9, 0x67});
	CHECK(!depacketize_h264({&bad_stap}, f));       // truncated
	auto stapb = pkt(1, 90, true, {0x79, 0, 0});
	CHECK(!depacketize_h264({&stapb}, f));          // mode-2 type
}

static void test_build_nack()
{
	CHECK(build_nack(1, 2, {}).empty());
	// 100, 101, 116 share one FCI (BLP bits 0 and 15); 117 is 17 away -> second FCI; wraps too
	auto n = build_nack(0x01020304, 0xAABBCCDD, {100, 101, 116, 117, 65535, 0});
	CHECK(n == Bytes({0x81, 205, 0, 5, 1, 2, 3, 4, 0xAA, 0xBB, 0xCC, 0xDD,
	                  0, 100, 0x80, 0x01, 0, 117, 0, 0, 0xFF, 0xFF, 0, 1}));
}

static void test_in_order_and_reorder()
{
	VideoJitterBuffer jb;
	jb.push(fu(10, 1000, 5, true, false), 0);
	jb.push(fu(12, 1000, 5, false, true), 0);  // 11 arrives late
	CHECK(!jb.pop(0));
	auto n = jb.take_nacks(0);
	CHECK(n.size() == 1 && n[0] == 11);
	jb.push(fu(11, 1000, 5, false, false), 5);
	auto fr = jb.pop(5);
	CHECK(fr && fr->keyframe && fr->timestamp == 1000);
	jb.push(single(13, 4000, 0x41), 6);
	fr = jb.pop(6);
	CHECK(fr && !fr->keyframe);
	CHECK(!jb.pop(6));
	CHECK(!jb.take_keyframe_request());
	jb.push(single(12, 1000, 0x41), 7);  // late duplicate of a delivered seq
	CHECK(!jb.pop(7));
}

static void test_seq_wrap()
{
	VideoJitterBuffer jb;
	jb.push(fu(65535, 1, 5, true, false), 0);
	jb.push(fu(0, 1, 5, false, true), 0);
	jb.push(single(1, 2, 0x41), 0);
	auto a = jb.pop(0), b = jb.pop(0);
	CHECK(a && a->keyframe && b && b->timestamp == 2);
}

static void test_first_packets_reordered()
{
	VideoJitterBuffer jb;
	jb.push(fu(101, 1, 5, false, true), 0);  // arrives before the frame's first packet
	jb.push(fu(100, 1, 5, true, false), 1);
	auto fr = jb.pop(1);
	CHECK(fr && fr->keyframe);
	CHECK(!jb.take_keyframe_request());
}

static void test_nack_retry_limit()
{
	VideoJitterBuffer jb({.max_delay_ms = 1000, .nack_interval_ms = 40, .max_nack_tries = 2});
	jb.push(single(1, 1, 0x65), 0);
	jb.push(single(3, 3, 0x41), 0);
	CHECK(jb.pop(0));                       // frame 1
	CHECK(jb.take_nacks(0) == std::vector<uint16_t>{2});
	CHECK(jb.take_nacks(10).empty());       // within interval
	CHECK(jb.take_nacks(40) == std::vector<uint16_t>{2});
	CHECK(jb.take_nacks(200).empty());      // tries used up
}

static void test_loss_skips_and_waits_for_keyframe()
{
	VideoJitterBuffer jb({.max_delay_ms = 100});
	jb.push(single(1, 1, 0x65), 0);
	CHECK(jb.pop(0));
	// seq 2 lost for good; 3 and 4 are P frames that can't decode without it
	jb.push(single(3, 3, 0x41), 10);
	jb.push(single(4, 4, 0x41), 10);
	CHECK(!jb.pop(50));                     // still waiting for a retransmit
	CHECK(!jb.pop(200));                    // gave up: skipped, P frames dropped
	CHECK(jb.take_keyframe_request());
	CHECK(!jb.take_keyframe_request());     // once per event
	jb.push(single(5, 5, 0x41), 210);
	CHECK(!jb.pop(210));                    // still not a keyframe
	jb.push(single(6, 6, 0x65), 220);
	auto fr = jb.pop(220);
	CHECK(fr && fr->keyframe && fr->timestamp == 6);
	jb.push(single(7, 7, 0x41), 230);
	CHECK(jb.pop(230));                     // P frames flow again
}

static void test_starts_mid_stream()
{
	VideoJitterBuffer jb;
	jb.push(fu(50, 1, 5, false, true), 0);  // tail of a frame we never saw the start of
	CHECK(!jb.pop(0));
	CHECK(jb.take_keyframe_request());
	jb.push(single(51, 2, 0x65), 1);
	CHECK(jb.pop(1));
}

int main()
{
	test_parse();
	test_depacketize();
	test_build_nack();
	test_in_order_and_reorder();
	test_seq_wrap();
	test_first_packets_reordered();
	test_nack_retry_limit();
	test_loss_skips_and_waits_for_keyframe();
	test_starts_mid_stream();
	if (failures)
		std::fprintf(stderr, "%d check(s) failed\n", failures);
	else
		std::puts("rtp_video: all checks passed");
	return failures ? 1 : 0;
}
