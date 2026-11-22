#include "stream.hpp"

#include "log.hpp"
#include "ounce_core.h"

#include <borealis.hpp>
#include <chiaki/time.h>
#include <cstring>

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
    bool alive       = true; // the Stream still exists
    uint64_t lastIdrMs = 0;
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
    const bool decoded = state->decoder->submit(buf, size);
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
        case CHIAKI_EVENT_QUIT:
        {
            const ChiakiQuitReason reason = event->quit.reason;
            std::string text = chiaki_quit_reason_string(reason);
            if (event->quit.reason_str && *event->quit.reason_str)
                text += std::string(": ") + event->quit.reason_str;
            const bool error = chiaki_quit_reason_is_error(reason);
            brls::Logger::info("Stream: ended ({})", text);
            brls::sync([state, text, error] {
                if (state->alive && state->events.ended)
                    state->events.ended(text, error);
            });
            break;
        }
        default:
            break;
    }
}

void finish(Stream::State& state)
{
    if (!state.initialized || state.joined)
        return;
    chiaki_session_stop(&state.session);
    chiaki_session_join(&state.session);
    chiaki_session_fini(&state.session);
    state.joined = true;
    const OunceCoreStats core = ounce_core_stats();
    brls::Logger::info("Stream: closed; congestion signals {}, FEC skipped while waiting for IDR {}, corrupt reports coalesced {}",
        core.congestion_signals, core.fec_skipped, core.reports_coalesced);
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
    ChiakiAudioSink sink {};
    sink.user      = state.get();
    sink.header_cb = onAudioHeader;
    sink.frame_cb  = onAudioFrame;
    chiaki_session_set_audio_sink(&state->session, &sink);
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

void Stream::stop()
{
    if (state && state->initialized && !state->joined)
        chiaki_session_stop(&state->session);
}

void Stream::setController(const ChiakiControllerState& controller)
{
    if (!state || !state->initialized || state->joined)
        return;
    ChiakiControllerState copy = controller;
    chiaki_session_set_controller_state(&state->session, &copy);
}

void Stream::loginPin(const std::string& pin)
{
    if (state && state->initialized && !state->joined)
        chiaki_session_set_login_pin(&state->session, reinterpret_cast<const uint8_t*>(pin.data()), pin.size());
}

}
