// End to end over localhost: a libdatachannel "phone" offers H.264 + a control channel, our Peer
// answers. One packet is dropped: the studio must NACK exactly that seq, and after the resend
// every frame decodes in order. set_quality must arrive on `control`.
#include "h264_source.h"
#include "peer.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

using namespace aircast;
using namespace std::chrono_literals;

constexpr int FRAMES = 30;
constexpr uint32_t SSRC = 0x1234;

struct Flag {
	std::mutex m;
	std::condition_variable cv;
	template <class F> bool wait(F pred, std::chrono::milliseconds t)
	{
		std::unique_lock l(m);
		return cv.wait_for(l, t, pred);
	}
	template <class F> void set(F fn)
	{
		{
			std::lock_guard l(m);
			fn();
		}
		cv.notify_all();
	}
};

int main()
{
	auto packets = test::encode_gradient(FRAMES, 1000);
	if (packets.empty()) {
		std::puts("peer: no H.264 encoder, skipping");
		return 77;
	}
	rtc::InitLogger(rtc::LogLevel::Warning);
	Flag f;
	bool gathered = false, track_open = false, got_quality = false;
	std::set<uint16_t> nacked;
	int pli = 0;

	// ── the "phone" ──
	auto phone = std::make_shared<rtc::PeerConnection>(rtc::Configuration{});
	rtc::Description::Video media("0", rtc::Description::Direction::SendOnly);
	media.addH264Codec(102);
	media.addSSRC(SSRC, "cam", "camera", "t1");
	auto track = phone->addTrack(media);
	auto control = phone->createDataChannel("control");
	control->onMessage(nullptr, [&](rtc::string m) {
		if (m.find("\"set_quality\"") != std::string::npos && m.find("\"program\"") != std::string::npos)
			f.set([&] { got_quality = true; });
	});
	track->onOpen([&] { f.set([&] { track_open = true; }); });
	track->onMessage(
		[&](rtc::binary b) {  // RTCP from the studio, possibly compound
			auto *d = reinterpret_cast<const uint8_t *>(b.data());
			for (size_t off = 0; off + 4 <= b.size();) {
				size_t len = (size_t(d[off + 2] << 8 | d[off + 3]) + 1) * 4;
				uint8_t fmt = d[off] & 0x1F, pt = d[off + 1];
				if (pt == 205 && fmt == 1)
					for (size_t i = off + 12; i + 4 <= off + len && i + 4 <= b.size(); i += 4) {
						uint16_t pid = uint16_t(d[i] << 8 | d[i + 1]), blp = uint16_t(d[i + 2] << 8 | d[i + 3]);
						f.set([&] {
							nacked.insert(pid);
							for (int k = 0; k < 16; ++k)
								if (blp >> k & 1)
									nacked.insert(uint16_t(pid + k + 1));
						});
					}
				if (pt == 206 && fmt == 1)
					f.set([&] { ++pli; });
				off += len;
			}
		},
		nullptr);
	phone->onGatheringStateChange([&](rtc::PeerConnection::GatheringState s) {
		if (s == rtc::PeerConnection::GatheringState::Complete)
			f.set([&] { gathered = true; });
	});
	phone->setLocalDescription();
	CHECK(f.wait([&] { return gathered; }, 5s));
	std::string offer = std::string(*phone->localDescription());

	// ── the studio ──
	int decoded = 0;
	bool in_order = true;
	std::vector<std::string> states;
	PeerSink sink{
		[&](Feed feed, const AVFrame *fr) {
			f.set([&] {
				in_order &= feed == Feed::Main && test::picture_matches(fr, decoded);
				++decoded;
			});
		},
		[](const AVFrame *) {},
		[](const std::string &, const std::string &) {},
		[&](const std::string &s) { f.set([&] { states.push_back(s); }); },
	};
	auto studio = std::make_unique<Peer>(7, "s1", offer, Role::Program, sink);
	std::string answer = studio->answer();
	CHECK(answer.find("a=fingerprint") != std::string::npos);
	CHECK(answer.find("H264") != std::string::npos);
	phone->setRemoteDescription(rtc::Description(answer, "answer"));
	CHECK(f.wait([&] { return track_open; }, 5s));
	CHECK(f.wait([&] { return got_quality; }, 3s));

	// Drop one packet in the middle of frame ~5, let the studio notice, then resend it.
	size_t drop = 0;
	for (size_t i = 0, frame = 0; i < packets.size(); ++i) {
		if (frame == 5 && !packets[i].marker) {
			drop = i;
			break;
		}
		frame += packets[i].marker;
	}
	auto send = [&](size_t i) {
		auto b = test::rtp_bytes(packets[i], SSRC);
		track->send(reinterpret_cast<const std::byte *>(b.data()), b.size());
	};
	for (size_t i = 0; i <= drop + 3; ++i)
		if (i != drop)
			send(i), std::this_thread::sleep_for(1ms);
	CHECK(f.wait([&] { return nacked.count(packets[drop].seq) > 0; }, 2s));
	send(drop);
	for (size_t i = drop + 4; i < packets.size(); ++i)
		send(i), std::this_thread::sleep_for(1ms);

	CHECK(f.wait([&] { return decoded == FRAMES; }, 3s));
	{
		std::lock_guard l(f.m);
		CHECK(in_order);
		CHECK(nacked.size() == 1);  // only the dropped packet
		CHECK(pli == 0);           // recovered by NACK, no keyframe needed
		CHECK(!states.empty() && states.front() == "connecting");
		CHECK(std::find(states.begin(), states.end(), "connected") != states.end());
		if (failures)
			std::fprintf(stderr, "decoded %d/%d, nacked %zu, pli %d\n", decoded, FRAMES, nacked.size(), pli);
	}

	studio.reset();
	phone->close();
	if (failures)
		std::fprintf(stderr, "%d check(s) failed\n", failures);
	else
		std::printf("peer: all checks passed (%d frames, NACK seq %u recovered)\n", FRAMES, packets[drop].seq);
	return failures ? 1 : 0;
}
