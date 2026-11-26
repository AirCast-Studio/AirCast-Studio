#include "sdp_filter.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>
#include <vector>

namespace aircast {

namespace {
std::string lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
	return s;
}

bool starts_with(const std::string &s, const std::string &p) { return s.rfind(p, 0) == 0; }

// "a=rtpmap:96 H264/90000" -> "96" for prefix "a=rtpmap:"
std::string pt_of(const std::string &line, const std::string &prefix)
{
	size_t end = line.find_first_of(" \t", prefix.size());
	return line.substr(prefix.size(), end == std::string::npos ? std::string::npos : end - prefix.size());
}

void filter_section(std::vector<std::string> &lines)
{
	if (lines.empty() || !starts_with(lines[0], "m="))
		return;
	std::string kind = lines[0].substr(2, lines[0].find(' ') - 2);
	std::string want = kind == "video" ? "h264" : kind == "audio" ? "opus" : "";
	if (want.empty())
		return;  // application (data channels) untouched

	std::set<std::string> codec_pts, mode1_pts;
	for (auto &l : lines) {
		if (starts_with(l, "a=rtpmap:") && starts_with(lower(l.substr(l.find(' ') + 1)), want + "/"))
			codec_pts.insert(pt_of(l, "a=rtpmap:"));
	}
	for (auto &l : lines) {
		if (starts_with(l, "a=fmtp:") && codec_pts.count(pt_of(l, "a=fmtp:")) && l.find("packetization-mode=1") != std::string::npos)
			mode1_pts.insert(pt_of(l, "a=fmtp:"));
	}
	const std::set<std::string> &keep = kind == "video" && !mode1_pts.empty() ? mode1_pts : codec_pts;
	if (keep.empty())
		return;  // nothing we can decode; leave it so the failure is visible, not silent

	// m=<kind> <port> <proto> <pt...>
	std::istringstream m(lines[0]);
	std::string head, port, proto, pt;
	m >> head >> port >> proto;
	std::string out = head + " " + port + " " + proto;
	while (m >> pt)
		if (keep.count(pt))
			out += " " + pt;
	lines[0] = out;

	std::vector<std::string> kept{lines[0]};
	for (size_t i = 1; i < lines.size(); ++i) {
		const std::string &l = lines[i];
		bool per_pt = false;
		for (const char *p : {"a=rtpmap:", "a=fmtp:", "a=rtcp-fb:"}) {
			if (starts_with(l, p)) {
				std::string id = pt_of(l, p);
				per_pt = true;
				if ((id == "*" || keep.count(id)) && l.find("transport-cc") == std::string::npos)
					kept.push_back(l);
			}
		}
		if (!per_pt && l.find("transport-wide-cc") == std::string::npos && l.find("urn:3gpp:video-orientation") == std::string::npos)
			kept.push_back(l);
	}
	lines = std::move(kept);
}
} // namespace

std::map<std::string, std::string> stream_ids_by_mid(const std::string &sdp)
{
	std::map<std::string, std::string> out;
	std::string mid, msid;
	auto flush = [&] {
		if (!mid.empty() && !msid.empty())
			out[mid] = msid;
		mid.clear();
		msid.clear();
	};
	std::istringstream in(sdp);
	for (std::string line; std::getline(in, line);) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (starts_with(line, "m="))
			flush();
		else if (starts_with(line, "a=mid:"))
			mid = line.substr(6);
		else if (starts_with(line, "a=msid:"))
			msid = line.substr(7, line.find(' ') == std::string::npos ? std::string::npos : line.find(' ') - 7);
	}
	flush();
	return out;
}

std::string filter_offer_sdp(const std::string &sdp)
{
	std::vector<std::vector<std::string>> sections(1);
	std::istringstream in(sdp);
	for (std::string line; std::getline(in, line);) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty())
			continue;
		if (starts_with(line, "m="))
			sections.emplace_back();
		sections.back().push_back(line);
	}
	std::string out;
	for (auto &s : sections) {
		filter_section(s);
		for (auto &l : s)
			out += l + "\r\n";
	}
	return out;
}

} // namespace aircast
