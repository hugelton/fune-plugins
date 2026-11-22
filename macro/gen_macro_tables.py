#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2016 Emilie Gillet (Mutable Instruments Plaits: the data and the formulas below)
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (the integer tables)
# The full MIT notice: plugins/macro/LICENSE-MIT.
"""Writes plugins/macro/macro_tables.h: the integer tables of MACRO.

    python3 plugins/macro/gen_macro_tables.py /path/to/eurorack/plaits/resources.cc

From Plaits' plaits/resources.cc, as they are (int16) or turned into integers (floats -> Q15, semitones -> cents):
  wav_integrated_waves (the wavetables, int16), lut_ws_* (the waveshaper's five curves, int16), lut_fold (Q15),
  lut_fm_frequency_quantizer (cents), lut_4x_downsampler_fir (Q15).
From Plaits' formulas, made for Fune's blocks (32 samples at 44.1 kHz; Plaits' are 12 at 48 kHz):
  the low-pass gate's decays (plaits/dsp/voice.cc, plaits/dsp/envelope.h), the noise filter's Q and gain
  (plaits/dsp/engine/noise_engine.cc), the chords (plaits/dsp/chords/chord_bank.cc), the organ registrations
  (plaits/dsp/engine/chord_engine.cc), the analog pair's intervals (plaits/dsp/engine/virtual_analog_engine.cc)."""
import math
import re
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent / "macro_tables.h"
K = (32 / 44100) / (12 / 48000)        # Plaits blocks in one of Fune's (2.9025)


def arr(src, name):
    m = re.search(r"const \w+ " + name + r"\[\] = \{(.*?)\};", src, re.S)
    if not m:
        sys.exit("not found: " + name)
    return [float(x) for x in m.group(1).replace("\n", " ").split(",") if x.strip()]


def q(x, one=32767):
    return max(-32768, min(32767, int(round(x * one))))


