#include "stream.hpp"

#include "input_math.hpp"
#include "log.hpp"
#include "microphone.hpp"
#include "ounce_core.h"

#include <borealis.hpp>
#include <chiaki/audiosender.h>
#include <chiaki/time.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <opus/opus.h>

namespace ounce
{

struct Stream::State : std::enable_shared_from_this<Stream::State>
{
    ChiakiSession session {};
    std::string address; // ConnectInfo.host points here
    Decoder* decoder   = nullptr;
    AudioOutput* audio = nullptr;
    Events events;
    bool initialized = false; // chiaki_session_init succeeded
    bool joined      = false;
    std::atomic<bool> alive { true }; // the Stream still exists
    std::atomic<uint64_t> lastIdrMs { 0 };
    // Feedback, written by chiaki's threads and read by the UI thread.
    std::atomic<uint16_t> rumble { 0 };     // heavy << 8 | light
    std::atomic<uint32_t> haptics { 0 };    // strongest since the UI read: left << 16 | right, 0..65535
    std::atomic<uint64_t> hapticsUs { 0 };  // when the last haptics packet arrived
    std::atomic<uint32_t> rumbleEvents { 0 }, hapticPackets { 0 }, hapticActive { 0 };
    // Held while the session is finalized, and by threads outside chiaki that
    // call into it (the decoder's opener), so they never see it half gone.
    std::mutex lifetime;
    // Microphone: captured on its own thread, encoded and sent while on.
    Microphone mic;
    OpusEncoder* micEncoder      = nullptr;
    ChiakiAudioSender* micSender = nullptr;
    bool micConnected            = false;
    std::atomic<uint64_t> micSent { 0 }, micShort { 0 };
    // How long a send takes: chiaki encrypts and sends each voice packet while
    // holding the lock every other send (input, acks from the receive thread)
    // needs, so a send that waits on Wi-Fi could stall the stream (2026-11-22).
    uint64_t micWindowUs = 0, micWindowSent = 0, micSlowestUs = 0, micOver2ms = 0, micOver10ms = 0;
    uint64_t micSlowestEverUs = 0, micOver2msEver = 0, micOver10msEver = 0;
};

namespace
{

ChiakiVideoResolutionPreset preset(int height)
{
    switch (height)
    {
        case 360: return CHIAKI_VIDEO_RESOLUTION_PRESET_360p;
        case 540: return CHIAKI_VIDEO_RESOLUTION_PRESET_540p;
        case 720: return CHIAKI_VIDEO_RESOLUTION_PRESET_720p;
        default: return CHIAKI_VIDEO_RESOLUTION_PRESET_1080p;
    }
}

// Chiaki's receive thread: the decoder takes the frame or asks for a keyframe.
bool onVideo(uint8_t* buf, size_t size, int32_t lost, bool recovered, void* user)
{
    auto* state = static_cast<Stream::State*>(user);
    if (lost > 0)
        brls::Logger::info("Stream: {} frame(s) lost{}", lost, recovered ? ", recovered" : "");
    const bool decoded = state->decoder->submit(buf, size, lost);
    if (!decoded && state->decoder->needsIdr())
    {
        const uint64_t now = chiaki_time_now_monotonic_ms();
        if (now - state->lastIdrMs >= 1000)
        {
            state->lastIdrMs = now;
            brls::Logger::info("Stream: decoder needs a keyframe, requesting one");
            chiaki_session_request_idr(&state->session);
        }
    }
    return decoded;
}

// Chiaki's receive thread: audio is decoded and queued right away.
void onAudioHeader(ChiakiAudioHeader* header, void* user)
{
    auto* state = static_cast<Stream::State*>(user);
    state->audio->open(header->channels, header->rate, header->frame_size);
}

void onAudioFrame(uint8_t* buf, size_t size, void* user)
{
    static_cast<Stream::State*>(user)->audio->play(buf, size);
}

// Chiaki's receive thread: PS5 haptics, 16-bit stereo PCM (3 kHz), turned into a
// rumble strength per side as chiaki-ng does without a DualSense attached.
void onHaptics(uint8_t* buf, size_t size, void* user)
{
    auto* state = static_cast<Stream::State*>(user);
    state->hapticPackets++;
    const size_t frames = size / (2 * sizeof(int16_t));
    if (!frames)
        return;
    int16_t pcm[512];
    const size_t n = std::min(frames, sizeof(pcm) / sizeof(pcm[0]) / 2);
    std::memcpy(pcm, buf, n * 2 * sizeof(int16_t)); // the buffer may be unaligned
    const auto [left, right] = hapticStrength(pcm, n);
    if (left > 0 || right > 0)
        state->hapticActive++;
    // Keep the strongest packet per side until the UI thread reads it, so a
    // click isn't replaced by the silent packet after it.
    const uint32_t l = uint32_t(left * 65535.0f), r = uint32_t(right * 65535.0f);
    uint32_t seen    = state->haptics.load();
    while (!state->haptics.compare_exchange_weak(seen, std::max(seen >> 16, l) << 16 | std::max(seen & 0xffff, r)))
    {
    }
    state->hapticsUs = chiaki_time_now_monotonic_us();
}

// Chiaki's threads: copy what's needed, act on the UI thread.
void onEvent(ChiakiEvent* event, void* user)
{
    std::shared_ptr<Stream::State> state = static_cast<Stream::State*>(user)->shared_from_this();
    switch (event->type)
    {
        case CHIAKI_EVENT_CONNECTED:
            brls::Logger::info("Stream: connected");
            brls::sync([state] {
                if (state->alive && state->events.connected)
                    state->events.connected();
            });
            break;
        case CHIAKI_EVENT_LOGIN_PIN_REQUEST:
        {
            const bool incorrect = event->login_pin_request.pin_incorrect;
            brls::Logger::info("Stream: console asks for its login PIN{}", incorrect ? " again" : "");
            brls::sync([state, incorrect] {
                if (state->alive && state->events.loginPin)
                    state->events.loginPin(incorrect);
            });
            break;
        }
        case CHIAKI_EVENT_RUMBLE:
            state->rumble = uint16_t(event->rumble.left << 8 | event->rumble.right);
            if (state->rumbleEvents++ == 0)
                brls::Logger::info("Stream: first rumble from the console ({}, {})", event->rumble.left, event->rumble.right);
            break;
        case CHIAKI_EVENT_MOTION_RESET:
            brls::Logger::info("Stream: console reset the motion origin");
            brls::sync([state] {
                if (state->alive && state->events.motionReset)
                    state->events.motionReset();
            });
            break;
        case CHIAKI_EVENT_QUIT:
        {
            const ChiakiQuitReason reason = event->quit.reason;
            std::string text = chiaki_quit_reason_string(reason);
            if (event->quit.reason_str && *event->quit.reason_str)
                text += std::string(": ") + event->quit.reason_str;
            const bool error = chiaki_quit_reason_is_error(reason);
            brls::Logger::info("Stream: ended ({})", text);
            brls::sync([state, text, error, reason] {
                if (state->alive && state->events.ended)
                    state->events.ended(text, error, reason);
            });
            break;
        }
        default:
            break;
    }
}

// Before the session goes: the capture thread sends through it.
void closeMicrophone(Stream::State& state)
{
    state.mic.stop();
    if (state.micSender)
        chiaki_audio_sender_free(state.micSender);
    state.micSender = nullptr;
    if (state.micEncoder)
    {
        opus_encoder_destroy(state.micEncoder);
        brls::Logger::info("Stream: microphone sent {} frames ({} not 40 bytes, skipped); slowest send {:.1f} ms, "
                           "{} over 2 ms, {} over 10 ms",
            state.micSent.load(), state.micShort.load(), state.micSlowestEverUs / 1000.0, state.micOver2msEver,
            state.micOver10msEver);
    }
    state.micEncoder = nullptr;
}

void finish(Stream::State& state)
{
    std::lock_guard lock(state.lifetime);
    if (!state.initialized || state.joined)
        return;
    closeMicrophone(state);
    chiaki_session_stop(&state.session);
    chiaki_session_join(&state.session);
    chiaki_session_fini(&state.session);
    state.joined = true;
    const OunceCoreStats core = ounce_core_stats();
    brls::Logger::info("Stream: closed; congestion signals {}, FEC skipped while waiting for IDR {}, corrupt reports coalesced {}",
        core.congestion_signals, core.fec_skipped, core.reports_coalesced);
    brls::Logger::info("Stream: feedback: {} rumble events, {} haptics packets ({} with vibration)",
        state.rumbleEvents.load(), state.hapticPackets.load(), state.hapticActive.load());
}

}

Stream::~Stream()
{
    if (!state)
        return;
    state->alive = false;
    finish(*state);
}

bool Stream::start(const Host& host, const StreamSettings& settings, Events events)
{
    if (state)
        return false;
    if (!host.registered)
        return false;
    state          = std::make_shared<State>();
    state->address = host.address;
    state->decoder = &decoder;
    state->audio   = &audio;
    state->events  = std::move(events);
    ounce_core_reset_stats();

    ChiakiConnectInfo info {};
    info.ps5  = host.ps5();
    info.host = state->address.c_str();
    std::memcpy(info.regist_key, host.registKey.data(), sizeof(info.regist_key));
    std::memcpy(info.morning, host.rpKey.data(), sizeof(info.morning));
    chiaki_connect_video_profile_preset(&info.video_profile, preset(settings.height),
        settings.fps >= 60 ? CHIAKI_VIDEO_FPS_PRESET_60 : CHIAKI_VIDEO_FPS_PRESET_30);
    info.video_profile.bitrate = settings.bitrate;
    info.video_profile.codec   = CHIAKI_CODEC_H264;
    // CivChiaki's session settings (docs/STREAMING.md).
    info.video_profile_auto_downgrade = false;
    info.enable_idr_on_fec_failure    = true;
    info.packet_loss_max              = 0.05;
    info.enable_keyboard              = false;
    info.enable_dualsense             = host.ps5();

    brls::Logger::info("Stream: connecting to {} at {}: {}x{} at {} fps, {} kbps, H.264", host.name, host.address,
        info.video_profile.width, info.video_profile.height, info.video_profile.max_fps, info.video_profile.bitrate);
    ChiakiErrorCode err = chiaki_session_init(&state->session, &info, chiakiLog());
    if (err != CHIAKI_ERR_SUCCESS)
    {
        brls::Logger::error("Stream: session init failed: {}", chiaki_error_string(err));
        state.reset();
        return false;
    }
    state->initialized = true;
    chiaki_session_set_event_cb(&state->session, onEvent, state.get());
    chiaki_session_set_video_sample_cb(&state->session, onVideo, state.get());
    // The decoder opens in the background; ask for a keyframe once it's ready.
    std::weak_ptr<State> weak = state;
    decoder.setKeyframeNeeded([weak] {
        auto s = weak.lock();
        if (!s)
            return;
        std::lock_guard lock(s->lifetime);
        if (!s->alive || !s->initialized || s->joined)
            return;
        s->lastIdrMs = chiaki_time_now_monotonic_ms();
        brls::Logger::info("Stream: decoder ready, requesting a keyframe");
        chiaki_session_request_idr(&s->session);
    });
    ChiakiAudioSink sink {};
    sink.user      = state.get();
    sink.header_cb = onAudioHeader;
    sink.frame_cb  = onAudioFrame;
    chiaki_session_set_audio_sink(&state->session, &sink);
    if (info.enable_dualsense)
    {
        ChiakiAudioSink haptics {};
        haptics.user     = state.get();
        haptics.frame_cb = onHaptics;
        chiaki_session_set_haptics_sink(&state->session, &haptics);
    }
    err = chiaki_session_start(&state->session);
    if (err != CHIAKI_ERR_SUCCESS)
    {
        brls::Logger::error("Stream: session start failed: {}", chiaki_error_string(err));
        chiaki_session_fini(&state->session);
        state.reset();
        return false;
    }
    return true;
}

void Stream::reset()
{
    if (!state)
        return;
    state->alive = false;
    finish(*state);
    state.reset();
}

void Stream::stop()
{
    if (state && state->initialized && !state->joined)
        chiaki_session_stop(&state->session);
}

bool Stream::setMicrophone(bool on)
{
    if (!state)
        return false;
    std::lock_guard lock(state->lifetime);
    if (!state->initialized || state->joined)
        return false;
    State& s = *state;
    if (!on)
    {
        s.mic.stop();
        // chiaki's flag is the reverse of its name: false sends "mute".
        chiaki_session_toggle_microphone(&s.session, false);
        brls::Logger::info("Stream: microphone muted");
        return true;
    }
    if (!s.micEncoder)
    {
        // As chiaki-ng's client: low delay, and a constant bitrate that fills
        // the sender's 40-byte frames exactly (it drops any other size).
        int error    = 0;
        s.micEncoder = opus_encoder_create(Microphone::Rate, Microphone::Channels, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
        if (!s.micEncoder || error != OPUS_OK)
        {
            brls::Logger::error("Stream: microphone encoder failed ({})", error);
            s.micEncoder = nullptr;
            return false;
        }
        opus_encoder_ctl(s.micEncoder, OPUS_SET_BITRATE(40 * 8 * Microphone::Rate / Microphone::FrameSamples));
        opus_encoder_ctl(s.micEncoder, OPUS_SET_VBR(0));
        opus_encoder_ctl(s.micEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
        opus_encoder_ctl(s.micEncoder, OPUS_SET_PHASE_INVERSION_DISABLED(1));
        s.micSender = chiaki_audio_sender_new(chiakiLog(), &s.session);
        if (!s.micSender)
        {
            brls::Logger::error("Stream: microphone sender failed");
            opus_encoder_destroy(s.micEncoder);
            s.micEncoder = nullptr;
            return false;
        }
    }
    State* raw = &s;
    if (!s.mic.start([raw](const int16_t* pcm) {
            uint8_t packet[40];
            const int size = opus_encode(raw->micEncoder, pcm, Microphone::FrameSamples, packet, sizeof(packet));
            if (size != int(sizeof(packet)))
            {
                raw->micShort++;
                return;
            }
            const uint64_t before = chiaki_time_now_monotonic_us();
            chiaki_audio_sender_opus_data(raw->micSender, packet, size_t(size));
            const uint64_t after = chiaki_time_now_monotonic_us(), took = after - before;
            raw->micSent++;
            State& m = *raw;
            m.micWindowSent++;
            m.micSlowestUs = std::max(m.micSlowestUs, took);
            m.micSlowestEverUs = std::max(m.micSlowestEverUs, took);
            if (took > 2000)
                m.micOver2ms++, m.micOver2msEver++;
            if (took > 10000)
                m.micOver10ms++, m.micOver10msEver++;
            if (!m.micWindowUs)
                m.micWindowUs = after;
            if (after - m.micWindowUs >= 5000000)
            {
                brls::Logger::info("Microphone: last 5 s: {} sent, slowest send {:.1f} ms, {} over 2 ms, {} over 10 ms",
                    m.micWindowSent, m.micSlowestUs / 1000.0, m.micOver2ms, m.micOver10ms);
                m.micWindowUs = after;
                m.micWindowSent = m.micSlowestUs = m.micOver2ms = m.micOver10ms = 0;
            }
        }))
        return false;
    if (!s.micConnected)
    {
        chiaki_session_connect_microphone(&s.session);
        s.micConnected = true;
    }
    chiaki_session_toggle_microphone(&s.session, true); // "unmute" (see above)
    brls::Logger::info("Stream: microphone on");
    return true;
}

void Stream::setController(const ChiakiControllerState& controller)
{
    if (!state || !state->initialized || state->joined)
        return;
    ChiakiControllerState copy = controller;
    chiaki_session_set_controller_state(&state->session, &copy);
}

Stream::Feedback Stream::feedback()
{
    Feedback out;
    if (!state)
        return out;
    const uint16_t rumble = state->rumble;
    out.heavy             = uint8_t(rumble >> 8);
    out.light             = uint8_t(rumble);
    // Packets come every 10 ms and frames every 8 to 17 ms: a frame with no
    // new packet keeps the last strength, until none came for HapticHoldUs.
    const uint64_t now = chiaki_time_now_monotonic_us(), at = state->hapticsUs;
    const unsigned packets = state->hapticPackets;
    if (packets != lastHapticPackets)
    {
        lastHaptics       = state->haptics.exchange(0);
        lastHapticPackets = packets;
    }
    if (at && (now < at || now - at <= HapticHoldUs))
    {
        out.hapticLeft  = float(lastHaptics >> 16) / 65535.0f;
        out.hapticRight = float(lastHaptics & 0xffff) / 65535.0f;
    }
    return out;
}

void Stream::loginPin(const std::string& pin)
{
    if (state && state->initialized && !state->joined)
        chiaki_session_set_login_pin(&state->session, reinterpret_cast<const uint8_t*>(pin.data()), pin.size());
}

}
