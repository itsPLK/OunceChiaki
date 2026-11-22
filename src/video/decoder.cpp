// Adapted from Moonlight-Switch (XITRIX, GPL-3.0): Switch2VideoDecoder.cpp.
#include "decoder.hpp"

#include "h264_sps.hpp"
#include "image_interop.hpp"

#include <algorithm>
#include <borealis.hpp>
#include <chrono>
#include <cstdio>
#include <fmt/format.h>

namespace ounce
{

namespace
{

constexpr size_t MaxSample   = 4 * 1024 * 1024;
constexpr uint64_t PollNs    = 2000000;  // bounded waits keep stop/join responsive
constexpr uint64_t SubmitNs  = 50000000; // a large IDR may need all SDK input chunks
constexpr uint64_t RetryUs   = 1000000;  // after a failed decoder creation
// The console declares 9 reference frames (headroom for loss recovery) while
// the decoder has VIDDEC_MAX_IMAGES (8) output images. It normally references
// only the previous frame, so the decoder is given 4.
constexpr uint64_t StallUs     = 1500000; // no picture this long after opening

// Reference frames declared to the decoder (see limitReferences).
constexpr unsigned DecoderRefs = 4;

bool timedOut(Result rc) { return R_VALUE(rc) == KERNELRESULT(TimedOut); }

struct Nal
{
    uint8_t type;
    size_t start, end; // payload, after the start code
    size_t codeStart;  // where its start code begins
};

// Splits an Annex B buffer at 3- and 4-byte start codes.
std::vector<Nal> nals(const uint8_t* data, size_t size)
{
    std::vector<Nal> out;
    size_t i = 0;
    while (i + 3 <= size)
    {
        if (data[i] == 0 && data[i + 1] == 0 && (data[i + 2] == 1 || (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1)))
        {
            const size_t code = i, payload = i + (data[i + 2] == 1 ? 3 : 4);
            if (!out.empty())
                out.back().end = code;
            if (payload < size)
                out.push_back({ static_cast<uint8_t>(data[payload] & 31), payload, size, code });
            i = payload;
            continue;
        }
        i++;
    }
    return out;
}

void appendWithStartCode(std::vector<uint8_t>& out, const uint8_t* begin, const uint8_t* end)
{
    static const uint8_t code[] = { 0, 0, 0, 1 };
    out.insert(out.end(), code, code + 4);
    out.insert(out.end(), begin, end);
}

}

Decoder::~Decoder()
{
    stop();
    std::lock_guard lock(mutex);
    closeHardware();
}

bool Decoder::setup(int w, int h, int f)
{
    stop();
    std::lock_guard lock(mutex);
    closeHardware();
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || f <= 0)
        return false;
    width = w, height = h, fps = f;
    needIdr = true, firstFrame = false, loggedKeyframe = false;
    outputErrors = 0, lastOutputError = 0;
    sps.clear(), pps.clear(), decoderSps.clear();
    sequence = 0;
    retryAfterUs = 0;
    windowUs     = nowUs();
    output.configure(fps);
    brls::Logger::info("Decoder: hardware H.264 {}x{} at {} fps; waiting for a keyframe", width, height, fps);
    return true;
}

void Decoder::setKeyframeNeeded(std::function<void()> callback)
{
    std::lock_guard lock(mutex);
    keyframeNeeded = std::move(callback);
}

void Decoder::start()
{
    if (!stopped.exchange(false))
        return;
    reader = std::thread(&Decoder::readFrames, this);
}

void Decoder::stop()
{
    stopped.store(true);
    if (reader.joinable())
        reader.join();
    if (opener.joinable())
        opener.join();
}

void Decoder::closeHardware()
{
    generation++; // an open still in progress is discarded
    if (pool && pool->open)
        pool->retireDecoder();
    open = false;
    // The last frame still held by the renderer releases the pool.
    pool.reset();
    pending.clear();
}

