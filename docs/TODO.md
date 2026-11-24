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
- **"Both sides" rumble** (optional): each Joy-Con gets the stronger haptics
  channel instead of stereo. Only if asked for.

## Waiting for a console test

- The PS4/PS5 choice when pairing a console that hasn't answered a search
  (`hosts_activity.cpp`).
- A PS4 stream (nothing PS4-specific has run yet).
- The microphone on good Wi-Fi, a minute off then a minute on: no lag, and
  no `Microphone: last 5 s` lines (sends over 2 ms). Also with a headset
  plugged in: does it take over from the console's microphone?
- Removing a console and pairing it again.
- 30 to 60 fps frame generation since the holds (`docs/STREAMING.md`):
  windows scale with the period, so extrapolation can hold a real picture up
  to 12 ms. Check skipped predictions, held waits and the feel with
  Extrapolate; if the latency shows, cap the window in milliseconds.

## Known, minor

- The SDK picks a 240 MB decoder work buffer at 1920x1088 (71 MB at 1080).
- Occasional decoder submit spikes of about 20 ms.
- Rare render loop stalls: in one 5 s window of a 4-minute run, held
  pictures waited 18-24 ms and 6 predictions were late (2026-11-24, around a
  game switch).

## Not possible

- Adaptive triggers: the console sends trigger effects, but Joy-Cons and the
  Pro Controller have nothing that resists.
