# To do

Open work, roughly in order. Remove an item when it lands on `main`.

## Features

- **Microphone.** The PS5 takes voice chat from the controller's microphone.
  chiaki-ng has it (`chiaki_session_connect_microphone`,
  `chiaki_session_toggle_microphone`, `audiosender.c`); it needs
  `opusencoder.c` back in `cmake/chiaki_core.cmake` (libounce has Opus) and
  capture from the Switch 2's built-in microphone or a headset, which isn't
  tried under kaiHEN yet. A mute toggle belongs in the stream menu.
- **HEVC (H.265).** PS5 only (a PS4 sends H.264 only). Better picture at the
  same bitrate. The blocker is the hardware decoder: libounce's `ViddecCodec`
  lists H.264 (0), VP9 (1), AV1 (3) and VP8 (4), with no HEVC, and
  Moonlight-Switch falls back to software HEVC ("HEVC has no SDK codec entry"),
  which would cost far too much latency at 1080p60. The unused value 2 is worth
  a try on the console. Without hardware HEVC, this stays H.264.
- **HDR.** PS5 only, and it needs HEVC first (chiaki's `CHIAKI_CODEC_H265_HDR`,
  10-bit Main10). Then a 10-bit decode path and an HDR swapchain with metadata
  through OunceVK, on the handheld screen and docked.

  HEVC with HDR is the best PS5 Remote Play offers: no AV1, and at most 1080p.
- **"Both sides" rumble** (optional): each Joy-Con gets the stronger haptics
  channel instead of stereo. Only if asked for.
- **Audio diagnostics.** Branch `audio` logs an audio summary every 5 s;
  drop it to a line per minute (or only when something went wrong) once audio
  has been quiet for a while.

## Waiting for a console test

- Branch `polish`: app icon, PS4/PS5 choice when pairing a console that hasn't
  answered a search, the decoder opening at the stream's coded size.
- A PS4 stream (nothing PS4-specific has run yet).
- Removing a console and pairing it again.

## Release

- First GitHub release: bump `VERSION`, run **Actions → Release**.

## Known, minor

- The SDK picks a 240 MB decoder work buffer at 1920x1088 (71 MB at 1080).
- Occasional decoder submit spikes of about 20 ms.

## Not possible

- Adaptive triggers: the console sends trigger effects, but Joy-Cons and the
  Pro Controller have nothing that resists.