// Creates a decoder session for the parameter sets in config. Runs on its own
// thread: it takes a few hundred ms, which must not stall chiaki's receive
// thread (packets then overflow, and loss snowballs).
static std::shared_ptr<DecoderPool> createPool(const std::vector<uint8_t>& config, int width, int height)
{
    struct ImportScope
    {
        ImportScope() { switch2PreimportDecoderImages = true; }
        ~ImportScope() { switch2PreimportDecoderImages = false; }
    } importScope;
    auto pool = std::make_shared<DecoderPool>();
    VideoDecoderConfig options {};
    options.image_count = VIDDEC_MAX_IMAGES;
    options.codec       = ViddecCodec_H264;
    // The coded size: the decoder rejects the console's 1080p stream (68
    // macroblock rows, cropped to 1080) when told 1080 (found 2026-11-22).
    options.width  = (width + 15) & ~15;
    options.height = (height + 15) & ~15;
    // The SDK's defaults: frame rate from the size's budget (60 at 1080p),
    // color policy from the size, max sample width * height * 3 / 2.
    options.frame_rate        = 0;
    options.max_sample_size   = 0;
    options.color_policy      = 0;
    options.codec_config      = config.data();
    options.codec_config_size = config.size();
    const uint64_t begin = nowUs();
    const Result rc      = videoDecoderCreate(&pool->decoder, &options);
    if (R_FAILED(rc))
    {
        brls::Logger::error("Decoder: create failed {}x{} rc=0x{:x}", options.width, options.height, rc);
        pool->decoder = {};
        return nullptr;
    }
    pool->open = true;
    brls::Logger::info("Decoder: hardware decoder open ({}x{} coded, {} images) in {} ms", options.width,
        options.height, pool->decoder.image_count, (nowUs() - begin) / 1000);
    return pool;
}

// With mutex held: opens a decoder in the background for the current SPS/PPS.
void Decoder::startOpen()
{
    if (open || opening || sps.empty() || pps.empty() || nowUs() < retryAfterUs || stopped.load())
        return;
    rebuildDecoderSps();
    // Open at the stream's own coded size. It equals the requested one on PS5
    // (1920x1088 for 1080p); a PS4 without 1080p may send a smaller stream.
    if (const auto info = parseSps(decoderSps); info && info->widthMbs && info->heightMapUnits)
    {
        const int w = int(info->widthMbs * 16), h = int(info->heightMapUnits * (info->frameMbsOnly ? 1 : 2) * 16);
        if (w <= 4096 && h <= 4096 && (w != ((width + 15) & ~15) || h != ((height + 15) & ~15)))
        {
            brls::Logger::info("Decoder: the stream is {}x{} coded, not {}x{}; opening at the stream's size", w, h, width, height);
            width = w, height = h;
        }
    }
    std::vector<uint8_t> config;
    appendWithStartCode(config, decoderSps.data(), decoderSps.data() + decoderSps.size());
    appendWithStartCode(config, pps.data(), pps.data() + pps.size());
    if (opener.joinable())
        opener.join(); // finished: `opening` is false
    opening              = true;
    const unsigned token = generation;
    opener = std::thread([this, config, token, w = width, h = height] {
        auto created = createPool(config, w, h);
        {
            std::lock_guard lock(mutex);
            opening = false;
            if (!created)
                retryAfterUs = nowUs() + RetryUs;
            else if (token != generation || stopped.load())
            {
                created->retireDecoder(); // reset or stopped meanwhile
                created.reset();
            }
            else
            {
                pool             = created;
                open             = true;
                needIdr          = true; // decode from the next keyframe
                openedUs         = nowUs();
                decodedSinceOpen = 0;
            }
        }
        std::function<void()> notify;
        {
            std::lock_guard lock(mutex);
            notify = keyframeNeeded;
        }
        if (created && notify)
            notify();
    });
}

void Decoder::fail(const char* operation, Result rc)
{
    brls::Logger::error("Decoder: {} failed rc=0x{:x}; waiting for a keyframe", operation, rc);
    closeHardware();
    needIdr = true;
}

