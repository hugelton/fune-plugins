#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2009 Robin Schmidt (www.rs-met.com): the formulas (Open303, rosic_TeeBeeFilter.h, rosic_Open303.cpp)
# Copyright (c) 2026 Leo Kuroshita (@kurogedelic), Hugelton Instruments: this generator, the integer forms
"""Acid's integer tables (acid_tables.h): Open303's float formulas evaluated once, here, for the integer DSP.

    python3 plugins/acid/tools/gen_acid_tables.py plugins/acid/acid_tables.h

The DSP runs the oscillator and the filter at twice the host's rate (ACID_OS = 2, 88.2 kHz; Open303 runs four times).
"""
import math
import sys

FS = 44100
OS = 2
FSO = FS * OS
CTL = 32                      # the host's block (samples at FS)

# the filter's coefficients against the cutoff, on a grid of an eighth of an octave from 200 Hz (Open303's floor)
L_STEP = 8                    # entries an octave
L_MIN = math.log2(200.0)
L_MAX = math.log2(20000.0)    # Open303's ceiling
NL = int(math.ceil((L_MAX - L_MIN) * L_STEP)) + 2


def tb303_coefs(f):
    """TeeBeeFilter::calculateCoefficientsApprox4, mode TB_303 (mystran & kunn), at the oversampled rate: b0, and k at full
    resonance before it is scaled (r = 1)."""
    wc = 2 * math.pi * f / FSO
    fx = wc * (1 / math.sqrt(2)) / (2 * math.pi)
    b0 = (0.00045522346 + 6.1922189 * fx) / (1.0 + 12.358354 * fx + 4.4156345 * (fx * fx))
    k = fx * (fx * (fx * (fx * (fx * (fx + 7198.6997) - 5837.7917) - 476.47308) + 614.95611) + 213.87126) + 16.998792
    return b0, k


def one_pole_hp_k(fc, fs):
    """Open303's OnePoleFilter HIGHPASS (dspguide): x = exp(-2 pi fc / fs). The DSP runs it as x - lp, lp a one-pole low-pass
    with the coefficient 1 - x (Q16); returns (1 - x) Q16 and b0 / x (the gain the form leaves out)."""
    x = math.exp(-2 * math.pi * fc / fs)
    return round((1 - x) * 65536), 0.5 * (1 + x) / x


def kf_time_ms(v):
    """the host's KF_TIME (draw_labels.c): 1 ms .. 10 s over 0 .. 127, exponential"""
    return 10000.0 ** (v / 127.0)


