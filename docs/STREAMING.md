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

## Presentation (to implement)

CivChiaki's rules, which the native renderer must keep:

- **Video:** at most 2 decoded frames waiting. The newest frame wins; a frame
  older than 2 frame periods is dropped. Frames are drawn by the render loop,
  never by the decode thread, so waiting for the display can't stall decoding.
- **Audio:** two 20 ms output buffers plus one replaceable chunk. When the
  output is full, stale audio is dropped instead of queued.

Don't add a jitter buffer or playout smoothing (Moonlight-Switch's
`AVFrameQueue` holds 3 frames by default): it trades latency for smoothness,
and CivChiaki showed it isn't needed on a LAN.

## Switch 2 platform code

Hardware H.264 decoding, zero-copy display through Vulkan and audren audio come
from Moonlight-Switch (XITRIX, GPLv3), adapted to the rules above. The UI is
XITRIX's Borealis fork, which also lets the app take over the Home, Capture and
Chat (C) buttons.
