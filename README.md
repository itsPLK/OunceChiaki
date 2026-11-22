# OunceChiaki

A minimal PlayStation Remote Play client for Nintendo Switch 2, for
[kaiHEN](https://github.com/ouncebrew). It streams a PS4 or PS5 on your local
network with the low-latency behavior of CivChiaki
([how it streams](docs/STREAMING.md)).

> [!WARNING]
> This is an unofficial project, not endorsed or certified by Sony Interactive
> Entertainment LLC. You must own a PS4 or PS5.

**Status:** early. It finds and pairs consoles and streams a PS5 at up to
1080p60 with the hardware decoder, with optional frame generation (30 to 60 or
60 to 120 fps). Motion, the touchscreen as the touchpad and rumble are new and
still being tested. PS4 is untested.

## Controls

The buttons map by position, as on a PlayStation controller.

| Switch | PlayStation |
|---|---|
| B / A / Y / X | Cross / Circle / Square / Triangle |
| L / R, ZL / ZR | L1 / R1, L2 / R2 |
| Plus | Options |
| Minus, or a tap on the screen | Touchpad click |
| Touchscreen | Touchpad (one or two fingers) |
| Chat (C) | PS button |
| Capture | Create / Share |
| Home | Stream menu: stats, microphone, disconnect (B: back to the game) |
| Motion | The controller's gyro and accelerometer |

Motion, the touchpad and rumble strength (off, weak, normal, strong) are in
Settings. On PS5, rumble also follows the game's haptics. Voice chat uses the
console's microphone: turn it on from the stream menu (it starts muted).

## Install

Download `OunceChiaki.nro` from the
[releases](../../releases) and copy it to `/switch/OunceChiaki/` on the SD card.
Settings and logs live in that folder.

## Build

Needs Docker. See [AGENTS.md](AGENTS.md) for details.

```sh
scripts/init-submodules.sh
scripts/build.sh   # -> build/switch2/OunceChiaki.nro
```

## Credits

- [chiaki-ng](https://github.com/streetpea/chiaki-ng) (Street Pea and the Chiaki
  contributors, originally [Chiaki](https://git.sr.ht/~thestr4ng3r/chiaki) by
  Florian Märkl): the Remote Play protocol.
- [Moonlight-Switch](https://github.com/XITRIX/Moonlight-Switch) and
  [Borealis](https://github.com/XITRIX/borealis) by XITRIX: Switch 2 video,
  audio and UI.
- [ouncebrew](https://github.com/ouncebrew): libounce, OunceVK and kaiHEN.

## Licence

GNU AGPL 3.0, as chiaki-ng (see [COPYING](COPYING)). If you distribute builds,
provide the corresponding source.
