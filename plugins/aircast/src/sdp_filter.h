#pragma once
#include <map>
#include <string>

namespace aircast {

// Rewrites the phone's offer before we answer it (03-protocol §2 SDP rules):
// - video keeps only H264 packetization-mode=1, audio keeps only opus;
// - drops rtx/red/ulpfec/flexfec, so NACKed packets come back in-band on the same SSRC
//   (rtp_video's jitter buffer expects that);
// - drops transport-cc, so the phone's bandwidth estimate follows our REMB per role;
// - drops video-orientation (CVO), so the phone rotates pixels itself instead of tagging packets.
std::string filter_offer_sdp(const std::string &sdp);

// mid -> msid stream id ("screen" / "camera", 03-protocol §2.1) for each media section.
std::map<std::string, std::string> stream_ids_by_mid(const std::string &sdp);

} // namespace aircast
