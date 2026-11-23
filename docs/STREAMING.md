# How OunceChiaki streams

OunceChiaki keeps the streaming behavior of CivChiaki (Chiaki-NG compiled for
the Civilization VII userland on Switch 2), which streamed with no visible lag
or hitches, and drops the workarounds that only that userland needed. This page
says what was kept, why, and where it lives. Change any of it only with
console measurements that show the change helps.

## Protocol core

The core is upstream chiaki-ng's C library (`third_party/chiaki-ng`, pinned at
the commit CivChiaki used), built by `cmake/chiaki_core.cmake`. The submodule is
never edited. `tools/patch_chiaki.py` writes patched copies of seven files into
the build tree; each patch must match exactly once, so a chiaki-ng update that
moves code fails the build instead of dropping a patch. The patches call
`src/core/ounce_core.c`.

| Patch | File | Why |
|---|---|---|
| 1 MiB socket receive buffer | `takion.c` | Absorbs packet bursts while the app is busy. If the system refuses 1 MiB, it halves until a size is accepted (upstream would fail the session) and logs the result. The advertised reliable window stays upstream's. |
| Receive backlog timer | `takion.c` | Times each wait for data. A wait that blocks (1 ms or more) means the socket was empty. |
| Delay-based congestion signal | `congestioncontrol.c` | With 1 MiB of buffer nothing is lost under overload, so the console never lowers its bitrate and the queue turns into input lag. When the socket has not been empty for 250 ms, the loss report is raised to `packet_loss_max` (5%) and the console backs off. |
| Corrupt-frame report coalescing | `streamconnection.c` | Reports for the same start within 100 ms are covered by the next one; don't flood the console. |
| Skip FEC while waiting for an IDR | `frameprocessor.c`, `videoreceiver.c` | The receiver throws P-frames away until the keyframe; repairing them only delays recovery. |
| ACK size check accepts duplicate TSNs | `takion.c` | Upstream rejects valid ACKs that list duplicate TSNs. |
| No AIA (curl) include | `common.c` | LAN only; `src/core/lan_only.c` stubs the internet path. |
| No thread-limit check | `thread.c` | Upstream reads the process resource limit with raw SVCs before each thread; under kaiHEN that kills the app. `pthread_create` fails cleanly anyway. |

Session settings (when the session code lands): `video_profile_auto_downgrade =
false`, `enable_idr_on_fec_failure = true`, `packet_loss_max = 0.05`, H.264.

Not carried over (CivChiaki needed them only because it ran as JavaScript
without a JIT): fibers instead of threads, batched video decryption, skipping
video MAC checks, JavaScript FEC and crypto, typed-array copies.

## Presentation

CivChiaki's rules, kept by `src/video/frame_queue.cpp`:

- **Video:** at most 2 decoded frames wait; a third drops the oldest. A frame
  that waited more than 2 frame periods is dropped. At 30 fps the newest frame
  is shown; at 60 fps the oldest waiting one, so one frame absorbs arrival
  jitter (the first frame waits at most one period for a second). Frames are
  drawn by the render loop, never by the decode thread, and the loop presents
  only when a new frame arrived (Borealis's frame-driven mode, with a 34 ms
  keepalive), so waiting for the display never stalls decoding.
- **Audio** (`src/app/audio.cpp`, adapted from Moonlight-Switch's audren
  output): each 10 ms Opus packet is decoded on chiaki's receive thread and
  queued to audren at once; when more than 40 ms is waiting, the packet is
  dropped instead of queued (CivChiaki kept two 20 ms buffers plus one
  replaceable chunk). chiaki-ng's own 3-packet jitter prefill is kept, as
  CivChiaki had it; lost packets are concealed by Opus.
  - The voice holds only 4 wave buffers (40 ms); a thread calls
    `audrvUpdate()` every 5 ms to hand it the rest. Updating only when a packet
    arrived ran the voice dry during network stalls, with audio still waiting.
  - The console's audio clock runs about 0.25% fast against audren's. Packets
    are resampled by 2 to 4 samples (under 1%, inaudible) to hold the queue
    20 ms under the limit; without that, the queue overflowed every few seconds
    and a packet was dropped: a crack.
  - By default the limit adapts (`src/app/audio_policy.hpp`): it rises by
    20 ms (up to 120 ms) when a burst outgrows it, and falls by 10 ms after 10 s
    without trouble; the first 2 s (the burst at connect) don't count. On a
    lossy Wi-Fi (2026-11-22) it played 80 s with no drops, where the fixed
    40 ms limit dropped 10 packets in 22 s. "Force low-latency audio" keeps
    the fixed limit.

Don't add a jitter buffer or playout smoothing (Moonlight-Switch's
`AVFrameQueue` holds 3 frames by default): it trades latency for smoothness,
and CivChiaki showed it isn't needed on a LAN.

### Experimental 30 to 60 fps display

Both experimental modes request a 30 fps stream. They estimate motion by
comparing luma samples on a 16-pixel grid in consecutive decoded pictures.
The game's HUD is part of the video and can distort along moving edges.

