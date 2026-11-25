# Acid

A bass line synth for Fune: one voice, a saw or a square through a resonant diode-ladder style filter, an envelope that sweeps it,
accents and slides. An outside engine plugin (id `acid`, version 2), integers only: the same C runs on the FM-1, the desktop, the
web and the Cardputer Zero.

**Based on [Open303](https://github.com/RobinSchmidt/Open303) by Robin Schmidt** (MIT; commit 313bf0d of 2024-03-29), an
open-source emulation of a classic 1981 bass line synth. Acid follows Open303's voice, its structure, formulas and constants,
ported to fixed point. Licence: [LICENSE](LICENSE) (MIT, Open303's copyright and this port's). Nothing of Open303 is outside this
folder; Fune's core stays MPL-2.0. No trade names: Acid on every screen and preset.

## Files

| File | |
| --- | --- |
| `acid.c` | the plugin: the DSP, its descriptor, its screens, its record |
| `acid_tables.h` | the integer tables (made by `tools/gen_acid_tables.py`: Open303's float formulas evaluated once) |
| `acid_presets.h` | the eight factory presets |
| `dev/` | the float comparison: `compare.cpp` runs Open303 from its own clone beside the integer port (`build.sh`) |

## Sound

Per sample, as Open303: the oscillator (a saw, or Open303's square: a tanh-shaped saw, a 53 % pulse at half the saw's level),
negated, a 44.5 Hz high-pass, the filter (Open303's TB_303 mode after mystran and kunn: four one-pole stages coupled to their
neighbours, the last fed back through a 150 Hz high-pass, the coefficients a rational fit of the cutoff), a 24.2 Hz high-pass, the
amplitude. Oscillator and filter run at 88.2 kHz (twice the host's rate; Open303 four times, with an elliptic anti-alias filter:
here the two samples are averaged). Left out: Open303's 14 Hz all-pass and 7.5 Hz notch (below hearing).

- **Envelope.** Jumps to 1 at a note, decays over DCY (200 ms on an accented note). The cutoff: CUT x 2^(scaler (env - offset)),
  scaler and offset from ENV and CUT by Open303's measured mapping.
- **Accent.** A note above velocity 110 (a step of strength Hard, a hard key). ACC sets how far: the cutoff rises by ACC x the
  envelope through a 15 ms leaky integrator (the "wow"), the amplitude by ACC x 4 x the envelope; the decay is 200 ms, the release
  50 ms. Below 110 the velocity changes nothing (the level is the same at every velocity): the accent is the dynamics.
- **Slide.** A note that comes while the last is still held glides there (the frequency through a leaky integrator of 0.2 x SLID;
  60 ms is Open303's); no envelope restarts. In Fune's sequencer: a note longer than its step (it overlaps the next). The host says
  so in `voice_t.legato` (interface 1.7); Acid is mono (`engine_t.poly = 1`).
- Fune's own: TUNE (cents), DRV (a soft clip after the filter, x1 .. x8.9 in, 1/sqrt out; 0 off, as Open303).

## Parameters

| | SOUND (K1 .. K4, then two) | | EDIT |
| --- | --- | --- | --- |
| WAVE | Saw / Sqr | TUNE | -100 .. +100 ct |
| CUT | 314 .. 2394 Hz (exponential, Open303's range) | SLID | 10 .. 510 ms (60) |
| RES | 0 .. 100 % (Open303's skew) | DRV | 0 .. 100 % |
| ENV | the envelope's depth | | |
| DCY | 200 .. 2000 ms | | |
| ACC | the accent's depth | | |

Macros (HOME): CUT RES ENV DCY. The host's filter and shape modulation do not reach it (`nomod`); ENV 1 is a gate in the presets
(A 0, S 100, R 20: long enough for an accent's release).

Presets: Squelch, Round Bass, Square Pluck, Long Sweep, Rubber, Hot Line, Dark Sub, Screamer (`acid_presets.h`: `KPreset`s in its
`KPluginExt`, version 2: PRESETS lists them).

## Screens

A small box of six knobs in a row (picture 1 draws the panel, the host's knobs on it, the row the FM-1's knobs hold lit), over it
the filter's sweep (picture 0): the cutoff against the first 800 ms of a plain note (filled) and of an accented one (a line), and
the note sounding now as a dot on its sweep. Small screens: the sweep shorter. A large screen: the sweep beside the box, EDIT's
three knobs under it beside the track's scope. Mockups: `memos/mockups/2026-11-22_acid/`.

## Version 2 (2026-11-26)

A note struck while the voice still sounds (not a slide) starts its first block from the last block's amplitude and cutoff and
ramps to its own across it (0.7 ms): its envelopes jump to the top at once, which clicked (+31 dB over 6 kHz in the 3 ms after a
retrigger, +2.6 dB now; a note from silence +9). Nothing else changed: the slide, the accent, a note from silence bit for bit.

## Fixed point

Filter signals Q20 (1.0 = 2^20: a stage's small steps at a low cutoff are not lost); every product two 32-bit ones (`mulsh_32`: the
FM-1 takes 64-bit products out of line); the coefficients looked up twice a block from `acid_tables.h` and ramped sample by sample;
envelopes once a block (Q24), ramped. No divide in the audio path. State: the voice's `ph[]` and `s[]` (`keep` 0xF3 holds the
filter over a retrigger) and 16 bytes a voice of its own (8 voices: 152 bytes of RAM with the rest).

## Against the original (dev/compare.cpp)

The same notes into Open303 (float, from its clone, the VST's defaults for the square) and the integer port (`dev/acid_devhost.c`:
Fune's calls without its mix); the 16-step line (`dev/pattern.h`, two bars at 125 BPM, accents and slides) at six settings and every
preset (DRV off). Level: RMS. Spectrum: third-octave bands 40 Hz .. 16 kHz within 60 dB of the loudest.

| case | Open303 dB | Acid dB | level diff dB | spectrum mean diff dB | spectrum max diff dB (at Hz) |
| --- | --- | --- | --- | --- | --- |
| line_defaults | -8.9 | -8.2 | +0.74 | 0.87 | 6.5 (6451) |
| line_dark_lowres | -8.1 | -7.4 | +0.71 | 0.95 | 7.6 (3225) |
| line_mid | -8.8 | -8.1 | +0.75 | 0.86 | 7.2 (10240) |
| line_open_highres | -8.5 | -7.8 | +0.72 | 0.60 | 2.5 (10240) |
| line_square_mid | -10.1 | -9.1 | +1.01 | 1.33 | 6.5 (8127) |
| line_max | -8.4 | -7.7 | +0.72 | 0.64 | 1.7 (40) |
| preset_Squelch | -8.4 | -7.7 | +0.74 | 1.17 | 10.4 (8127) |
| preset_Round_Bass | -10.0 | -9.3 | +0.71 | 1.08 | 9.1 (4064) |
| preset_Square_Pluck | -10.3 | -9.4 | +0.94 | 1.29 | 7.6 (6451) |
| preset_Long_Sweep | -7.6 | -6.9 | +0.71 | 1.35 | 14.0 (10240) |
| preset_Rubber | -8.7 | -7.7 | +0.98 | 1.16 | 5.2 (6451) |
| preset_Hot_Line | -8.1 | -7.4 | +0.76 | 1.16 | 11.3 (10240) |
| preset_Dark_Sub | -11.2 | -10.4 | +0.78 | 1.23 | 8.6 (2032) |
| preset_Screamer | -6.5 | -5.9 | +0.60 | 1.21 | 16.5 (12902) |

Worst level difference +1.01 dB; spectrum mean difference over the cases 1.06 dB.

The level is ~0.7 dB over Open303 in every case alike (likely its 7.5 Hz notch and its anti-alias filter, which the port leaves out); the
spectra differ by about 1 dB on average, most at 6 .. 13 kHz (two times oversampling and an average where Open303 has four and an
elliptic filter). Screamer's peaks pass twice full scale and are bent by a soft knee under the host's limit (Open303 is unbounded).

    OPEN303=/path/to/Open303 sh plugins/acid/dev/build.sh && plugins/acid/dev/build/acid_compare [WAVDIR]

## Builds and cost

- Desktop: `cmake --build build --target kreisel_plugin_acid` -> `build/plugins_acid/acid.so` (copy into `~/.kreisel/plugins/`).
- FM-1: `python3 tools/kpl_build.py plugins/acid/acid.c` -> `build/fm1/plugins/acid.kpl`: 8496 bytes (3 flash sectors: the presets' words included), RAM 152 bytes (of the images' 2 KiB pool), 67 relocations.
- CPU on the FM-1 (fm1-emu 0.4.0, instructions, not cycles): an emulator-only firmware with Acid linked in and picked on track 1
  (then the same with Retro), a key held 1.5 s of a 2.5 s run: ISR instructions per emulated second 74.3 M (Acid), 71.8 M (Retro),
  17.7 M with no key. Acid takes 4.2 M/s more than one Retro voice: about 20.7 M/s, ~15 000 instructions a 32-sample block (~470 a
  sample, 1.25 Retro voices); ~47 us a block at 320 MHz. Declared `cost` 50: a mono engine counts once against the FM-1's 120 us a
  track. (The measurement firmware is not kept: a temporary patch, as `memos/2026-11-21_fm1_formant.md` measured Formant.)
- Tests: `tests/acid_test.c` (it registers in the FM-1's room, sounds, nothing overflows at the extremes, mono, the slide only on
  overlapping notes, the level does not follow the velocity, an accent does, a song finds it); `fune-plugin-check` passes
  (`fune_plugin_check_acid`).
