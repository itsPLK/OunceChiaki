// Adapted from Moonlight-Switch (XITRIX, GPL-3.0): AudrenAudioRenderer.cpp.
#include "audio.hpp"

#include <algorithm>
#include <borealis.hpp>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <malloc.h>
#include <opus/opus.h>

namespace ounce
{

namespace
{

const uint8_t sinkChannels[] = { 0, 1 };

const AudioRendererConfig rendererConfig = {
    .output_rate     = AudioRendererOutputRate_48kHz,
    .num_voices      = 24,
    .num_effects     = 0,
    .num_sinks       = 1,
    .num_mix_objs    = 1,
    .num_mix_buffers = 2,
};

}

AudioOutput::~AudioOutput() { close(); }

bool AudioOutput::open(unsigned ch, unsigned r, unsigned samples)
{
    close();
    if (ch < 1 || ch > 2 || r != 48000 || samples < 120 || samples > 5760)
    {
        brls::Logger::error("Audio: unsupported format {} channels, {} Hz, {} samples", ch, r, samples);
        return false;
    }
    channels     = int(ch);
    rate         = int(r);
    frameSamples = int(samples);
    bufferBytes  = frameSamples * channels * int(sizeof(int16_t));
    played = dropped = concealed = underruns = 0;
    trimmed = cushions = wTrimmed = 0;
    windowNs  = 0;
    policy    = AudioPolicy(adaptive);
    lastLimit = policy.limit();

    int error = 0;
    decoder   = opus_decoder_create(rate, channels, &error);
    pcm       = static_cast<int16_t*>(std::malloc(bufferBytes));
    const int poolSize = (bufferBytes * BufferCount + AUDREN_MEMPOOL_ALIGNMENT - 1) & ~(AUDREN_MEMPOOL_ALIGNMENT - 1);
    pool = memalign(AUDREN_MEMPOOL_ALIGNMENT, poolSize);
    if (!decoder || error != OPUS_OK || !pcm || !pool)
    {
        brls::Logger::error("Audio: setup failed (opus {})", error);
        close();
        return false;
    }
    Result rc = audrenInitialize(&rendererConfig);
    if (R_FAILED(rc))
    {
        brls::Logger::error("Audio: audrenInitialize 0x{:x}", rc);
        close();
        return false;
    }
    rc = audrvCreate(&driver, &rendererConfig, channels);
    if (R_FAILED(rc))
    {
        brls::Logger::error("Audio: audrvCreate 0x{:x}", rc);
        audrenExit();
        close();
        return false;
    }
    driverOpen = true;
    std::memset(buffers, 0, sizeof(buffers));
    for (int i = 0; i < BufferCount; i++)
    {
        buffers[i].data_raw            = pool;
        buffers[i].size                = poolSize;
        buffers[i].start_sample_offset = i * frameSamples;
        buffers[i].end_sample_offset   = buffers[i].start_sample_offset + frameSamples;
    }
    const int poolId = audrvMemPoolAdd(&driver, pool, poolSize);
    audrvMemPoolAttach(&driver, poolId);
    audrvDeviceSinkAdd(&driver, AUDREN_DEFAULT_DEVICE_NAME, channels, sinkChannels);
    rc = audrenStartAudioRenderer();
    if (R_FAILED(rc))
    {
        brls::Logger::error("Audio: audrenStartAudioRenderer 0x{:x}", rc);
        close();
        return false;
    }
    audrvVoiceInit(&driver, 0, channels, PcmFormat_Int16, rate);
    audrvVoiceSetDestinationMix(&driver, 0, AUDREN_FINAL_MIX_ID);
    for (int i = 0; i < channels; i++)
        for (int j = 0; j < channels; j++)
            audrvVoiceSetMixFactor(&driver, 0, i == j ? 1.0f : 0.0f, i, j);
    audrvVoiceStart(&driver, 0);
    brls::Logger::info("Audio: {} channel(s) at {} Hz, {} samples ({} ms) per packet; {}", channels, rate, frameSamples,
        frameSamples * 1000 / rate, adaptive ? "adaptive to an unstable network" : "low latency");
    return true;
}

void AudioOutput::close()
{
    if (driverOpen)
    {
        // Stop the driver before freeing the pool it plays from.
        driverOpen = false;
        audrvVoiceStop(&driver, 0);
        audrvClose(&driver);
        audrenExit();
        brls::Logger::info("Audio: closed; {} packets played, {} dropped as late, {} quiet ones trimmed, {} concealed, "
                           "{} underruns ({} cushioned); limit {} ms at the end",
            played, dropped, trimmed, concealed, underruns, cushions, policy.limit());
    }
    if (decoder)
        opus_decoder_destroy(decoder);
    decoder = nullptr;
    std::free(pcm);
    pcm = nullptr;
    std::free(pool);
    pool = nullptr;
}

// Audio handed to audren and not played yet, from the wave buffer states.
int AudioOutput::queuedMs()
{
    int pending = 0;
    for (const auto& buffer : buffers)
        if (buffer.state == AudioDriverWaveBufState_Waiting || buffer.state == AudioDriverWaveBufState_Queued
            || buffer.state == AudioDriverWaveBufState_Playing)
            pending++;
    return pending * frameSamples * 1000 / rate;
}

int AudioOutput::freeBuffer()
{
    for (int i = 0; i < BufferCount; i++)
        if (buffers[i].state == AudioDriverWaveBufState_Free || buffers[i].state == AudioDriverWaveBufState_Done)
            return i;
    return -1;
}

void AudioOutput::play(const uint8_t* packet, size_t size)
{
    if (!driverOpen)
        return;
    // Decode every packet, even dropped ones, so the Opus state stays continuous.
    const int decoded = opus_decode(decoder, packet && size ? packet : nullptr, packet ? opus_int32(size) : 0, pcm,
        frameSamples, 0);
    if (decoded <= 0)
        return;
    if (!packet || !size)
    {
        concealed++;
        wConcealed++;
    }
    audrvUpdate(&driver);
    report();
    const uint64_t nowUs = armTicksToNs(armGetSystemTick()) / 1000;
    const int queued     = queuedMs();
    wQueueMin            = std::min(wQueueMin, queued);
    wQueueMax            = std::max(wQueueMax, queued);
    const AudioAction action = policy.decide(queued, quietPcm(pcm, size_t(decoded) * channels), nowUs);
    if (policy.limit() != lastLimit)
    {
        brls::Logger::info("Audio: limit {} ms (queued {} ms)", policy.limit(), queued);
        lastLimit = policy.limit();
    }
    if (action == AudioAction::Trim)
    {
        trimmed++;
        wTrimmed++;
        return;
    }
    if (action == AudioAction::Drop || freeBuffer() < 0)
    {
        dropped++;
        wDropped++;
        return;
    }
    // Nothing left to play when a packet arrives: the output went silent.
    if (queued == 0 && played > 0)
    {
        underruns++;
        wUnderruns++;
        policy.underrun(nowUs);
        if (policy.isAdaptive())
        {
            // The gap happened anyway: 10 ms of silence ahead of this packet
            // leaves a cushion for the next hiccup.
            std::vector<int16_t> silence(size_t(frameSamples) * channels, 0);
            queue(silence.data(), frameSamples);
            cushions++;
        }
    }
    queue(pcm, decoded);
    played++;
    wPlayed++;
}

void AudioOutput::queue(const int16_t* samples, int count)
{
    const int index = freeBuffer();
    if (index < 0)
        return;
    auto& buffer       = buffers[index];
    void* destination  = static_cast<char*>(pool) + index * bufferBytes;
    const size_t bytes = size_t(count) * channels * sizeof(int16_t);
    std::memcpy(destination, samples, bytes);
    armDCacheFlush(destination, bytes);
    buffer.end_sample_offset = buffer.start_sample_offset + count;
    audrvVoiceAddWaveBuf(&driver, 0, &buffer);
    if (!audrvVoiceIsPlaying(&driver, 0))
        audrvVoiceStart(&driver, 0);
    audrvUpdate(&driver);
}

void AudioOutput::report()
{
    const uint64_t now = armTicksToNs(armGetSystemTick());
    if (!windowNs)
        windowNs = now;
    if (now - windowNs < 5000000000ull)
        return;
    brls::Logger::info("Audio: last 5 s: {} played, {} dropped as late, {} trimmed, {} concealed, {} underruns; queued {}..{} ms, limit {} ms",
        wPlayed, wDropped, wTrimmed, wConcealed, wUnderruns, wQueueMin == (1 << 30) ? 0 : wQueueMin, wQueueMax, policy.limit());
    windowNs = now;
    wPlayed = wDropped = wTrimmed = wConcealed = wUnderruns = 0;
    wQueueMin = 1 << 30, wQueueMax = 0;
}

}
