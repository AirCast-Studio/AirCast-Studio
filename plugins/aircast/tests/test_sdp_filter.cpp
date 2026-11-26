#include "sdp_filter.h"

#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(c) \
	do { \
		if (!(c)) { \
			std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
			++failures; \
		} \
	} while (0)

static bool has(const std::string &s, const char *x) { return s.find(x) != std::string::npos; }

// Shape of a flutter_webrtc (libwebrtc) Android offer, trimmed.
static const char *kOffer =
	"v=0\r\n"
	"o=- 1 2 IN IP4 127.0.0.1\r\n"
	"s=-\r\n"
	"a=group:BUNDLE 0 1 2\r\n"
	"m=video 9 UDP/TLS/RTP/SAVPF 96 97 102 103 127 125\r\n"
	"a=mid:0\r\n"
	"a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01\r\n"
	"a=extmap:4 urn:3gpp:video-orientation\r\n"
	"a=rtpmap:96 VP8/90000\r\n"
	"a=rtcp-fb:96 nack\r\n"
	"a=rtpmap:97 rtx/90000\r\n"
	"a=fmtp:97 apt=96\r\n"
	"a=rtpmap:102 H264/90000\r\n"
	"a=rtcp-fb:102 goog-remb\r\n"
	"a=rtcp-fb:102 transport-cc\r\n"
	"a=rtcp-fb:102 nack\r\n"
	"a=rtcp-fb:102 nack pli\r\n"
	"a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f\r\n"
	"a=rtpmap:103 rtx/90000\r\n"
	"a=fmtp:103 apt=102\r\n"
	"a=rtpmap:127 H264/90000\r\n"
	"a=fmtp:127 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42e01f\r\n"
	"a=rtpmap:125 red/90000\r\n"
	"a=ssrc-group:FID 1111 2222\r\n"
	"m=audio 9 UDP/TLS/RTP/SAVPF 111 63 9 126\r\n"
	"a=mid:1\r\n"
	"a=rtpmap:111 opus/48000/2\r\n"
	"a=rtcp-fb:111 transport-cc\r\n"
	"a=fmtp:111 minptime=10;useinbandfec=1\r\n"
	"a=rtpmap:63 red/48000/2\r\n"
	"a=rtpmap:9 G722/8000\r\n"
	"a=rtpmap:126 telephone-event/8000\r\n"
	"m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
	"a=mid:2\r\n"
	"a=sctp-port:5000\r\n";

int main()
{
	std::string out = aircast::filter_offer_sdp(kOffer);

	CHECK(has(out, "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"));
	CHECK(has(out, "a=rtpmap:102 H264/90000"));
	CHECK(has(out, "a=rtcp-fb:102 nack pli"));
	CHECK(has(out, "a=rtcp-fb:102 goog-remb"));
	CHECK(!has(out, "transport-cc"));
	CHECK(!has(out, "transport-wide-cc"));
	CHECK(!has(out, "urn:3gpp:video-orientation"));
	CHECK(!has(out, "VP8") && !has(out, "rtx") && !has(out, "red/") && !has(out, "apt="));
	CHECK(!has(out, "a=rtpmap:127"));  // packetization-mode=0 dropped
	CHECK(has(out, "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"));
	CHECK(has(out, "useinbandfec=1") && !has(out, "G722") && !has(out, "telephone-event"));
	CHECK(has(out, "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\na=mid:2\r\na=sctp-port:5000\r\n"));
	CHECK(has(out, "a=ssrc-group:FID 1111 2222"));  // harmless; libdatachannel ignores the rtx ssrc
	CHECK(out.rfind("v=0\r\n", 0) == 0);

	// Game mode: two video sections tagged by msid stream id.
	auto ids = aircast::stream_ids_by_mid("v=0\r\nm=video 9 X 1\r\na=mid:0\r\na=msid:screen t1\r\n"
	                                      "m=video 9 X 1\r\na=mid:1\r\na=msid:camera t2\r\n"
	                                      "m=application 9 X\r\na=mid:2\r\n");
	CHECK(ids.size() == 2 && ids["0"] == "screen" && ids["1"] == "camera");

	// An offer with no H264 at all is left alone rather than emptied.
	std::string vp8 = "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=rtpmap:96 VP8/90000\r\n";
	CHECK(aircast::filter_offer_sdp(vp8) == vp8);

	if (failures)
		std::fprintf(stderr, "%d check(s) failed\n", failures);
	else
		std::puts("sdp_filter: all checks passed");
	return failures ? 1 : 0;
}