def main(path):
    out = []
    w = out.append
    w("/* SPDX-License-Identifier: MIT")
    w(" * Copyright (c) 2009 Robin Schmidt (www.rs-met.com): the formulas (Open303)")
    w(" * Copyright (c) 2026 Leo Kuroshita (@kurogedelic), Hugelton Instruments")
    w(" * Made by plugins/acid/tools/gen_acid_tables.py: do not edit. */")
    w("#ifndef ACID_TABLES_H")
    w("#define ACID_TABLES_H")
    w("#include <stdint.h>")
    w(f"#define ACID_OS {OS}")
    w(f"#define ACID_L_MIN {round(L_MIN * 65536)}   /* log2(200 Hz), Q16: the filter's floor */")
    w(f"#define ACID_L_MAX {round(L_MAX * 65536)}   /* log2(20 kHz), Q16: its ceiling */")
    w(f"#define ACID_L_SHIFT {16 - int(math.log2(L_STEP))}   /* an entry an eighth of an octave */")
    # the filter: b0 Q24, k at full resonance Q12
    w(f"/* the filter at log2(f) = ACID_L_MIN + i / {L_STEP} octave, run at {FSO} Hz: b0 (Q24), k at full resonance (Q12) */")
    w(f"static const int32_t ACID_B0[{NL}] = {{")
    rows = []
    ks = []
    for i in range(NL):
        f = 2 ** (L_MIN + i / L_STEP)
        b0, k = tb303_coefs(f)
        rows.append(str(round(b0 * (1 << 24))))
        ks.append(str(round(k * 4096)))
    for i in range(0, NL, 8):
        w("    " + ", ".join(rows[i:i + 8]) + ",")
    w("};")
    w(f"static const int32_t ACID_K[{NL}] = {{")
    for i in range(0, NL, 8):
        w("    " + ", ".join(ks[i:i + 8]) + ",")
    w("};")
    # resonance skew: (1 - exp(-3 r)) / (1 - exp(-3)), r = v / 127
    w("/* the resonance knob 0 .. 127 -> Open303's skewed resonance (1 - e^(-3r)) / (1 - e^-3), Q12 */")
    w("static const int16_t ACID_RSKEW[128] = {")
    rs = [str(round((1 - math.exp(-3 * v / 127)) / (1 - math.exp(-3)) * 4096)) for v in range(128)]
    for i in range(0, 128, 16):
        w("    " + ", ".join(rs[i:i + 16]) + ",")
    w("};")
    # the main envelope's decay a block: exp(-CTL / (tau fs)), Q16, for the KF_TIME values 73 .. 105 (198 .. 1995 ms)
    w("/* the main (filter) envelope's decay a block, Q16, for DCY's values 73 .. 105 (KF_TIME: ~200 .. 2000 ms) */")
    w("#define ACID_DCY_MIN 73")
    w("#define ACID_DCY_MAX 105")
    dc = [str(min(65535, round(math.exp(-CTL / (kf_time_ms(v) * 0.001 * FS)) * 65536))) for v in range(73, 106)]
    w("static const uint16_t ACID_DCY[33] = {")
    for i in range(0, 33, 11):
        w("    " + ", ".join(dc[i:i + 11]) + ",")
    w("};")
    acc = round(math.exp(-CTL / (200 * 0.001 * FS)) * 65536)
    w(f"#define ACID_DCY_ACCENT {acc}   /* an accented note's: 200 ms */")
    # slide: Open303's pitch slew limiter, a leaky integrator of time constant 0.2 x the slide time, at FS, a sample: 1 - c, Q16
    w("/* the slide (a leaky integrator on the frequency, time constant 0.2 x SLID), a sample at the host's rate: 1 - c, Q18, for")
    w(" * SLID's values 32 .. 86 (KF_TIME: ~10 .. 510 ms) */")
    w("#define ACID_SLD_MIN 32")
    w("#define ACID_SLD_MAX 86")
    sl = [str(round((1 - math.exp(-1 / (0.2 * kf_time_ms(v) * 0.001 * FS))) * 262144)) for v in range(32, 87)]
    w("static const uint16_t ACID_SLD[55] = {")
    for i in range(0, 55, 11):
        w("    " + ", ".join(sl[i:i + 11]) + ",")
    w("};")
    # fixed one-poles and block coefficients
    hp1, _ = one_pole_hp_k(44.486, FSO)
    hp2, _ = one_pole_hp_k(24.167, FS)
    fb, fbg = one_pole_hp_k(150.0, FSO)
    w(f"#define ACID_HP1_K {hp1}   /* the pre-filter high-pass, 44.5 Hz at the oversampled rate: its low-pass's 1 - x, Q16 */")
    w(f"#define ACID_HP2_K {hp2}   /* the post-filter high-pass, 24.2 Hz at the host's rate */")
    w(f"#define ACID_FB_K {fb}   /* the feedback high-pass, 150 Hz at the oversampled rate */")
    w(f"#define ACID_FB_G {round(fbg * 4096)}   /* its b0 / x, Q12 (folded into k) */")
    blk = lambda ms: round(math.exp(-CTL / (ms * 0.001 * FS)) * 65536)
    w(f"#define ACID_AMP_DEC {blk(1230)}   /* the amplitude's decay a block, Q16: 1230 ms */")
    w(f"#define ACID_AMP_REL {blk(1.0)}   /* its release, 1 ms */")
    w(f"#define ACID_AMP_REL_ACC {blk(50.0)}   /* an accented note's release, 50 ms */")
    w(f"#define ACID_RC2 {blk(15.0)}   /* the accent's sweep (the wow): a leaky integrator of 15 ms on the main envelope, a block, Q16 */")
    w(f"#define ACID_L_314 {round(math.log2(314.0) * 65536)}   /* CUT's range, 314 .. 2394 Hz (Open303's), log2 Q16 */")
    w(f"#define ACID_L_SPAN {round(math.log2(2394.0 / 314.0) * 65536)}")
    vi = [str(round(127 * 4096 / max(v, 1))) for v in range(128)]
    w("/* 127 / velocity, Q12 (the amp hook undoes the host's velocity) */")
    w("static const uint32_t ACID_VINV[128] = {")
    for i in range(0, 128, 16):
        w("    " + ", ".join(vi[i:i + 16]) + ",")
    w("};")
    dc = [str(round(2 * 4096 / math.sqrt(4 + d // 4))) for d in range(128)]
    w("/* DRV's make-up after its soft clip: 2 / sqrt(4 + DRV / 4), Q12 */")
    w("static const uint16_t ACID_DRV_COMP[128] = {")
    for i in range(0, 128, 16):
        w("    " + ", ".join(dc[i:i + 16]) + ",")
    w("};")
    w("#endif")
    open(path, "w").write("\n".join(out) + "\n")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "acid_tables.h")
