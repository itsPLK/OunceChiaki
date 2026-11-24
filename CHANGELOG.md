# Changelog

## Unreleased

- **FSR upscaling** (off by default): FSR 1 EASU on luma when the stream is below the screen's resolution, once per picture (about 1.7 ms of GPU time at 720p to 1080p, no added latency); greyed out at the screen's resolution
- **FSR sharpening** (Off, Low, Medium, High): FSR 1 RCAS after EASU, with noise detection (about 0.3 ms more)

## v0.1.0 (2026.11.23)

Initial release of OunceChiaki for Nintendo Switch 2 (kaiHEN).

- Low-latency LAN Remote Play streaming up to 1080p60 with Switch 2 hardware H.264 decoding and zero-copy Vulkan display
- Adaptive low-latency audio output with clock-drift resampling and an optional **Force low-latency audio** mode
- Frame generation (30→60 and 60→120 fps) with **Extrapolate** and **Interpolate** modes
- Full controller support: DualSense-style motion controls (gyro and accelerometer), touchscreen as two-finger touchpad with tap-to-click, HD rumble (plus PS5 haptics), and voice chat via the console's microphone
- Quick menu over the stream (Home button) with time, battery, Wi-Fi, live stream stats overlay, microphone toggle, screen brightness, **Disconnect**, and **Disconnect and sleep**
- LAN console discovery, manual IP pairing, and automatic wake-on-connect for paired consoles