def emit(f, ctype, name, vals, per=12, comment=None):
    if comment:
        f.write("/* " + comment + " */\n")
    f.write("static const %s %s[%d] = {\n" % (ctype, name, len(vals)))
    for i in range(0, len(vals), per):
        f.write("    " + ", ".join(str(int(v)) for v in vals[i:i + per]) + ",\n")
    f.write("};\n")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    src = Path(sys.argv[1]).read_text()
    waves = [int(v) for v in arr(src, "wav_integrated_waves")]
    assert len(waves) == 192 * 132
    ws = [[int(v) for v in arr(src, n)] for n in
          ("lut_ws_inverse_tan", "lut_ws_inverse_sin", "lut_ws_linear", "lut_ws_bump", "lut_ws_double_bump")]
    fold = [q(v) for v in arr(src, "lut_fold")]
    assert len(fold) == 516
    fmq = [int(round(v * 100)) for v in arr(src, "lut_fm_frequency_quantizer")]
    fir = [q(v) for v in arr(src, "lut_4x_downsampler_fir")]

    # the low-pass gate, a value of DCAY (d = 0 .. 1) a row: Plaits' coefficients a block of 12 at 48 kHz, scaled to a block of
    # 32 at 44.1 kHz (K of Plaits' blocks: c -> 1 - (1 - c)^K)
    def per(c):
        return 1 - (1 - min(c, 0.999)) ** K
    short, tail, denv = [], [], []
    for i in range(128):
        d = i / 127
        sd = (200.0 * 12) / 48000 * 2 ** (-96.0 * d / 12)
        short.append(round(per(sd) * (1 << 30)))
        tail.append(round(K * (20.0 * 12) / 48000 * 2 ** (-72.0 * d / 12) * (1 << 30)))   # x 2^(COLR) - short, at the block
        denv.append(round((1 - min(sd * 2, 0.999)) ** K * 65536))                       # the decay envelope's factor, Q16
    hf2 = [round(2 ** (i / 127) * 16384) for i in range(128)]                            # 2^(COLR), Q14

    # the noise model: Q = 0.5 * 2^(10 MORF), the SVF's damping 1 / Q (Q16) and the input's gain 1 / sqrt(40 (0.5 + Q)) (Q16)
    nr, ng = [], []
    for i in range(128):
        qq = 0.5 * 2 ** (i / 127 * 10)
        nr.append(round(65536 / qq))
        ng.append(round(65536 / math.sqrt(40 * (0.5 + qq))))

    chords = [[0.00, 0.01, 11.99, 12.00], [0.00, 7.00, 7.01, 12.00], [0.00, 5.00, 7.00, 12.00], [0.00, 3.00, 7.00, 12.00],
              [0.00, 3.00, 7.00, 10.00], [0.00, 3.00, 10.00, 14.00], [0.00, 3.00, 10.00, 17.00], [0.00, 2.00, 9.00, 16.00],
              [0.00, 4.00, 11.00, 14.00], [0.00, 4.00, 7.00, 11.00], [0.00, 4.00, 7.00, 12.00]]
    regs = [[0.0, 1.0, 0.0, 0.0, 0.0, 0.0], [1.0, 0.0, 0.0, 0.0, 0.0, 0.0], [0.5, 0.0, 0.5, 0.0, 0.0, 0.0],
            [0.33, 0.0, 0.33, 0.0, 0.33, 0.0], [0.33, 0.0, 0.0, 0.33, 0.0, 0.33], [0.5, 0.0, 0.0, 0.0, 0.0, 0.5],
            [0.0, 0.5, 0.0, 0.0, 0.0, 0.5], [0.0, 0.1, 0.1, 0.0, 0.2, 0.6]]

    with OUT.open("w") as f:
        f.write("/* SPDX-License-Identifier: MIT\n"
                " * Copyright 2016 Emilie Gillet (Mutable Instruments Plaits: plaits/resources.cc and the formulas)\n"
                " * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (the integer forms)\n"
                " * The full MIT notice: plugins/macro/LICENSE-MIT. Written by gen_macro_tables.py: do not edit. */\n"
                "#ifndef MACRO_TABLES_H\n#define MACRO_TABLES_H\n#include <stdint.h>\n\n")
        emit(f, "int16_t", "MC_WAVES", waves, 12,
             "Plaits' wav_integrated_waves: 192 waves of 132 (128 + 4) samples, each integrated (the oscillator differentiates)")
        f.write("/* Plaits' lut_ws_inverse_tan, _inverse_sin, _linear, _bump, _double_bump (257 each), and the last again */\n")
        f.write("static const int16_t MC_WS[6][257] = {\n")
        for t in ws + [ws[4]]:
            f.write("    {" + ", ".join(str(v) for v in t) + "},\n")
        f.write("};\n")
        emit(f, "int16_t", "MC_FOLD", fold, 12, "Plaits' lut_fold (516: index -1 .. 514 of 512), Q15")
        emit(f, "int16_t", "MC_FM_RATIO", fmq, 13, "Plaits' lut_fm_frequency_quantizer: the modulator's interval, cents")
        emit(f, "int16_t", "MC_FIR", fir, 4, "Plaits' lut_4x_downsampler_fir, Q15")
        emit(f, "int32_t", "MC_LPG_SHORT", short, 8, "the gate's short decay a block, Q30, by DCAY (voice.cc short_decay)")
        emit(f, "int32_t", "MC_LPG_TAIL", tail, 8, "its tail a block at COLR 0, Q30 (voice.cc decay_tail, before - short)")
        emit(f, "uint16_t", "MC_DENV", [min(v, 65535) for v in denv], 12, "the decay envelope's factor a block, Q16 (voice.cc)")
        emit(f, "uint16_t", "MC_HF2", hf2, 12, "2^COLR, Q14 (the tail lengthens with the colour)")
        emit(f, "int32_t", "MC_NOISE_R", nr, 8, "the noise filter's damping 1/Q, Q16 (Q = 0.5 * 2^(10 MORF))")
        emit(f, "int32_t", "MC_NOISE_G", ng, 8, "its input gain 1/sqrt(40 (0.5 + Q)), Q16 (noise_engine.cc, without 1/sqrt(f0))")
        f.write("/* Plaits' chords (chord_bank.cc): four notes each, cents */\n")
        f.write("static const int16_t MC_CHORDS[11][4] = {\n")
        for c in chords:
            f.write("    {" + ", ".join(str(int(round(x * 100))) for x in c) + "},\n")
        f.write("};\n/* the organ / string registrations (chord_engine.cc), Q15 */\nstatic const int16_t MC_REGS[8][6] = {\n")
        for r in regs:
            f.write("    {" + ", ".join(str(q(x)) for x in r) + "},\n")
        f.write("};\n\n#endif\n")
    print("wrote", OUT, OUT.stat().st_size, "bytes")


if __name__ == "__main__":
    main()
