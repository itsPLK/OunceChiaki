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

Those axes are a DualSense's held flat. A console in handheld is held upright,
screen to the player, so as it came its turns landed on the wrong DualSense
axes: turning left was a roll, a steering-wheel turn a yaw (PS5 gyro keyboard,
2026-11-27). In handheld the app turns the upright pose into a flat controller
(`uprightToFlat`): the console's top is the DualSense's far edge. The log says
`Controller: motion from ...` when the source changes. A Pro Controller and
detached Joy-Cons are held flat already and pass through unchanged.

Borealis reads only the latest six-axis sample once per loop: about 210 gyro
events a second, half of them repeats, of the sensor's 200 samples. Feeding
every sample instead, evenly timed, felt no different on the console
(2026-11-27): motion still feels a little coarse, and this isn't the cause.

The console asks for a motion reset often: in the PS5's gyro keyboard about
once per key, often twice 65 ms apart. chiaki-ng's reset subtracts the gravity
measured then from later accelerometer readings, so after turning the
controller well past half a turn from that pose, one axis drifts sideways.
Rotating the axes to level at each reset instead (tried 2026-11-27) made the
gyro keyboard unusable: the gyro's axes moved with every key. A real
DualSense's stay fixed to the body, so chiaki-ng's reset is kept.

The log shows the first accelerometer and gyro samples. Checked in handheld
(2026-11-27, with a probe since removed): the system's axes are x right, y the
console's top, z the screen normal; Borealis's mapping to SDL's is right.

## Touchpad

The touchscreen (1280×720 in HID coordinates) maps onto the whole touchpad
(1920×1079), with up to two fingers. A touch shorter than 250 ms that moves at
most 24 px is a tap and clicks the touchpad for 100 ms; Minus also clicks. When
the quick menu opens, the fingers are lifted, and the screen is ignored until
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

## Microphone

Voice chat, from the quick menu (Home): the **Microphone** switch. It starts off in every stream, and capture runs only while it's
on.

- Capture: `audin` (`src/app/microphone.cpp`). The Switch 2 lists one input,
  `BuiltInHeadset`, which is the console's own microphone; 48 kHz stereo 16-bit,
  read in 10 ms buffers. Whether a plugged-in headset takes over isn't tried yet.
- Sending (`Stream::setMicrophone`): as chiaki-ng's client, Opus
  `RESTRICTED_LOWDELAY` at a constant 32 kbps, so every 10 ms frame is exactly
  the 40 bytes chiaki's audio sender takes (it drops any other size), then
  `chiaki_audio_sender_opus_data`. The first "on" sends
  `chiaki_session_connect_microphone`.
- `chiaki_session_toggle_microphone`'s flag is the reverse of its name: `true`
  sends "unmute" (its own log says so; chiaki-ng's GUI passes an inverted flag).
- chiaki encrypts and sends each voice packet while holding
  `gkcrypt_local_mutex`, which every other send needs, the receive thread's
  acks included. If a `send()` waited on the Wi-Fi with it held, the stream
  would stall. The app times each send and logs `Microphone: last 5 s: ...`
  only when one took over 2 ms; the close line has the slowest. On a very bad
  Wi-Fi (2026-11-22) the stream lagged with the microphone on, not shown to be
  caused by it. If it is: patch `chiaki_takion_send_mic_packet` to send after
  unlocking, with `MSG_DONTWAIT`.

When a stream closes, the log has `Stream: microphone sent N frames (...);
slowest send N ms`.
