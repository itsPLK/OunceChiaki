// Reads an H.264 sequence parameter set (ITU-T H.264 7.3.2.1.1 and E.1.1),
// for logging what the console's stream asks of the decoder.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ounce
{

struct SpsInfo
{
    unsigned profile = 0, level = 0, refFrames = 0, pocType = 0;
    unsigned widthMbs = 0, heightMapUnits = 0;
    bool frameMbsOnly = false, vui = false, bitstreamRestriction = false;
    unsigned maxReorderFrames = 0, maxDecFrameBuffering = 0;
};

// sps: the NAL unit without its start code (first byte is the NAL header).
std::optional<SpsInfo> parseSps(const std::vector<uint8_t>& sps);
std::string describe(const SpsInfo& info);

// The SPS with max_num_ref_frames and the VUI's max_dec_frame_buffering limited
// to maxRefs (a bitstream restriction is added when the VUI has none), and
// max_num_reorder_frames 0. Everything else is copied bit for bit. Null when
// the SPS can't be rewritten (no VUI, or a syntax this doesn't handle).
std::optional<std::vector<uint8_t>> limitReferences(const std::vector<uint8_t>& sps, unsigned maxRefs);

}
