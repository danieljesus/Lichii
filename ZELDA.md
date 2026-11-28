# The `zelda` branch

**Very experimental.** This is a personal branch of [Lichii](https://github.com/zortos293/Lichii), a Nintendo Switch 2 emulator, tuned and tested with one game: *The Legend of Zelda: Ocarina of Time*. It is not an official Lichii release and not a separate project. Everything here that can go upstream is proposed there; this branch is where it is tried first.

If you want Lichii, use [Lichii](https://github.com/zortos293/Lichii). If you want to try these builds, they are under [Releases](https://github.com/danieljesus/Lichii/releases). Other games are not tested here and may run worse than on Lichii.

## What it has that Lichii's `nx2` does not

- **Precompiled shaders built from your own play**, prepared in the background and resumable. This is the work of [auz466](https://github.com/auz466) (Lichii #1366, open) on top of [zmckenney](https://github.com/zmckenney)'s precompile series; this branch carries it and keeps it merged with `nx2`.
- **macOS:** a MoltenVK build of our own (cooperative matrices, and precise shader arithmetic that Metal can inline; the latter was merged into SPIRV-Cross as KhronosGroup/SPIRV-Cross#2762), plus several changes to how render passes and compute work are issued on MoltenVK (Lichii #2307, #2310, #2341, #2446, #2492, #2635, open).
- **Steam Deck and other four-core CPUs:** the GPU back thread gets a physical core to itself (#2667, open); host images are reused instead of recreated (#1955, open).
- **Optimization profiles (PGO)** trained on this branch's code with Ocarina of Time, for Linux and Windows, and a release job that reports when a profile no longer fits the code (#1909, open).
- **Audio:** a stream that loses its device keeps playing, and a stuck renderer can be stopped (#2286, #2392, #2429, open).
- **Hardening of file parsers and services** found by fuzzing and review, about thirty changes (among them #2364, #2371, #2372, #2375, #2379, #2380, #2403, #2496, #2580, open), and CPU-emulation fixes (#2264, #2266, #2281, #2287, open).

More than 80 changes that started here have been merged into Lichii; they are not listed because they are simply part of Lichii now.

## What it does not have

The branch follows `nx2` one batch behind: each build is merged up to a known `nx2` commit, measured on three machines, and only then released. Build 9 is at `nx2` of 26 November 2026, about twenty commits behind at the time of release.

## Measured, with Ocarina of Time

Build 9 against build 8, same scene, alternating runs:

| Machine | Standing still | Turning the camera |
|---|---|---|
| Apple M6 (macOS, MoltenVK) | 37 → 60 fps (the game's cap) | 34 → 52 fps |
| Steam Deck OLED | 21.4 → 22.8 fps | 17.6 → 18.6 fps |
| RTX 5080 (Windows) | 59.8 → 59.6 fps (60 fps cap) | 50.8 → 50.5 fps |

Build 9's gains are on macOS and on the Steam Deck (there with GameMode turned off in the emulator; absolute frame rates on the Deck vary from one session to another, the comparison between builds within a session is what holds); on Windows it performs the same as build 8. These are one game on three machines and say nothing about other titles. The Deck's row was measured on the published package; the Mac's and the Windows rows on builds made as the release job makes them, each then checked against the published package.

## Known issues

- Rarely, the game stops on its opening video right after starting. Close the emulator and start again.
- On Linux, GameMode 1.8 and later undoes the four-core thread layout unless its CPU pinning is turned off; the log explains how.
- Rarely the emulator can crash from a race in the GPU query cache (seen once in over a hundred runs, while quitting; a fix is proposed upstream as #2627).
- macOS: with the internal resolution at 2x, Ocarina of Time closes about a minute after starting. 1x and 1.5x are not affected; a fix is being tested.
- Build 9's Linux PGO package was built with part of its optimization profile not applied; measured, it performs like a correctly built one. The next build fixes the release job.

## Thanks

To the Lichii maintainers and contributors, who review and merge what we send, and to auz466 and zmckenney, whose precompiled-shader work this branch is built around.
