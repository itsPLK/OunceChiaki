# Working on OunceChiaki (for AI agents and new contributors)

OunceChiaki is a minimal PlayStation Remote Play client for Switch 2: a homebrew
NRO for kaiHEN, built with the public ounce-devkit (libounce, OunceVK). It keeps
CivChiaki's streaming behavior (`docs/STREAMING.md`): read that before touching
anything in the stream path.

## Ground rules

- **Small app, full controller.** A minimal UI for LAN streaming of a paired
  PS4/PS5, done well: no PSN sign-in, internet play, or anything that adds
  latency. The controller side should be as complete as the hardware allows:
  gyro (DualSense-style motion), the touchscreen as the touchpad (touches and
  presses), and rumble (`docs/CONTROLLER.md`).
- **Measure on the console.** Streaming changes need console logs that show
  they help. One change per test build.
- **Upstream stays untouched.** Never edit `third_party/chiaki-ng` or
  `extern/borealis`. Change chiaki-ng through `tools/patch_chiaki.py` (each patch
  must match exactly once) and code in `src/core/`.
- **Switch 2 platform code** comes from Moonlight-Switch; say so in the file
  header.
- **Licence:** AGPL-3.0 (chiaki-ng's, see `COPYING`). Moonlight-Switch and
  Borealis code keep their notices.
- **Git:** work on a feature branch, commit at milestones, and leave merging and
  pushing to the owner. Never push or open PRs unless asked.

## Where things are

| Path | What |
|---|---|
| `src/app/` | The app: `main.cpp`, Borealis activities and views, `log.cpp`, `discovery.cpp` |
| `src/core/` | Code the chiaki-ng patches call (`ounce_core.c`), LAN-only stubs |
| `tools/patch_chiaki.py` | The chiaki-ng patches (see `docs/STREAMING.md`) |
| `cmake/chiaki_core.cmake` | Builds chiaki-ng's library with the patched files, nanopb and jerasure |
| `resources/romfs/` | Our romfs files; the NRO step adds the Borealis resources it needs |
| `scripts/build.sh` | The build (Docker on the Mac, directly in CI) |
| `scripts/deps.sh` | mbedTLS and Opus for libounce, pinned in `scripts/deps/sources.sha256` |
| `scripts/console.sh` | FTP deploy and log download |
| `.github/workflows/` | `build.yml` (every push), `release.yml` (by hand) |
| `docs/TODO.md` | Open work: add to it, and remove what lands |
| `.for_reference/` | Gitignored reference checkouts: CivChiaki, Moonlight-Switch, fastcars-recomp-nx2 |

## Building

```sh
scripts/init-submodules.sh   # once: only the submodules the build needs
scripts/build.sh             # -> build/switch2/OunceChiaki.nro
scripts/build.sh --clean     # rebuild the app tree (keeps build/deps)
```

`scripts/build.sh` builds the `ounce-chiaki-build` Docker image from
`docker/Dockerfile` on first use (devkitA64 plus the pinned ounce-devkit, the
same SDK as CI) and rebuilds it when `docker/Dockerfile` or
`scripts/install-sdk.sh` change. The first build takes a few minutes; later ones
are incremental. `build/switch2/compile_commands.json` is generated for editors.

## Releasing

Releases are built by GitHub Actions, never uploaded by hand. Bump `VERSION`
(`MAJOR.MINOR.PATCH`), land it on `main`, then run **Actions → Release → Run
workflow**. It builds the NRO, tags `v<VERSION>` and publishes the release; it
stops if the tag already exists. The version shows in HBMenu and in the app.

## Testing on the console (FTP)

FTP is for development only. The owner gives the console's address and opens
the FTP server (OunceShell, port 1337, anonymous). It answers only while no
homebrew is running: ask the owner to close OunceChiaki first.

```sh
scripts/console.sh <ip> deploy     # upload the NRO and check its size
scripts/console.sh <ip> log        # -> build/logs/ounce.log
```

| Console path | What |
|---|---|
| `/switch/OunceChiaki/OunceChiaki.nro` | the app |
| `/switch/OunceChiaki/logs/ounce.log` (`ounce.1.log`: the run before) | log |

Tell the owner what to test and what to look for, and to turn on **Settings →
Debug log** first: off (the default), the log keeps events and problems only,
and the periodic stats in `docs/STREAMING.md` (stream, video timing, GPU,
decoder, audio, Borealis's presents) and chiaki's heartbeats are left out, so
a stream doesn't write the SD card every second. After the run, read the log
before guessing.

**Crashes.** kaiHEN kills a crashing app without running its exception handler,
so a CPU fault leaves no report: the last lines of `ounce.log` show where it
was. The log is saved every 250 ms (`src/app/log.cpp`), so the final quarter
second before a crash can be missing; per-line SD writes on chiaki's receive
thread once froze a stream. Log the steps around a suspect call to narrow it down. `abort()` and
`std::terminate` write `logs/crash.txt` (seen on the console 2026-11-24);
a GPU fault shows there as `Vulkan frame completion failed: -4` (device lost).
Borealis enables no robust buffer access, so an out-of-bounds shader read or a
command buffer re-recorded while pending loses the GPU; `tools/crash.py build/logs/crash.txt` names its addresses (with
the ELF of the same build).

**Known kaiHEN limits** (found on the console):

- Raw resource-limit SVCs (`svcGetInfo(InfoType_ResourceLimit)`,
  `svcGetResourceLimit*Value`) kill the app. chiaki-ng called them before every
  thread; `tools/patch_chiaki.py` removes that.
- The limited broadcast (255.255.255.255) gets no answers; discovery also sends
  to the subnet's broadcast address, which works.
- A 1 MiB UDP `SO_RCVBUF` is accepted with Borealis's default socket setup.
- Without a nifm request the Wi-Fi connection drops within minutes, and Horizon
  then shuts down every socket (sends fail with EPIPE, "Broken pipe").
  `src/app/network.cpp` holds a request for the app's life and logs every
  connection change; it held for 7 minutes idle with no drop.
