# Changelog

## v0.2.1 (2026.11.27)

- Motion controls in handheld fixed: the console held upright now acts as a DualSense held flat, so turning left is a turn (it came through as a roll) and a steering-wheel turn is a roll
- With **Debug log** on, the log shows how far each motion sensor turned every second, for checking motion problems

## v0.2.0 (2026.11.24)

- **FSR upscaling** (off by default): AMD FidelityFX Super Resolution 1 (EASU) sharpens the picture when streaming below the screen's resolution, such as 720p on the 1080p screen. It runs once per decoded picture and is shared by frame generation, with no added latency; greyed out when the stream already matches the screen
- **FSR sharpening** (Off, Low, Medium, High): FSR 1's RCAS after the upscale, with noise detection so compression noise isn't amplified
- Smoother 60→120 fps frame generation on Wi-Fi: a picture arriving early in a burst now waits for the pending generated frame instead of dropping it (Interpolate: no dropped frames, steady 120 fps; Extrapolate: nearly all skipped predictions recovered, a real picture waits at most 6 ms for the prediction before it)
- **Debug log** setting (off by default): the periodic stream statistics (and the video draw's and upscale's GPU time) are written only when it's on; otherwise the log keeps events and problems, so streaming no longer writes to the SD card every second

## v0.1.0 (2026.11.23)

Initial release of OunceChiaki for Nintendo Switch 2 (kaiHEN).

- Low-latency LAN Remote Play streaming up to 1080p60 with Switch 2 hardware H.264 decoding and zero-copy Vulkan display
- Adaptive low-latency audio output with clock-drift resampling and an optional **Force low-latency audio** mode
- Frame generation (30→60 and 60→120 fps) with **Extrapolate** and **Interpolate** modes
- Full controller support: DualSense-style motion controls (gyro and accelerometer), touchscreen as two-finger touchpad with tap-to-click, HD rumble (plus PS5 haptics), and voice chat via the console's microphone
- Quick menu over the stream (Home button) with time, battery, Wi-Fi, live stream stats overlay, microphone toggle, screen brightness, **Disconnect**, and **Disconnect and sleep**
- LAN console discovery, manual IP pairing, and automatic wake-on-connect for paired consoles
