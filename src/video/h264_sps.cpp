#include "h264_sps.hpp"

#include <fmt/format.h>

namespace ounce
{

namespace
{

class Bits
{
  public:
    explicit Bits(std::vector<uint8_t> data) : data(std::move(data)) {}

    bool ok() const { return !overrun; }
    unsigned bit()
    {
        if (position >= data.size() * 8)
        {
            overrun = true;
            return 0;
        }
        const unsigned value = (data[position / 8] >> (7 - position % 8)) & 1;
        position++;
        return value;
    }
    uint32_t bits(unsigned count)
    {
        uint32_t value = 0;
        while (count--)
            value = (value << 1) | bit();
        return value;
    }
    uint32_t ue() // Exp-Golomb, unsigned
    {
        unsigned zeros = 0;
        while (!bit() && ok() && zeros < 32)
            zeros++;
        return zeros ? ((1u << zeros) - 1 + bits(zeros)) : 0;
    }
    int32_t se()
    {
        const uint32_t k = ue();
        return k & 1 ? int32_t((k + 1) / 2) : -int32_t(k / 2);
    }

  private:
    std::vector<uint8_t> data;
    size_t position = 0;
    bool overrun    = false;
};

// Removes emulation prevention bytes (00 00 03 -> 00 00).
std::vector<uint8_t> rbsp(const std::vector<uint8_t>& nal)
{
    std::vector<uint8_t> out;
    unsigned zeros = 0;
    for (size_t i = 1; i < nal.size(); i++) // skip the NAL header
    {
        if (zeros >= 2 && nal[i] == 3)
        {
            zeros = 0;
            continue;
        }
        zeros = nal[i] ? 0 : zeros + 1;
        out.push_back(nal[i]);
    }
    return out;
}

void skipScalingList(Bits& b, int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size; j++)
    {
        if (next)
            next = (last + b.se() + 256) % 256;
        last = next ? next : last;
    }
}

void skipHrd(Bits& b)
{
    const unsigned count = b.ue() + 1;
    b.bits(8); // bit_rate_scale, cpb_size_scale
    for (unsigned i = 0; i < count && b.ok(); i++)
    {
        b.ue();
        b.ue();
        b.bit();
    }
    b.bits(20); // four 5-bit lengths
}

}

std::optional<SpsInfo> parseSps(const std::vector<uint8_t>& sps)
{
    if (sps.size() < 4 || (sps[0] & 31) != 7)
        return std::nullopt;
    Bits b(rbsp(sps));
    SpsInfo info;
    info.profile = b.bits(8);
    b.bits(8); // constraint flags
    info.level = b.bits(8);
    b.ue(); // seq_parameter_set_id
    static const unsigned high[] = { 100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135 };
    for (unsigned p : high)
        if (info.profile == p)
        {
            if (b.ue() == 3) // chroma_format_idc
                b.bit();
            b.ue();
            b.ue();
            b.bit();
            if (b.bit()) // seq_scaling_matrix_present
                for (int i = 0; i < 8; i++)
                    if (b.bit())
                        skipScalingList(b, i < 6 ? 16 : 64);
            break;
        }
    b.ue(); // log2_max_frame_num
    info.pocType = b.ue();
    if (info.pocType == 0)
        b.ue();
    else if (info.pocType == 1)
    {
        b.bit();
        b.se();
        b.se();
        const unsigned cycle = b.ue();
        for (unsigned i = 0; i < cycle && b.ok(); i++)
            b.se();
    }
    info.refFrames = b.ue();
    b.bit(); // gaps_in_frame_num_allowed
    info.widthMbs       = b.ue() + 1;
    info.heightMapUnits = b.ue() + 1;
    info.frameMbsOnly   = b.bit();
    if (!info.frameMbsOnly)
        b.bit();
    b.bit(); // direct_8x8_inference
    if (b.bit()) // frame_cropping
        for (int i = 0; i < 4; i++)
            b.ue();
    info.vui = b.bit();
    if (info.vui)
    {
        if (b.bit() && b.bits(8) == 255) // aspect_ratio_info
            b.bits(32);
        if (b.bit()) // overscan_info
            b.bit();
        if (b.bit()) // video_signal_type
        {
            b.bits(4);
            if (b.bit())
                b.bits(24);
        }
        if (b.bit()) // chroma_loc_info
        {
            b.ue();
            b.ue();
        }
        if (b.bit()) // timing_info
        {
            b.bits(32);
            b.bits(32);
            b.bit();
        }
        const bool nal = b.bit();
        if (nal)
            skipHrd(b);
        const bool vcl = b.bit();
        if (vcl)
            skipHrd(b);
        if (nal || vcl)
            b.bit(); // low_delay_hrd
        b.bit(); // pic_struct_present
        info.bitstreamRestriction = b.bit();
        if (info.bitstreamRestriction)
        {
            b.bit();
            b.ue();
            b.ue();
            b.ue();
            b.ue();
            info.maxReorderFrames     = b.ue();
            info.maxDecFrameBuffering = b.ue();
        }
    }
    if (!b.ok())
        return std::nullopt;
    return info;
}

namespace
{

class Writer
{
  public:
    void bit(unsigned value)
    {
        current = (current << 1) | (value & 1);
        if (++count == 8)
        {
            bytes.push_back(current);
            current = 0;
            count   = 0;
        }
    }
    void bits(uint32_t value, unsigned n)
    {
        while (n--)
            bit((value >> n) & 1);
    }
    void ue(uint32_t value)
    {
        const uint64_t v = uint64_t(value) + 1;
        unsigned length  = 0;
        while ((v >> length) > 1)
            length++;
        bits(0, length);
        for (int i = length; i >= 0; i--)
            bit((v >> i) & 1);
    }
    void se(int32_t value) { ue(value > 0 ? uint32_t(value) * 2 - 1 : uint32_t(-value) * 2); }
    // rbsp_trailing_bits
    std::vector<uint8_t> finish()
    {
        bit(1);
        while (count)
            bit(0);
        return bytes;
    }

