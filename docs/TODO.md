# To do

Open work, roughly in order. Remove an item when it lands on `main`.

## Features

- **HEVC (H.265) and HDR: later, if a way opens.** PS5 only; better picture at
  the same bitrate, and HDR needs it (10-bit Main10). The chip has HEVC, but
  the decoder service doesn't offer it (2026-11-22, branch `hevc-probe`):
  - libounce rejects codec 2 itself (`0x1759`, bad input: it sizes the work
    memory per codec).
  - `viddecDecoderInitialize` accepts H.264 at 640x368 with 64 MB of work, and
    rejects codec 2 (with or without the VPS/SPS/PPS) with the same
    `0x17986f` (module 111, 3020) as the unused value 9.
  - Not run yet: the branch's last build sweeps codec values 0 to 31 and tries
    opening `/dev/nvhost-nvdec` (the original Switch's route, FFmpeg nvtegra).
  - KaiVideo (Jellyfin player for Switch 2) found the same and decodes HEVC on
    the CPU with FFmpeg; for a stream that costs latency, a CPU copy to the
    GPU and battery. The decoder's HDR output is VP9 and AV1 only.
- **Stream menu as a sidebar.** The Home menu (disconnect, stream stats,
  microphone) in the style of the Switch's own overlay: a panel down the
  right side of the screen over the stream. Add the Wi-Fi connection (network
  name and signal strength), the time, the battery (level, charging) and a
  screen brightness slider with an auto-brightness toggle. And "Disconnect and
  put the console to sleep" (chiaki's `chiaki_session_goto_bed`, rest mode).
  Borealis dialogs show at most three buttons, so this needs its own view.
- **"Both sides" rumble** (optional): each Joy-Con gets the stronger haptics
  channel instead of stereo. Only if asked for.
- **Audio diagnostics.** `src/app/audio.cpp` logs an audio summary every 5 s;
  drop it to a line per minute (or only when something went wrong) once audio
  has been quiet for a while.

## Waiting for a console test

- The PS4/PS5 choice when pairing a console that hasn't answered a search
  (`hosts_activity.cpp`).
- A PS4 stream (nothing PS4-specific has run yet).
- The microphone on good Wi-Fi, a minute off then a minute on: no lag, and
  no `Microphone: last 5 s` lines (sends over 2 ms). Also with a headset
  plugged in: does it take over from the console's microphone?
- Removing a console and pairing it again.

## Release

- First GitHub release: bump `VERSION`, run **Actions → Release**.

## Known, minor

- The SDK picks a 240 MB decoder work buffer at 1920x1088 (71 MB at 1080).
- Occasional decoder submit spikes of about 20 ms.

## Not possible

- Adaptive triggers: the console sends trigger effects, but Joy-Cons and the
  Pro Controller have nothing that resists.
