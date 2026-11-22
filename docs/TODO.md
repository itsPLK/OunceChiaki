# To do

Open work, roughly in order. Remove an item when it lands on `main`.

## Features

- **Microphone.** The PS5 takes voice chat from the controller's microphone.
  chiaki-ng has it (`chiaki_session_connect_microphone`,
  `chiaki_session_toggle_microphone`, `audiosender.c`); it needs
  `opusencoder.c` back in `cmake/chiaki_core.cmake` (libounce has Opus) and
  capture from the Switch 2's built-in microphone or a headset, which isn't
  tried under kaiHEN yet. A mute toggle belongs in the stream menu.
- **"Both sides" rumble** (optional): each Joy-Con gets the stronger haptics
  channel instead of stereo. Only if asked for.
- **Audio diagnostics.** `src/app/audio.cpp` logs an audio summary every 5 s;
  drop it to a line per minute (or only when something went wrong) once audio
  has been quiet for a while.

## Waiting for a console test

- The PS4/PS5 choice when pairing a console that hasn't answered a search
  (`hosts_activity.cpp`).
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
- HEVC (H.265), and with it HDR. The Switch 2 decoder service has no HEVC
  codec: on 2026-11-22 (branch `hevc-probe`) `viddecDecoderInitialize`
  accepted H.264 at 640x368 with 64 MB of work memory, and rejected codec 2
  (with or without the VPS/SPS/PPS) with the same `0x17986f` (module 111,
  3020) as the unused value 9. libounce itself rejects codec 2 earlier
  (`0x1759`, bad input). Software HEVC would cost too much latency at
  1080p60. PS5 HDR needs HEVC (and the decoder's HDR output is VP9 and AV1
  only), so the stream stays H.264, which is also what a PS4 sends.