- **Extrapolation** displays the real picture immediately. A timer wakes the
  render loop 16.7 ms later to project the picture half a motion vector
  forward. If the next real frame arrives late enough to trigger Borealis's
  34 ms keepalive, the repeat reuses the last computed motion field so the
  display holds the extrapolated picture instead of snapping back to the
  previous real frame. A new real frame takes priority. A prediction over 8 ms
  late is skipped. At 720p, build f750d57 drew 3,146 real and 2,979 predicted
  frames in 105 seconds on the console. At 1080p, 73 five-second windows
  averaged 149.3 generated draws/5 s (59.6 unique fps, 16.71/16.74 ms draw
  intervals, 2 spacing rejections total).
- **Interpolation** uses both decoded pictures to reconstruct their midpoint.
  It samples both images along the estimated motion, blends aligned samples,
  and holds the earlier image where their colors disagree substantially. The
  midpoint is presented when the second picture arrives, and that real picture
  is scheduled 16.7 ms later. If a new picture overtakes the deferred one, the
  newest real picture is shown immediately. Gaps, resolution changes, and CPU
  upload fallback also resume real-frame display.

On the console, build 72bbacc at 720p drew 742 real frames and 709 midpoints
in 25 seconds. At 1080p it drew 6,690 real frames and 6,362 midpoints in 225
seconds. Both averaged 58.0 new draws/s in complete gameplay windows. At
1080p the mean real-to-midpoint and midpoint-to-real intervals were 17.0 ms
and 16.9 ms. Real decode-to-draw age averaged 16.1 ms.

For a console comparison, use 720p and the same pan, camera reversal, and scene
cut in all three 30 fps modes. Compare “Video timing” and Borealis's “presents”
counts. A steady generated mode should approach 150 real and 150 generated
draws per five seconds, with both draw intervals near 16.7 ms. Interpolation
adds about 16.7 ms to real decode-to-draw age. “supersededReal” counts deferred
real pictures overtaken by a newer input. These times measure CPU scheduling.
A high-speed recording can measure display cadence.

### Experimental 60 to 120 fps display

The same two modes work on a 60 fps stream, generating the picture between two
real ones 8.3 ms after the real picture. All the timing above is a fraction of
the stream's frame period (`GenerationTiming` in `src/video/frame_generation.hpp`):
half a period for the generated picture, inputs under 0.36 of a period old,
predictions at most 0.24 of a period late; at 30 fps these are the values tuned
for 30 to 60 fps. A pair must be two consecutive pictures: each decoded frame
carries a picture number that counts chiaki's lost pictures and the decoder's
drops. (Pairs used to be judged by arrival spacing, 0.66 to 1.35 periods; at
60 fps Wi-Fi jitter put most consecutive pictures outside that window, and
generation fell from about 50 to 10-20 pictures a second.) Only the upper
spacing limit (2.5 periods) is kept to catch stalls: consecutive pictures
decoded back-to-back after a Wi-Fi burst remain valid motion pairs (removing
the 0.25-period lower limit cut spacing rejections at 1080p 60 to 120 fps
extrapolation from 51.2/5 s to 0.2-0.7/5 s and raised average unique fps from
89.5 to 102-103). The log's "pairs rejected" line counts the reasons. It needs
a 120 Hz output (the handheld screen; docked, a TV the console drives at
120 Hz) and twice the GPU work of 30 to 60 fps. Interpolation adds about 8.3 ms
to real decode-to-draw age.

### In-game stream stats

Open the quick menu with Home and turn on **Stream stats**. The overlay lasts
for this session and stays drawn while the menu is open (top left, clear of
the panel; with the menu open the app redraws on its own loop, so the rates
read differently). Received FPS counts H.264 pictures at decoder input,
including ones subsequently dropped. Shown FPS counts new real and generated
draws. Keepalive repeats are excluded. Decode time runs from decoder submission
to output, and decoded-to-draw time includes queueing and any interpolation
delay. Both are averages for real pictures drawn in the last sampling window.

Text updates once per second. The graph plots the latest 120 intervals between
new draws, with guides at 16.7 and 33.3 ms. Values above 50 ms reach the top
edge. Turn **Stream stats** off in the quick menu to remove it.

## The hardware decoder

`src/video/decoder.cpp` drives the SDK's H.264 decoder (adapted from
Moonlight-Switch). Found on the console (2026-11-22) with the PS5's 1080p60
stream (Main profile, level 5.0, 68 slices per keyframe, 9 reference frames):

- **Configure the coded size.** The stream is 68 macroblock rows (1088 lines)
  cropped to 1080. Told 1080, the decoder fails the first picture
  (`0xfa66f`, module 111 description 2003) and stops taking input; told 1088 it
  decodes. Frame rate, color policy and maximum sample size use the SDK's
  defaults.
- **Fewer reference frames.** The decoder has 8 output images; its SPS is
  rewritten (`src/video/h264_sps.cpp`, tested by `tools/tests/run.sh`) to
  declare 4 reference frames and a 4-frame decode buffer. The console normally
  references only the previous frame.
- The stream has no frame reordering (`max_num_reorder_frames` 0), so the
  decoder outputs each frame as soon as it is decoded.

Measured: 60 frames in and out per second, 5.5 ms decode, 0.5 ms waiting in the
frame queue, zero-copy display (the GPU samples the decoder's image).

## Switch 2 platform code

Hardware H.264 decoding, zero-copy display through Vulkan and audren audio come
from Moonlight-Switch (XITRIX, GPLv3), adapted to the rules above. The UI is
XITRIX's Borealis fork, which also lets the app take over the Home, Capture and
Chat (C) buttons.
