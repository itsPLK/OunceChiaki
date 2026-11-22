# Controller

Player 1 becomes a DualSense (PS5) or DualShock 4 (PS4). The code is in
`src/app/controller.cpp` (input), `src/app/rumble.cpp` (output) and
`src/app/input_math.hpp` (the arithmetic, tested by `tools/tests/input_test.cpp`).

## Buttons and sticks

By position; see the table in the README. ZL/ZR are digital, so L2/R2 are 0 or 255.

## Motion

Borealis reads player 1's six-axis sensor every loop (handheld, Pro Controller
or Joy-Con pair) and reports it in SDL's units and axes: acceleration in g,
rotation in rad/s. That is what chiaki-ng's GUI feeds its orientation tracker
from SDL, so the same calls are used (`chiaki_orientation_tracker_update`,
`chiaki_accel_new_zero_*`). `CHIAKI_EVENT_MOTION_RESET` from the console resets
the origin.

The log shows the first accelerometer and gyro samples. Held flat and still, the
accelerometer should read about (0, 1, 0) and the gyro about zero. If motion
moves the wrong way in a game, the axis signs in Borealis's
`handleControllerSensors` are the suspect.

## Touchpad

The touchscreen (1280×720 in HID coordinates) maps onto the whole touchpad
(1920×1079), with up to two fingers. A touch shorter than 250 ms that moves at
most 24 px is a tap and clicks the touchpad for 100 ms; Minus also clicks. When
the stream menu opens, the fingers are lifted, and the screen is ignored until
nothing touches it, so a tap on the menu is not a click in the game.

## Rumble

Two sources, mixed per frame on the UI thread:

- `CHIAKI_EVENT_RUMBLE`: the two classic motors (0..255). The heavy one drives
  the 160 Hz band on both sides, the light one the 320 Hz band.
- PS5 haptics (with `enable_dualsense`): 16-bit stereo PCM at 3 kHz, 10 ms per
  packet. A packet's strength per side is the larger of its mean absolute
  amplitude doubled (chiaki-ng's measure, with its noise floor of 100) and half
  its peak, so short clicks are felt (Astro's Playroom's right trigger test sends
  sparse clicks whose mean is only 0.03). The strongest packet since the last
  frame is used, and haptics stop 50 ms after the last packet. The stream is
  stereo, as a DualSense's two actuators: left drives the left Joy-Con, right
  the right one. Games do mix it unevenly (in Astro's Playroom's trigger tests
  the right channel is often silent).

The stronger of the two wins, scaled by the Rumble setting (weak 0.5, normal 1,
strong 1.6). It drives libnx HD rumble directly, not Borealis's `sendRumble`,
which uses a fixed strength and varies the frequency instead. Changes under 2%
are not sent.

Horizon can report vibration as not permitted (seen on the console, 2026-11-22):
every send then succeeds but nothing moves, and the actual value read back is
0. The app then opens a permit-vibration session
(`hidBeginPermitVibrationSession`) for the stream's lifetime, which leaves the
system setting alone.

When a stream closes, the log has `Stream: feedback: N rumble events, N haptics
packets (N with vibration)` and `Controller: N motion samples, N touches, N taps`.
