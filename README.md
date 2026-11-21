# OunceChiaki

A minimal PlayStation Remote Play client for Nintendo Switch 2, for
[kaiHEN](https://github.com/ouncebrew). It streams a PS4 or PS5 on your local
network with the low-latency behavior of CivChiaki
([how it streams](docs/STREAMING.md)).

> [!WARNING]
> This is an unofficial project, not endorsed or certified by Sony Interactive
> Entertainment LLC. You must own a PS4 or PS5.

**Status:** early. The current build starts, finds consoles on the network and
tests the system buttons. Pairing and streaming come next.

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
