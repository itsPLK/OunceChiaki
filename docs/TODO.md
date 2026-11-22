# To do

Open work, roughly in order. Remove an item when it lands on `main`.

## Features

- **"Both sides" rumble** (optional): each Joy-Con gets the stronger haptics
  channel instead of stereo. Only if asked for.
- **Audio diagnostics.** `src/app/audio.cpp` logs an audio summary every 5 s;
  drop it to a line per minute (or only when something went wrong) once audio
  has been quiet for a while.

## Waiting for a console test

- The PS4/PS5 choice when pairing a console that hasn't answered a search
  (`hosts_activity.cpp`).
- A PS4 stream (nothing PS4-specific has run yet).
- Removing a console and pairing it again.

## Release

- First GitHub release: bump `VERSION`, run **Actions → Release**.

## Known, minor

- The SDK picks a 240 MB decoder work buffer at 1920x1088 (71 MB at 1080).
- Occasional decoder submit spikes of about 20 ms.

## Not possible

- Adaptive triggers: the console sends trigger effects, but Joy-Cons and the
  Pro Controller have nothing that resists.
