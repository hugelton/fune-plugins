<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Mini

A monophonic three-oscillator synthesizer of the early 1970s for [Fune](https://github.com/hugelton/Fune), drawn as its panel:
wooden end cheeks, a dark panel, black knobs with white skirts. An outside engine plugin, id `mini`. Plain C, integers only:
the same code on every device, the FM-1 included.

**Based on [CA-72](https://github.com/idlefoundry/ca-72) by Idle Foundry** (copyright © 2026 Idle Foundry Ltd., GPL-3.0-or-later),
a circuit-modelled emulation of the 1972 instrument. Mini is a small integer port of CA-72's light voice ("Potato mode",
`crates/ca72/src/light.rs`): its structure (the oscillators at the circuit's levels and duty cycles, the mixer's loaded channels,
the ladder's input pair and four one-poles solved without delay, the contours of RC segments with the circuit's time laws and
trigger, the VCA's law, glide, the modulation mix and wheel), its laws read from tables made from CA-72's own functions.

## Licence

This folder is **GPL-3.0-or-later** as a whole (`LICENSE`). Fune's core is MPL-2.0 and contains nothing of it; Mini is built and
handed out as a plugin of its own, with this folder's licence and notices.

| File | What |
| --- | --- |
| `mini.c` | the plugin: its parameters, panel, keys as host voices, presets' hook |
| `mini_int.h` | the voice: CA-72's light voice ported to integers |
| `mini_int_tables.h` | its laws: tables generated from CA-72 (do not edit) |
| `tools/mini_gen/` | the tables' generator (Rust, not part of the build): takes CA-72 at `88a6b23` from GitHub and writes `mini_int_tables.h` |
| `mini_knobs.h`, `mini_presets.h`, `mini_test.c`, `CMakeLists.txt` | the panel's knobs, the factory presets (Mini's own), the test, the build |

"Minimoog" and "Moog" are trademarks of their owners; Mini is not theirs, uses neither name, and copies no logo or trade dress.

## Version 3 (2026-11-26)

The ladder's four stages keep 256 times the input pair's resolution (8 in version 2): at the lowest cutoffs a quiet signal's step
truncated to nothing, a stage froze, and the ladder could not start ringing from the mixer's hiss: the Whistle preset (every
source off, the filter's own ringing) was silent. Everything else sounds as it did (the teaser's MURIYA P: -73 dB of difference).

## What the port leaves out

The ladder runs at the voice's rate (CA-72's at eight times: Mini aliases more when it rings hard at high cutoffs), its input pair
is solved as if straight and then through its tanh once (CA-72 solves it exactly); the contours' absorbed charge, the waveforms'
droop, the noise's bands, the VCA's thump, FILTER MODE and POLES, and the plug-in's own extras (POLY, DRIVE, ENTROPY ...) are not
here. Measured against CA-72's light voice itself on Mini's eight presets (2026-11-22): the levels within 0.4 dB, the
third-octave spectra within 0.3 .. 4.4 dB on average.

## Building

In a Fune checkout this folder is added by Fune's CMake: `build/mini/mini.so` and the test (`kreisel_mini_test [DIR]`, which
also writes each preset as a WAV). Elsewhere set `FUNE_DIR` to a Fune checkout (its `sdk/` and `src/`). The FM-1 image:
`python3 tools/kpl_build.py plugins/mini/mini.c` in a Fune checkout.

To make the tables again (Rust 1.85 or later, network for CA-72):

```bash
cargo run --release --manifest-path plugins/mini/tools/mini_gen/Cargo.toml > plugins/mini/mini_int_tables.h
```
