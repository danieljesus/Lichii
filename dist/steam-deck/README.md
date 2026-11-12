# Lichii on the Steam Deck

This is the `steam-deck` branch. It is `nx2` plus the work below, measured in Ocarina of Time on
a Steam Deck OLED.

## What the branch adds

- **Steam Deck defaults.** The Deck is detected by its DMI (Valve, Jupiter or Galileo). On it,
  the defaults are bilinear scaling and no anti-aliasing (+2.5-2.8 % fps), and both can still be
  changed in the settings.
- **Texture cache.** Past the critical memory, the collector takes images that are costly to make
  again (ASTC transcoded on the GPU) last. On the Deck the cache sits near its limit while you
  walk. Before this change it transcoded the same textures over and over, costing up to 27 ms of
  GPU a frame: panning went from 15.8 to 19.0 fps, and frames over 80 ms fell from 54 to 18 a
  run. Upstream #1435 (keep the images used in the last 150 frames) covers much of the same
  ground. `EDEN_NX2_TC_COSTLY_AGE=<frames>` tunes it, and 0 turns it off.
- **CPU writes.** OnCPUWrite skips the texture and shader cache locks when the page filters rule
  them out. That's +1.7 % on a desktop and +1.6 % on the Deck.
- **Open PRs carried here.** #1161 (Ocarina water and draw cost), #948 (uneven-mip ASTC kept in
  BC on small GPUs), #1234, #823 and others.

## Measured (Steam Deck OLED, GPU fixed at 1600 MHz, Lossless Scaling frame generation on)

Hyrule Field, 3 runs each:

| | standing | panning | frames ≥ 60 ms |
|---|---|---|---|
| before (prueba/F9 + defaults) | 18.5 fps | 15.8-18.9 fps | ~150-210 a run |
| this branch | 19.0-19.5 fps | 19.8-20.9 fps | 71-107 a run |

The Deck is GPU-bound. The game's own upscaler pass alone costs 8.5-10 ms of GPU a frame, and
half resolution gives the same fps.

## Setting it up

1. Build Lichii (see the repository README). On SteamOS, a distrobox (e.g. Arch) with clang
   works well.
2. Optionally, run `dist/steam-deck/install-helpers.sh` once from a terminal. It installs two small
   root helpers with a sudoers rule, so the launcher can set the CPU energy preference to
   "performance" and fix the GPU clock at 1600 MHz while Lichii runs, then restore both.
3. Add `dist/steam-deck/lichii-steam.sh` to Steam as a non-Steam game. It reads these
   environment variables:
   - `LICHII_BIN` (default `build/bin/lichii`);
   - `LICHII_DISTROBOX=<container>` if Lichii was built in a distrobox;
   - `LICHII_EPP=0` and `LICHII_GPUCLK=0` skip the helpers.

## Recommended settings

- **Frame generation.** Lossless Scaling (LSFG, x2) if you own it: Lichii loads `Lossless.dll` from
  `~/.local/share/eden/lossless/`. The generation passes cost ~1.4 ms of GPU.
- **Resolution 1x.** Lower resolutions don't help, because the frame is bound by fixed-size
  compute work.
- **Ocarina FSR off** (the default). Replacing the game's upscaler with FSR 3.1 saves ~2 ms of
  GPU, but no fps.
- **ASTC decoding on the GPU** (the default). Asynchronous CPU decoding doubled the stutters.
