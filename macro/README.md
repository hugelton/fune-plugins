# Macro

A Fune engine plugin (id `macr`): eight synthesis models in one engine, chosen with a list value, played with three timbre
controls and a low-pass gate. One integer code for every device (the FM-1 has no FPU): desktop, web, Cardputer Zero, FM-1.

Credits: after Mutable Instruments Plaits (MIT), by Emilie Gillet: the models, their tables and formulas are ported from
[Plaits](https://github.com/pichenettes/eurorack) to fixed point. Licence: MIT (`LICENSE`; Plaits' notice in `LICENSE-MIT`).

## Controls

| Page | K1 | K2 | K3 | K4 |
| --- | --- | --- | --- | --- |
| SOUND | MODL (the model) | HARM | TIMB | MORF |
| EDIT | DCAY (the gate's decay; at its top the gate stays open and the track's envelope shapes the note) | COLR (VCA .. VCF) | TENV (the decay envelope to TIMB, -100 .. +100 %) | LVL |

HOME's macros: HARM, TIMB, MORF, DCAY. Each note pings the gate; its velocity sets how far the gate opens (brighter, and through
the host louder). The host's FLT modulation (ENV 1, LFO) moves TIMB, its SHP modulation MORF. At its defaults the track's ENV 1 is
a gate (`gate = 1`): the low-pass gate is the note's shape.

## Models

| # | Name | After Plaits' | HARM | TIMB | MORF |
| --- | --- | --- | --- | --- | --- |
| 0 | Pair | virtual analog (VA_VARIANT 2's OUT) | the second oscillator's detune (unison, fifths, octaves) | the square's width, then its sync sweep | the saw: notch, saw, triangle |
| 1 | Fold | waveshaping | the waveshaper's curve | the wavefolder's gain | the slope's symmetry |
| 2 | FM | two-operator FM (4x oversampled) | the ratio (quantized) | the index | the feedback (phase / self) |
| 3 | Grain | grain (formant, OUT) | the second formant's interval, the carrier's bleed | the formant | the window's shape |
| 4 | Harm | additive (24 harmonics) | the bumps | the spectrum's centre | its width |
| 5 | Wave | wavetable (4 banks of 8 x 8) | the bank (quantized past the middle) | x | y |
| 6 | Chord | chords (divide-down voices) | the chord (eleven) | the inversion | the registration (all eight) |
| 7 | Noise | filtered noise | LP .. BP .. HP | the clock (sample hold .. white) | the resonance |

Where it differs from Plaits (each is said at its model in `macro.c`): Chord's MORF sweeps Plaits' eight organ / string
registrations across its whole range (Plaits goes on to wavetable voices past the middle: their state does not fit a voice);
Harm's TIMB is not moved by TENV (its spectrum is the part's); Noise ends in a soft knee, not Plaits' limiter; the models' gains
are evened out so a held note is about as loud in each; the parameters step once a 32-sample block where Plaits ramps them.
The AUX outputs are not played.

## Sizes and cost

`python3 tools/kpl_build.py plugins/macro/macro.c`: see the session report and `memos/2026-11-22_macro_plugin.md` for the
figures (the .kpl, RAM, the FM-1's instructions a block per model).

## Dev tools (`dev/`, not part of the plugin)

- `build_compare.sh EURORACK`: builds `plaits_compare` (Plaits' float engines at 44.1 kHz beside these integer models: the
  spectra's difference in sixth-octave bands; `plaits_compare gate`: the gate against Plaits' LPG), `macro_wavs` (WAVs through
  Fune's audio path) and `macro_shots` (the pages, a note sounding).
- `emu_flash.py`, `emu_cost.py`: a flash file for fm1-emu with the plugin installed, and its cost there.
- `gen_macro_tables.py EURORACK/plaits/resources.cc`: writes `macro_tables.h`.