// Returns decoded pictures to the frame queue. With mutex held.
bool Decoder::drain(uint64_t timeoutNs)
{
    if (!open)
        return true;
    pool->releaseFrames();
    for (unsigned count = 0; count < VIDEO_DECODER_DEFAULT_IMAGES; ++count)
    {
        VideoFrame frame {};
        const Result rc = videoDecoderGetFrame(&pool->decoder, &frame, count ? 0 : timeoutNs);
        if (timedOut(rc))
            return true;
        if (R_FAILED(rc))
        {
            // Not fatal by itself; a decoder that produces nothing is caught
            // by the first-picture timeout and the pending-sample limit.
            outputErrors++;
            if (rc != lastOutputError || outputErrors == 1 || outputErrors % 120 == 0)
                brls::Logger::warning("Decoder: output check rc=0x{:x} ({} so far, {} samples pending)", rc,
                    outputErrors, pending.size());
            lastOutputError = rc;
            return true;
        }
        // The decoder may report the coded size (multiples of 16).
        if (!frame.width || !frame.height || (frame.width & 1) || (frame.height & 1) || frame.width > unsigned((width + 15) & ~15)
            || frame.height > unsigned((height + 15) & ~15) || !frame.planes[0] || !frame.planes[1])
        {
            videoDecoderReleaseFrame(&pool->decoder, &frame);
            fail("frame geometry", MAKERESULT(Module_Libnx, LibnxError_BadInput));
            return false;
        }
        auto out       = std::make_shared<DecodedFrame>();
        out->pool      = pool;
        out->frame     = frame;
        out->width     = frame.width;
        out->height    = frame.height;
        out->decodedUs = nowUs();
        for (auto it = pending.begin(); it != pending.end(); ++it)
            if (it->stamp == frame.timestamp_ns)
            {
                out->submittedUs   = it->submitUs;
                out->sequence      = it->sequence;
                const uint64_t lat = out->decodedUs - it->submitUs;
                latencyUs += lat;
                latencyMaxUs = std::max(latencyMaxUs, lat);
                pending.erase(pending.begin(), std::next(it));
                break;
            }
        decoded++;
        decodedSinceOpen++;
        if (!firstFrame)
        {
            brls::Logger::info("Decoder: first picture {}x{}", frame.width, frame.height);
            firstFrame = true;
        }
        output.push(std::move(out));
        switch2WakeVideoFrame();
    }
    return true;
}