  private:
    std::vector<uint8_t> bytes;
    uint8_t current = 0;
    unsigned count  = 0;
};

// Reads a field and writes it unchanged.
struct Copy
{
    Bits& in;
    Writer& out;
    unsigned bit()
    {
        const unsigned v = in.bit();
        out.bit(v);
        return v;
    }
    uint32_t bits(unsigned n)
    {
        const uint32_t v = in.bits(n);
        out.bits(v, n);
        return v;
    }
    uint32_t ue()
    {
        const uint32_t v = in.ue();
        out.ue(v);
        return v;
    }
    int32_t se()
    {
        const int32_t v = in.se();
        out.se(v);
        return v;
    }
};

void copyScalingList(Copy& c, int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size; j++)
    {
        if (next)
            next = (last + c.se() + 256) % 256;
        last = next ? next : last;
    }
}

void copyHrd(Copy& c)
{
    const unsigned count = c.ue() + 1;
    c.bits(8);
    for (unsigned i = 0; i < count && c.in.ok(); i++)
    {
        c.ue();
        c.ue();
        c.bit();
    }
    c.bits(20);
}

// Adds emulation prevention bytes: 00 00 followed by 00..03 gets a 03.
std::vector<uint8_t> escape(uint8_t header, const std::vector<uint8_t>& payload)
{
    std::vector<uint8_t> out { header };
    unsigned zeros = 0;
    for (uint8_t byte : payload)
    {
        if (zeros >= 2 && byte <= 3)
        {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(byte);
        zeros = byte ? 0 : zeros + 1;
    }
    return out;
}

}

std::optional<std::vector<uint8_t>> limitReferences(const std::vector<uint8_t>& sps, unsigned maxRefs)
{
    if (sps.size() < 4 || (sps[0] & 31) != 7)
        return std::nullopt;
    Bits in(rbsp(sps));
    Writer out;
    Copy c { in, out };
    const unsigned profile = c.bits(8);
    c.bits(8);
    c.bits(8);
    c.ue();
    static const unsigned high[] = { 100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135 };
    for (unsigned p : high)
        if (profile == p)
        {
            if (c.ue() == 3)
                return std::nullopt; // 4:4:4 has 12 scaling lists; not a console stream
            c.ue();
            c.ue();
            c.bit();
            if (c.bit())
                for (int i = 0; i < 8; i++)
                    if (c.bit())
                        copyScalingList(c, i < 6 ? 16 : 64);
            break;
        }
    c.ue();
    const unsigned poc = c.ue();
    if (poc == 0)
        c.ue();
    else if (poc == 1)
    {
        c.bit();
        c.se();
        c.se();
        const unsigned cycle = c.ue();
        for (unsigned i = 0; i < cycle && in.ok(); i++)
            c.se();
    }
    const unsigned refs    = in.ue();
    const unsigned limited = refs < maxRefs ? refs : maxRefs;
    out.ue(limited);
    c.bit();
    c.ue();
    c.ue();
    if (!c.bit())
        c.bit();
    c.bit();
    if (c.bit())
        for (int i = 0; i < 4; i++)
            c.ue();
    if (!c.bit())
        return std::nullopt; // no VUI: rare for console streams; leave it as it is
    if (c.bit() && c.bits(8) == 255)
        c.bits(32);
    if (c.bit())
        c.bit();
    if (c.bit())
    {
        c.bits(4);
        if (c.bit())
            c.bits(24);
    }
    if (c.bit())
    {
        c.ue();
        c.ue();
    }
    if (c.bit())
    {
        c.bits(32);
        c.bits(32);
        c.bit();
    }
    const bool nal = c.bit();
    if (nal)
        copyHrd(c);
    const bool vcl = c.bit();
    if (vcl)
        copyHrd(c);
    if (nal || vcl)
        c.bit();
    c.bit(); // pic_struct_present
    const bool restriction = in.bit();
    out.bit(1);
    if (restriction)
    {
        c.bit();
        c.ue();
        c.ue();
        c.ue();
        c.ue();
        in.ue(); // max_num_reorder_frames
        in.ue(); // max_dec_frame_buffering
    }
    else
    {
        out.bit(1); // motion_vectors_over_pic_boundaries
        out.ue(2);  // max_bytes_per_pic_denom
        out.ue(1);  // max_bits_per_mb_denom
        out.ue(16); // log2_max_mv_length_horizontal
        out.ue(16); // log2_max_mv_length_vertical
    }
    out.ue(0);
    out.ue(limited);
    if (!in.ok())
        return std::nullopt;
    return escape(sps[0], out.finish());
}

std::string describe(const SpsInfo& i)
{
    std::string text = fmt::format("profile {} level {}, {}x{} macroblocks{}, {} reference frame(s), POC type {}", i.profile,
        i.level, i.widthMbs, i.heightMapUnits * (i.frameMbsOnly ? 1 : 2), i.frameMbsOnly ? "" : " (interlaced)", i.refFrames,
        i.pocType);
    if (!i.vui)
        text += ", no VUI";
    else if (!i.bitstreamRestriction)
        text += ", VUI without bitstream restriction (decoder may hold frames for reordering)";
    else
        text += fmt::format(", reorder {} frame(s), decode buffer {} frame(s)", i.maxReorderFrames, i.maxDecFrameBuffering);
    return text;
}

}