void Decoder::readFrames()
{
    while (!stopped.load())
    {
        {
            std::lock_guard lock(mutex);
            drain(PollNs);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

void Decoder::report(uint64_t now)
{
    if (now - windowUs < 1000000)
        return;
    const auto stats = output.stats();
    brls::Logger::info("Decoder: {} in, {} out, {} dropped; decode {:.1f} ms avg {:.1f} max; submit {:.1f} ms max; "
                       "queue shown {} backlog {} age {} newer {}",
        submitted, decoded, dropped, decoded ? latencyUs / 1000.0 / decoded : 0.0, latencyMaxUs / 1000.0,
        submitMaxUs / 1000.0, stats.shown, stats.droppedBacklog, stats.droppedAge, stats.droppedNewer);
    windowUs  = now;
    submitted = decoded = dropped = 0;
    latencyUs = latencyMaxUs = submitUs = submitMaxUs = 0;
}

// The SPS the decoder gets: the console's with DecoderRefs reference frames.
void Decoder::rebuildDecoderSps()
{
    if (sps.empty())
        return;
    auto limited     = limitReferences(sps, DecoderRefs);
    decoderSps       = limited ? std::move(*limited) : sps;
    const auto given = parseSps(decoderSps);
    const std::string text = given ? describe(*given) : std::string("?");
    if (text != lastDecoderSps)
    {
        brls::Logger::info("Decoder: the decoder gets {}", text);
        lastDecoderSps = text;
    }
}

bool Decoder::submit(const uint8_t* data, size_t size, int lostBefore)
{
    if (!data || !size || size > MaxSample)
        return false;
    const auto units = nals(data, size);
    bool vcl = false, idr = false, hasSps = false;
    for (const auto& nal : units)
    {
        if (nal.type == 7)
        {
            std::vector<uint8_t> next(data + nal.start, data + nal.end);
            if (next != sps)
            {
                std::string hex;
                for (uint8_t byte : next)
                    hex += fmt::format("{:02x}", byte);
                const auto info = parseSps(next);
                brls::Logger::info("Decoder: SPS {}: {}", hex, info ? describe(*info) : std::string("doesn't parse"));
            }
            sps    = std::move(next);
            hasSps = true;
            rebuildDecoderSps();
        }
        else if (nal.type == 8)
            pps.assign(data + nal.start, data + nal.end);
        else if (nal.type == 1 || nal.type == 5)
        {
            vcl = true;
            idr |= nal.type == 5;
        }
    }
    std::lock_guard lock(mutex);
    if (!vcl)
    {
        // Header-only unit: keep the parameter sets and start opening the
        // decoder before the first frame arrives.
        startOpen();
        return true;
    }
    received.fetch_add(1, std::memory_order_relaxed); // pictures, for the stats overlay
    sequence += uint64_t(lostBefore > 0 ? lostBefore : 0) + 1;
    const uint64_t pictureNumber = sequence;
    const uint64_t now = nowUs();
    report(now);
    if (!open)
    {
        startOpen();
        dropped++;
        return false;
    }
    if (needIdr)
    {
        if (!idr)
        {
            dropped++;
            return false;
        }
        needIdr  = false;
        openedUs = now; // the first-picture timeout counts from this keyframe
    }
    if (!drain(0))
    {
        dropped++;
        return false;
    }
    if (!decodedSinceOpen && nowUs() > openedUs + StallUs)
    {
        fail("first picture", KERNELRESULT(TimedOut));
        dropped++;
        return false;
    }
    pool->releaseFrames();

    const uint8_t* bytes = data;
    size_t length        = size;
    if (idr && !hasSps)
    {
        // Keyframes need their parameter sets in-band as well.
        sample.clear();
        appendWithStartCode(sample, decoderSps.data(), decoderSps.data() + decoderSps.size());
        appendWithStartCode(sample, pps.data(), pps.data() + pps.size());
        sample.insert(sample.end(), data, data + size);
        bytes  = sample.data();
        length = sample.size();
    }
    else if (hasSps)
    {
        // Replace the console's SPS with the decoder's.
        sample.clear();
        size_t copied = 0;
        for (const auto& nal : units)
            if (nal.type == 7)
            {
                sample.insert(sample.end(), data + copied, data + nal.codeStart);
                appendWithStartCode(sample, decoderSps.data(), decoderSps.data() + decoderSps.size());
                copied = nal.end;
            }
        sample.insert(sample.end(), data + copied, data + size);
        bytes  = sample.data();
        length = sample.size();
    }
    if (idr && !loggedKeyframe)
    {
        loggedKeyframe = true;
        std::string types;
        for (const auto& nal : nals(bytes, length))
            types += fmt::format("{}{}", types.empty() ? "" : ",", nal.type);
        brls::Logger::info("Decoder: first keyframe {} bytes, NAL units {}", length, types);
    }
    const auto stamp = int64_t(now) * 1000;
    const Result rc  = videoDecoderSubmit(&pool->decoder, bytes, length, stamp, false, SubmitNs);
    const uint64_t took = nowUs() - now;
    submitUs += took;
    submitMaxUs = std::max(submitMaxUs, took);
    if (R_FAILED(rc))
    {
        // A timed-out submit may have taken a prefix; never resend the sample.
        fail("submit", rc);
        dropped++;
        return false;
    }
    submitted++;
    pending.push_back({ stamp, now, pictureNumber });
    if (pending.size() > 64)
    {
        fail("output stalled", KERNELRESULT(TimedOut));
        return false;
    }
    drain(0);
    return !needIdr;
}

}
