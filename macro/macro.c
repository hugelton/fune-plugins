/* SPDX-License-Identifier: MIT
 * Copyright 2016 Emilie Gillet (Mutable Instruments Plaits: the synthesis models this file is ported from)
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments (the fixed-point port, Fune's plugin)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. */
/* Macro, an outside engine plugin (id `macr`): eight synthesis models after Mutable Instruments Plaits (MIT), ported to integers
 * (the FM-1 has no FPU): one code for every device. Plaits' idea of the controls: a model, then HARMONICS, TIMBRE and MORPH, and a
 * low-pass gate (DECAY, COLOUR) pinged by each note. Its parameters, on P_E0 .. :
 *   SOUND  0 MODL (the model), 1 HARM, 2 TIMB, 3 MORF
 *   EDIT   4 DCAY (the gate's decay; at its top the gate stays open: the host's envelope shapes it), 5 COLR (VCA .. VCF),
 *          6 TENV (the decay envelope to TIMB, as Plaits' TIMBRE attenuverter with a trigger patched), 7 LVL
 * The velocity sets how far each note opens the gate (brighter and, through the host, louder). The host's FLT modulation moves
 * TIMB, its SHP modulation MORF.
 *
 * The models, each after its Plaits file (plaits/dsp/...):
 *   0 Pair   engine/virtual_analog_engine.cc (VA_VARIANT 2's OUT), oscillator/variable_shape_oscillator.h (the synced square),
 *            oscillator/variable_saw_oscillator.h
 *   1 Fold   engine/waveshaping_engine.cc, oscillator/oscillator.h (OSCILLATOR_SHAPE_SLOPE)
 *   2 FM     engine/fm_engine.cc, downsampler/4x_downsampler.h, oscillator/sine_oscillator.h (SinePM)
 *   3 Grain  engine/grain_engine.cc (its OUT), oscillator/grainlet_oscillator.h
 *   4 Harm   engine/additive_engine.cc (OUT: 24 harmonics), oscillator/harmonic_oscillator.h
 *   5 Wave   engine/wavetable_engine.cc (OUT), oscillator/wavetable_oscillator.h (Hermite, the differentiator)
 *   6 Chord  engine/chord_engine.cc (the divide-down voices), chords/chord_bank.cc, oscillator/string_synth_oscillator.h
 *   7 Noise  engine/noise_engine.cc (OUT), noise/clocked_noise.h, stmlib/dsp/filter.h (Svf, ProcessMultimodeLPtoHP)
 *   the gate voice.cc, envelope.h (LPGEnvelope, DecayEnvelope), fx/low_pass_gate.h; band-limiting stmlib/dsp/polyblep.h
 * Where it differs from Plaits (to fit integers, a voice's 11 words of state, the FM-1's time) the comment at the model says so.
 *
 * A voice's state (voice_t): ph[0..2] and s[0..2] the model's, s[3] flags (and the model it was set up for), s[4] the decay
 * envelope (high half) and the gate's last gain (low half), s[5] the vactrol (Q30), s[6], s[7] the gate's filter. A part's: the
 * Harm model's amplitudes and the Wave model's smoothed position (kept by part, 4 or 8 of them). Integers only; no division in
 * the sample loops (reciprocals once a block); 64-bit arithmetic once a block only. */
#include "kreisel_sdk.h"
#include <stddef.h>         /* (offsetof: macro_ui.c's attach) */
#include "kreisel_mul.h"   /* 64-bit products only through k_smul / k_umul (the FM-1's erratum: out of line there) */
#include "macro_tables.h"
#define SM(a, b) k_smul((int32_t)(a), (int32_t)(b))
#define UM(a, b) k_umul((uint32_t)(a), (uint32_t)(b))

enum { M_PAIR, M_FOLD, M_FM, M_GRAIN, M_HARM, M_WAVE, M_CHORD, M_NOISE, M_COUNT };
enum { E_MODL, E_HARM, E_TIMB, E_MORF, E_DCAY, E_COLR, E_TENV, E_LVL };

/* s[3]'s flags */
#define F_NEW 1u           /* a note began: the gate and the decay envelope are triggered at the next block */
#define F_RAMP 2u          /* the vactrol rises (Plaits' ramp_up_) */
#define F_HIGH1 4u         /* an oscillator's high_ */
#define F_HIGH2 8u
#define F_SET 16u          /* the model's state is set up (for the model in bits 8 ..) */
#define F_SYNC 32u          /* (Noise) the clock restarts with the note */
#define F_MODEL(f) (((f) >> 8) & 15u)

#define ONE15 32768
#define ONE30 (1 << 30)

/* ------------------------------------------------------------------ helpers --- */
static inline int32_t mc_abs(int32_t x) { return x < 0 ? -x : x; }
static inline int32_t p15(int32_t v) { return clamp(v, 0, 127) * 258; }   /* 0 .. 127 -> Q15 (0 .. 32766) */
static inline int32_t lerp15(int32_t a, int32_t b, int32_t f) { return a + (((b - a) * f) >> 15); }   /* |b - a| < 2^16 */
/* a * f (f Q15, 0 .. 32768) for any a: two products */
static inline int32_t mul15(int32_t a, int32_t f) { return (a >> 15) * f + (((a & 0x7FFF) * f) >> 15); }

/* the phase increment (2^32 a cycle) of a pitch in 1/16 semitone (MIDI x 16), any octave: the host's table, shifted */
static uint32_t inc16(int32_t p)
{
    int sh = 0;
    while (p < 0) { p += 192; sh--; }
    while (p > 2047) { p -= 192; sh++; }
    {
        const uint32_t i = pitch_inc((uint32_t)p);
        if (sh < 0) return i >> -sh;
        return i > (0x7FFFFFFFu >> sh) ? 0x7FFFFFFFu : i << sh;
    }
}
/* the note's increment moved by `ct` cents (with the host's residual detune) */
static inline uint32_t inc_ct(const vmod_t *m, int32_t ct)
{
    const int32_t p = m->pitch16 + ct * 16 / 100;
    if (p >= 0 && p <= 2047 && ct > -24000) return cents_inc(m->pitch16, ct, m->fine);
    return inc16(p);
}

/* a fraction of a sample, Q15, from how far a phase has run past an edge: excess / inc, by a reciprocal made once a block */
typedef struct { uint32_t r; uint8_t sh; } Rcp;
static inline void rcp_set(Rcp *c, uint32_t inc)
{
    uint8_t sh = 0;
    if (inc < 2) inc = 2;
    while (inc >= 65536u) { inc >>= 1; sh++; }
    c->sh = sh;
    c->r = 0x80000000u / inc;
}
static inline int32_t rcp_t(const Rcp *c, uint32_t ex)
{
    const uint32_t t = ((ex >> c->sh) * c->r) >> 16;
    return t > 32767u ? 32767 : (int32_t)t;
}

/* polyBLEP (stmlib/dsp/polyblep.h), t Q15: the step's residuals this sample and the next, and the integrated (a slope's) */
static inline int32_t blep_this(int32_t t) { return (t * t) >> 16; }
static inline int32_t blep_next(int32_t t) { t = ONE15 - t; return -((t * t) >> 16); }
static inline int32_t iblep_next(int32_t t)
{
    const int32_t t1 = t >> 1, t2 = (t1 * t1) >> 15, t4 = (t2 * t2) >> 15;
    return 6144 - t1 + ((3 * t2) >> 1) - t4;
}
static inline int32_t iblep_this(int32_t t) { return iblep_next(ONE15 - t); }

/* Hermite between a table's points (stmlib's InterpolateHermite), f Q15; the result scaled << 4 (the wavetables' small steps) */
static inline int32_t hermite4(const int16_t *x, int32_t f)
{
    const int32_t xm1 = x[0], x0 = x[1], x1 = x[2], x2 = x[3];
    const int32_t c = (x1 - xm1) >> 1, v = x0 - x1, w = c + v, a = w + v + ((x2 - x0) >> 1), b = w + a;
    const int32_t f12 = f >> 3;
    return ((((((a * f12) >> 12) - b) * f12 >> 12) + c) * f12 >> 8) + x0 * 16;
}

/* a part's own state, in the part's memory the host keeps for it (interface 1.5, engine_t.mem: the host's, not this plugin's RAM):
 * the voices of a part play the same values, so what depends only on them is worked out once a block (mc_block) */
typedef struct {
    uint16_t amp[24];          /* Harm: the harmonics' amplitudes, Q16, after the one-pole and the sum */
    int32_t wpos[3];           /* Wave: the position (x y z, Q16) after Plaits' first one-pole */
} MacroPart;
static void *(*mc_part_mem)(const void *part);   /* the host's (KHostApi.part_mem: macro_ui.c's attach) */
static MacroPart mc_fallback;                    /* (an older host, or none: one part's for all) */
static MacroPart *mc_part(const track_t *t)
{
    MacroPart *q = mc_part_mem ? (MacroPart *)mc_part_mem(t) : 0;
    return q ? q : &mc_fallback;
}

/* the controls of a block (Q15 0 .. 32767), and the note */
typedef struct {
    int32_t h, tm, mo;
    uint32_t inc;
    int32_t pitch16, fine;
    MacroPart *part;
} Ctl;

/* ------------------------------------------------------- 0 Pair (virtual analog) --- */
/* virtual_analog_engine.cc, VA_VARIANT 2, OUT: a square synced to the note (TIMB: its width, then the sync's sweep) and a
 * variable saw (MORF: the saw, its notch, the triangle) detuned by HARM (unison, fifths, octaves). Plaits ramps the gains and
 * the frequencies across its 12-sample blocks; here they step each 32-sample block. The edge's place uses the frequency (Plaits
 * also adds the width's change since the last sample). */
static const int16_t VA_INTERVALS[5] = {0, 701, 1201, 1901, 2401};
static inline int32_t squash15(int32_t x) { return ((((x * x) >> 15) * ((3 * ONE15 - 2 * x) >> 2)) >> 13); }
static int32_t va_detune_ct(int32_t h)   /* ComputeDetuning: cents */
{
    int32_t d = 2 * h + ((h * 205) >> 12) - 33587;   /* 2.05 h - 1.025 (Q15) */
    int32_t sign = 1, i, f, a, b;
    d = clamp(d, -ONE15, ONE15);
    if (d < 0) { sign = -1; d = -d; }
    d = 4 * d - (d >> 13);                     /* x 3.9999 */
    i = d >> 15; f = d & 0x7FFF;
    if (i > 3) { i = 3; f = 32767; }
    a = VA_INTERVALS[i]; b = VA_INTERVALS[i + 1];
    return sign * (a + (((b - a) * squash15(squash15(f))) >> 15));
}

static void m_pair(voice_t *v, int32_t *buf, uint32_t n, const Ctl *c, const vmod_t *m)
{
    const int32_t tm = c->tm, mo = c->mo;
    /* the square: width, sync, gain from TIMB */
    int32_t sq_pw = clamp(tm + ((tm * 1229) >> 12) - 4915, 164, 16384);     /* 1.3 t - 0.15, 0.005 .. 0.5 */
    const int32_t sd = tm - 16384, sync_ct = tm < 16384 ? 0 : (((sd * sd) >> 15) * 19200) >> 15;
    const int32_t sq_gain = tm >= 4096 ? ONE15 : tm * 8;
    /* the saw: width, shape, gain from MORF */
    int32_t saw_pw = mo < 16384 ? mo + 16384 : ONE15 - (mo - 16384) * 2;
    const int32_t saw_tri = clamp(10 * ONE15 - 21 * mo, 0, ONE15);
    const int32_t saw_gain = clamp(8 * (ONE15 - mo), 655, ONE15);
    const int32_t norm = (int32_t)((1u << 30) / (uint32_t)(sq_gain > saw_gain ? sq_gain : saw_gain));
    const int32_t gq = (int32_t)(UM((sq_gain * 9830) >> 15, norm) >> 15);          /* x 0.3 / max */
    const int32_t gs = (int32_t)(UM(saw_gain >> 1, norm) >> 15);        /* x 0.5 / max */
    /* the output 2 this - 1 of each (the saw's / 1.2): this_mix weights A (saw) and B (square), less their constant */
    const int32_t A = (gs * 27307) >> 14, B = 2 * gq;          /* 27307: 1 / 1.2 */
    const int32_t dc = (A + B) >> 1;
    uint32_t minc = c->inc > 0x40000000u ? 0x40000000u : c->inc;
    uint32_t sinc = sync_ct ? inc_ct(m, sync_ct) : minc;
    uint32_t winc = inc_ct(m, va_detune_ct(c->h));
    uint32_t mph = v->ph[0], sph = v->ph[1], wph = v->ph[2], i, pwq, pww;
    uint32_t flags = (uint32_t)v->s[3];
    int32_t next = v->s[0];
    int32_t up, down, ud_step, notch_lo;
    Rcp rm, rs, rw;
    if (sinc > 0x40000000u) sinc = 0x40000000u;
    if (winc > 0x40000000u) winc = 0x40000000u;
    rcp_set(&rm, minc); rcp_set(&rs, sinc); rcp_set(&rw, winc);
    /* widths as phases, kept 2 f from the edges (pw 0.5 when f >= 0.25) */
    saw_pw = (saw_pw * 36045) >> 15;                                  /* x 1.1 */
    saw_pw = clamp(saw_pw, 164, ONE15);
    {
        const uint32_t lo_s = 2u * sinc, lo_w = 2u * winc;   /* (each inc held under 2^30) */
        uint32_t q = (uint32_t)sq_pw << 17, w = saw_pw >= ONE15 ? 0xFFFFFFFFu : (uint32_t)saw_pw << 17;
        if (q < lo_s) q = lo_s;
        if (q > 0u - lo_s) q = 0u - lo_s;
        if (w < lo_w) w = lo_w;
        if (w > 0u - lo_w) w = 0u - lo_w;
        pwq = q; pww = w;
    }
    /* the saw's slopes (Q14 per unit phase: phase>>16 * up >> 15 is Q15) and the triangle's step at an edge, (up + down) f */
    up = (int32_t)(0x40000000u / ((pww >> 16) ? (pww >> 16) : 1));
    down = (int32_t)(0x40000000u / (((0u - pww) >> 16) ? ((0u - pww) >> 16) : 1));
    ud_step = (int32_t)(UM(up + down, winc) >> 31);                             /* Q15 */
    ud_step = (int32_t)(SM(ud_step, saw_tri) >> 15);
    notch_lo = ((39322 - (int32_t)(pww >> 17)) * (ONE15 - saw_tri)) >> 15;   /* (1.2 - pw) x notch */
    {
        const int32_t notch_hi = (39322 * (ONE15 - saw_tri)) >> 15;           /* 1.2 x notch */
        int high_w = (flags & F_HIGH1) != 0, high_q = (flags & F_HIGH2) != 0;
        for (i = 0; i < n; i++) {
            int32_t this_w, this_q, nw = 0, nq = 0, x, tri, ns;
            uint32_t o;
            /* the saw (variable_saw_oscillator.h); its this/next are carried as one mix with the square's (next) */
            this_w = 0; this_q = 0;
            o = wph; wph += winc;
            if (!high_w && wph >= pww && wph >= o) {
                const int32_t t = rcp_t(&rw, wph - pww);
                this_w += mul15(notch_lo, blep_this(t)) - mul15(ud_step, iblep_this(t));
                nw += mul15(notch_lo, blep_next(t)) - mul15(ud_step, iblep_next(t));
                high_w = 1;
            } else if (wph < o) {
                const int32_t t = rcp_t(&rw, wph);
                this_w += -mul15(notch_hi, blep_this(t)) + mul15(ud_step, iblep_this(t));
                nw += -mul15(notch_hi, blep_next(t)) + mul15(ud_step, iblep_next(t));
                high_w = 0;
            }
            if (wph < pww) {
                ns = (int32_t)(wph >> 17);
                tri = (int32_t)(((wph >> 16) * (uint32_t)up) >> 15);
            } else {
                ns = 39322;
                tri = ONE15 - (int32_t)((((wph - pww) >> 16) * (uint32_t)down) >> 15);
            }
            nw += (ns * (ONE15 - saw_tri) + tri * saw_tri) >> 15;   /* (weights of 1 in all: under 1.2 x 2^30) */
            /* the square synced to the note (variable_shape_oscillator.h, waveshape 1: square_amount 1) */
            {
                int reset = 0, tdr = 0, swrap;
                int32_t rt = 0;
                o = mph; mph += minc;
                if (mph < o) {
                    uint32_t sar, adv;
                    rt = rcp_t(&rm, mph);
                    adv = sinc - (uint32_t)mul15((int32_t)(sinc >> 1), rt) * 2u;   /* (1 - rt) f */
                    sar = sph + adv;
                    if (sar < sph) tdr = 1;
                    if (!high_q && sar >= pwq) tdr = 1;
                    if (sar >= pwq) { this_q -= blep_this(rt); nq -= blep_next(rt); }
                    reset = 1;
                }
                o = sph; sph += sinc; swrap = sph < o;
                if (tdr || !reset) {
                    int guard = 0;
                    for (;;) {
                        if (!high_q) {
                            if (!swrap && sph < pwq) break;
                            {
                                const int32_t t = rcp_t(&rs, sph - pwq);
                                this_q += blep_this(t); nq += blep_next(t);
                            }
                            high_q = 1;
                        }
                        if (high_q) {
                            if (!swrap) break;
                            swrap = 0;
                            {
                                const int32_t t = rcp_t(&rs, sph);
                                this_q -= blep_this(t); nq -= blep_next(t);
                            }
                            high_q = 0;
                        }
                        if (++guard > 3) break;
                    }
                }
                if (reset) { sph = (uint32_t)mul15((int32_t)(sinc >> 1), rt) * 2u; high_q = 0; }
                nq += sph < pwq ? 0 : ONE15;
            }
            /* the mix: this = what was carried, plus this sample's corrections */
            x = next + mul15(A, this_w) + mul15(B, this_q);
            next = mul15(A, nw) + mul15(B, nq);
            buf[i] = x - dc;
        }
        flags = (flags & ~(F_HIGH1 | F_HIGH2)) | (high_w ? F_HIGH1 : 0) | (high_q ? F_HIGH2 : 0);
    }
    v->ph[0] = mph; v->ph[1] = sph; v->ph[2] = wph;
    v->s[0] = next;
    v->s[3] = (int32_t)flags;
}

/* ------------------------------------------------------------------- 1 Fold --- */
/* waveshaping_engine.cc: a band-limited slope (MORF: its symmetry) through a waveshaper (HARM: five curves, crossfaded) and the
 * wavefolder (TIMB: its gain). Tame() (how rich the spectrum is) narrows both at high notes, as Plaits does. The shape and the
 * folder's gain ramp across the block from the last (Plaits' ParameterInterpolator). */
static int32_t tame15(uint32_t inc, int32_t harm_q8, int32_t order)   /* Tame(f0, harmonics, order), Q15 */
{
    const int64_t f = (int64_t)(UM(inc, harm_q8) >> 8), max_f = 0x80000000ll / order;
    int32_t a;
    if (f <= max_f) return ONE15;
    a = ONE15 - (int32_t)(((f - max_f) << 15) / (0x80000000ll - max_f));
    if (a <= 0) return 0;
    return (((a * a) >> 15) * a) >> 15;
}

static void m_fold(voice_t *v, int32_t *buf, uint32_t n, const Ctl *c)
{
    const uint32_t inc = c->inc > 0x40000000u ? 0x40000000u : c->inc;
    const int32_t h = c->h, tm = c->tm, mo = c->mo;
    const int32_t slope_q8 = 768 + ((mc_abs(mo - 16384) * 1280) >> 15);   /* 3 + |m - 0.5| 5 */
    const int32_t shape_amt = mc_abs(h - 16384) * 2;
    const int32_t sa_att = tame15(inc, slope_q8, 16);
    const int32_t wf_mult = (slope_q8 * ((3 * ONE15 + ((shape_amt * sa_att) >> 15) * 5) >> 4)) >> 11;
    const int32_t wf_att = tame15(inc, wf_mult, 12);
    const int32_t shape1 = 16384 + (((h - 16384) * sa_att) >> 15);   /* Q15 */
    const int32_t wfg1 = 983 + ((((tm * 15073) >> 15) * wf_att) >> 15);   /* 0.03 + 0.46 t att */
    const int32_t shape0 = v->s[1] ? v->s[2] : shape1, wfg0 = v->s[1] ? v->s[1] : wfg1;   /* (0: a fresh voice) */
    uint32_t pw = (uint32_t)(16384 + ((mo * 14746) >> 15)) << 17;     /* 0.5 + 0.45 m */
    uint32_t ph = v->ph[0], flags = (uint32_t)v->s[3], i;
    int32_t next = v->s[0], up, down, step;
    int high = (flags & F_HIGH1) != 0;
    Rcp r;
    rcp_set(&r, inc);
    if (pw < 2 * inc) pw = 2 * inc;
    if (pw > 0u - 2 * inc) pw = 0u - 2 * inc;
    up = (int32_t)(0x40000000u / ((pw >> 16) ? (pw >> 16) : 1));
    down = (int32_t)(0x40000000u / (((0u - pw) >> 16) ? ((0u - pw) >> 16) : 1));
    step = (int32_t)(UM(up + down, inc) >> 31);                                  /* (up + down) f, Q15 */
    for (i = 0; i < n; i++) {
        int32_t this_s = next, x, shp, si, sf, idx, ii, fi, a0, a1, mix, wfg, fidx;
        const uint32_t o = ph;
        int lt;
        ph += inc;
        lt = ph >= o && ph < pw;
        next = 0;
        if (high != lt) {
            const int32_t t = rcp_t(&r, ph - pw);
            this_s -= mul15(step, iblep_this(t));
            next -= mul15(step, iblep_next(t));
            high = lt;
        }
        if (ph < o) {
            const int32_t t = rcp_t(&r, ph);
            this_s += mul15(step, iblep_this(t));
            next += mul15(step, iblep_next(t));
            high = 1;
        }
        next += high ? (int32_t)(((ph >> 16) * (uint32_t)up) >> 15)
                     : ONE15 - (int32_t)((((ph - pw) >> 16) * (uint32_t)down) >> 15);
        x = clamp(2 * this_s - ONE15, -ONE15, 32767);
        /* the waveshaper: index 127 x + 128 into two of the five curves */
        shp = shape0 + (int32_t)(((shape1 - shape0) * (int32_t)i) >> 5);
        shp = 4 * shp - (shp >> 13);                                    /* x 3.9999 */
        si = shp >> 15; sf = shp & 0x7FFF;
        idx = 127 * x + (128 << 15);
        ii = (idx >> 15) & 255; fi = idx & 0x7FFF;
        a0 = lerp15(MC_WS[si][ii], MC_WS[si][ii + 1], fi);
        a1 = lerp15(MC_WS[si + 1][ii], MC_WS[si + 1][ii + 1], fi);
        mix = a0 + (((a1 - a0) * sf) >> 15);
        /* the folder: index mix g + 0.5 into lut_fold (512 points) */
        wfg = wfg0 + (((wfg1 - wfg0) * (int32_t)i) >> 5);
        fidx = ((mix * wfg) >> 15) + 16384;                                                /* Q15 of 0 .. 1 */
        fidx = clamp(fidx, 0, 32767) << 9;                                                  /* x 512, Q15 */
        buf[i] = hermite4(&MC_FOLD[(fidx >> 15)], fidx & 0x7FFF) >> 4;
    }
    v->ph[0] = ph;
    v->s[0] = next;
    v->s[1] = wfg1;
    v->s[2] = shape1;
    v->s[3] = (int32_t)((flags & ~F_HIGH1) | (high ? F_HIGH1 : 0));
}

/* --------------------------------------------------------------------- 2 FM --- */
/* fm_engine.cc: two sine operators at four times the rate, back down through its FIR; HARM the modulator's ratio (quantized),
 * TIMB the index (less at high notes, as Plaits), MORF the feedback (below the middle: the phase's, above: the modulator's own).
 * The sub-oscillator is Plaits' AUX: not here. */
static void m_fm(voice_t *v, int32_t *buf, uint32_t n, const Ctl *c, const vmod_t *m)
{
    const int32_t hi = c->h >> 8, hf = (c->h & 255) << 7;                      /* index into the 129 points (h x 128) */
    const int32_t ratio_ct = MC_FM_RATIO[hi] + (((MC_FM_RATIO[hi + 1] - MC_FM_RATIO[hi]) * hf) >> 15);
    const uint32_t cinc = c->inc >> 2;
    uint32_t minc = inc_ct(m, ratio_ct);
    const int32_t mn16 = c->pitch16 - 24 * 16 + ratio_ct * 16 / 100;           /* the modulator's note, 1/16 st */
    int32_t taming = ONE15 - (int32_t)((SM(mn16 - 72 * 16, 6554)) >> 7);   /* 1 - (note - 72) 0.025 */
    const int32_t fb = c->mo * 2 - ONE15;
    const int32_t pfb = fb < 0 ? (fb * fb) >> 16 : 0;      /* 0.5 fb^2 */
    const int32_t mfb = fb > 0 ? (fb * fb) >> 17 : 0;      /* 0.25 fb^2 */
    int32_t amount, prev = v->s[0], head = v->s[1], tail = 0;
    uint32_t cph = v->ph[0], mph = v->ph[1], i, j;
    minc >>= 2;                                                                /* (at the 4x rate) */
    taming = clamp(taming, 0, ONE15);
    taming = (taming * taming) >> 15;
    amount = (((c->tm * c->tm) >> 15) * taming) >> 15;      /* 2 t^2 tame, Q14 */
    for (i = 0; i < n; i++) {
        for (j = 0; j < 4; j++) {
            const int32_t k = (prev * pfb) >> 15;
            int32_t mod, car;
            mph += minc + (uint32_t)((int32_t)(minc >> 15) * k);
            cph += cinc;
            mod = sine_i(mph + ((uint32_t)(mfb * prev) << 2));
            car = sine_i(cph + ((uint32_t)(amount * mod) << 3));
            prev += ((car - prev) * 1638) >> 15;                                   /* ONE_POLE 0.05 */
            head += car * MC_FIR[3 - j];
            tail += car * MC_FIR[j];
        }
        buf[i] = head >> 15;
        head = tail; tail = 0;
    }
    v->ph[0] = cph; v->ph[1] = mph;
    v->s[0] = prev; v->s[1] = head;
}

/* ------------------------------------------------------------------ 3 Grain --- */
/* grain_engine.cc, OUT: two grainlets (a formant's sine under a carrier's window, restarted each period: band-limited at the
 * restart) a formant apart; TIMB the formant, HARM the second's interval (and, below its middle, the carrier's bleed), MORF the
 * window's shape. The two share the carrier (Plaits' two have the same frequency), so one carrier phase serves both. */
static void m_grain(voice_t *v, int32_t *buf, uint32_t n, const Ctl *c)
{
    const uint32_t f0 = c->inc > 0x20000000u ? 0x20000000u : c->inc;
    const int32_t p1 = (24 * 16) + ((c->tm * 84 * 16) >> 15);
    uint32_t f1 = inc16(p1), f2 = inc16(p1 - 24 * 16 + ((c->h * 48 * 16) >> 15));
    const int32_t bleed = c->h < 16384 ? ONE15 - 2 * c->h : 0;
    const int32_t bf = (bleed * (2 * ONE15 - bleed)) >> 15;
    const int32_t norm = (int32_t)((1u << 30) / (uint32_t)(ONE15 + bf));
    /* the window's shape: 0.33 + (m - 0.33) max(1 - 24 f0, 0) */
    const int32_t keep = clamp(ONE15 - (int32_t)((f0 >> 17) * 24), 0, ONE15);
    const int32_t shape = 10813 + (((c->mo - 10813) * keep) >> 15);
    const int32_t s3 = shape * 3, si = s3 >> 15, sf = s3 & 0x7FFF;
    uint32_t cph = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2], i, lim = 0, k = 0, bp = 0, s1 = 0, s2 = 0;
    int32_t next = v->s[0], dc = v->s[1], c_after, dcc;
    Rcp r;
    if (f1 > 0x40000000u) f1 = 0x40000000u;
    if (f2 > 0x40000000u) f2 = 0x40000000u;
    rcp_set(&r, f0);
    if (si == 0) {          /* phase (1 + t^3 15), held at 1, + 0.75 */
        const int32_t t = ONE15 - sf, t3 = (((t * t) >> 15) * t) >> 15;
        k = 4096u + (uint32_t)((t3 * 15) >> 3);                               /* Q12 */
        lim = (uint32_t)(0xFFFFFFFFull * 4096 / k);
    } else if (si == 1) {   /* a breakpoint 0.001 + 0.499 t^3: the first half of the sine before it, the second after */
        const int32_t t = ONE15 - sf, t3 = (((t * t) >> 15) * t) >> 15;
        const uint64_t b = 4294967ull + ((uint64_t)(uint32_t)(t3 * 16351) << 2);       /* Q32: 0.001 + 0.499 t3 */
        bp = (uint32_t)b;
        s1 = (uint32_t)((0x80000000ull << 16) / b);                           /* 0.5 / bp, Q16 */
        s2 = (uint32_t)((0x80000000ull << 16) / (0x100000000ull - b));        /* 0.5 / (1 - bp), Q16 */
    } else {                /* 0.25 + phase (0.5 + t^3 14.5), held at 0.75 */
        const int32_t t = si > 2 ? ONE15 : sf, t3 = (((t * t) >> 15) * t) >> 15;
        k = 2048u + (uint32_t)((t3 * 29) >> 4);                               /* Q12 */
        lim = (uint32_t)(0x80000000ull * 4096 / k);
    }
    c_after = si >= 2 ? 16384 : 0;      /* the window at the restart (phase 0); at phase 1 it is 0 for every shape */
    {   /* the DC blocker: a one-pole high-pass at 0.3 f0 (tan approximated: pi f) */
        const int32_t g = (int32_t)(UM(f0 >> 8, 61766) >> 24);   /* 0.3 pi f, Q16 (61766: 0.3 pi 2^16) */               /* pi f, Q16 */
        dcc = (int32_t)(((uint32_t)g << 15) / (uint32_t)(65536 + g));
    }
    for (i = 0; i < n; i++) {
        uint32_t o = cph, arg;
        int32_t this_s = next, win, g;
        next = 0;
        cph += f0;
        if (cph < o) {   /* the restart: the formants from 0, the step's residuals */
            const int32_t rt = rcp_t(&r, cph);
            /* the grainlets just before (the window is 0 at phase 1, whatever the formants) and just after (phase 0) */
            const int32_t disc = (c_after * (int32_t)(UM(2 * bf, norm) >> 16)) >> 14;
            this_s += mul15(disc, blep_this(rt));
            next += mul15(disc, blep_next(rt));
            ph1 = (uint32_t)mul15((int32_t)(f1 >> 1), rt) * 2u;
            ph2 = (uint32_t)mul15((int32_t)(f2 >> 1), rt) * 2u;
        } else {
            ph1 += f1; ph2 += f2;
        }
        /* the window at the carrier's phase */
        if (si == 0) arg = cph >= lim ? 0xC0000000u : (cph >> 12) * k + 0xC0000000u;
        else if (si == 1) arg = (cph < bp ? (cph >> 16) * s1 : 0x80000000u + ((cph - bp) >> 16) * s2) + 0xC0000000u;
        else arg = cph >= lim ? 0xC0000000u : 0x40000000u + (cph >> 12) * k;
        win = (sine_i(arg) + ONE15) >> 2;                                      /* 0 .. 0.5 */
        g = sine_i(ph1) + sine_i(ph2) + 2 * bf;
        next += (win * (mul15(g, norm) >> 1)) >> 14;   /* (g under 2^17, x norm: under 2^17; x win (2^14): under 2^31) */
        /* the DC blocker */
        {
            const int32_t vv = mul15(this_s - dc, dcc), lp = vv + dc;
            dc = lp + vv;
            buf[i] = (this_s - lp) * 2;   /* (x 2: Fune's level for this model, OUT_GAIN) */
        }
    }
    v->ph[0] = cph; v->ph[1] = ph1; v->ph[2] = ph2;
    v->s[0] = next; v->s[1] = dc;
}

/* ------------------------------------------------------------------- 4 Harm --- */
/* additive_engine.cc, OUT: 24 harmonics; TIMB the centre of the spectrum, MORF its width (the slope), HARM its bumps. As in Plaits
 * the amplitudes follow their targets through a slow one-pole and are then normalised; they are the part's (the voices of a part
 * play the same values), made once a block in mc_block. Each voice fades the harmonics near Nyquist (1 - 2 f) and plays them by
 * Plaits' recurrence (harmonic_oscillator.h), in Q28. TENV does not move this model's TIMB (its spectrum is the part's). */
static void harm_amps(const int16_t *p, MacroPart *part)
{
    const int32_t h = p15(p[P_E0 + E_HARM]), tm = p15(p[P_E0 + E_TIMB]), mo = p15(p[P_E0 + E_MORF]);
    const int32_t rs = ((ONE15 - ((h * 19661) >> 15)) * mo) >> 15;                          /* (1 - 0.6 b) m, Q15 */
    const int32_t slope = 655 + (int32_t)(SM((((rs * rs) >> 15) * rs) >> 15, 130417) >> 15);  /* 0.01 + 1.99 rs^3, Q16 */
    const int32_t bumps = ((h * h) >> 14) * 16;                                              /* 16 b^2, Q16 */
    const int32_t margin = (int32_t)((((int64_t)(0xFFFFFFFFu / (uint32_t)slope) - 65536) * 65536) / (65536 + bumps));   /* (1/slope - 1) / (1 + bumps) */
    const int32_t center = (int32_t)(SM(tm, 23 * 65536 + margin) >> 15) - margin / 2;
    int32_t gains[24];
    int64_t sum = 66;                                                                        /* 0.001 */
    uint16_t *a = part->amp;
    int i;
    for (i = 0; i < 24; i++) {
        int32_t d = i * 65536 - center, g;
        int64_t order;
        if (d < 0) d = -d;
        order = SM(d, slope) >> 16;                                                         /* Q16 */
        if (order >= 65536) { gains[i] = 0; continue; }
        g = 2 * (65536 - (int32_t)order);                                                    /* (gain + |gain|) */
        g = (int32_t)(SM(g, g) >> 16);
        {
            const int32_t b = 16384 + (int32_t)(SM((int32_t)order, bumps) >> 16);           /* 0.25 + order bumps, Q16 cycles */
            g = (int32_t)(SM(g, 65536 + 2 * sine_i((uint32_t)b << 16)) >> 16);
        }
        g = (int32_t)(SM(g, g) >> 16);
        g = (int32_t)(SM(g, g) >> 16);                                                       /* (under 2^28) */
        gains[i] = g;
    }
    for (i = 0; i < 24; i++) {   /* ONE_POLE(amplitude, gain, 0.001) at Plaits' rate: 0.0029 a block here (190 / 65536) */
        int32_t x = a[i] + (int32_t)(SM(gains[i] - a[i], 190) >> 16);
        if (x < 0) x = 0;
        gains[i] = x;
        sum += x;
    }
    {   /* normalised: a / sum, Q16 (a reciprocal of the sum brought under 2^16) */
        int sh = 0;
        uint32_t s32, rcp;
        while ((sum >> sh) >= 65536) sh++;
        s32 = (uint32_t)(sum >> sh);
        rcp = 0x80000000u / (s32 ? s32 : 1);
        for (i = 0; i < 24; i++) {
            const uint64_t x = UM((uint32_t)gains[i] >> sh, rcp) >> 15;
            a[i] = (uint16_t)(x > 65535 ? 65535 : x);
        }
    }
}

static void m_harm(voice_t *v, int32_t *buf, uint32_t n, const Ctl *c)
{
    const uint16_t *a = c->part->amp;
    const uint32_t inc = c->inc >= 0x80000000u ? 0x7FFFFFFFu : c->inc;
    int32_t am[24], top = 0, k;
    uint32_t ph = v->ph[0], i;
    for (k = 0; k < 24; k++) {   /* the amplitude less near Nyquist: a (1 - 2 f (k + 1)) */
        const uint64_t f = UM(inc, k + 1);
        int32_t g;
        if (f >= 0x80000000ull) { am[k] = 0; continue; }
        g = 32768 - (int32_t)(f >> 16);
        am[k] = (int32_t)(UM(a[k], g) >> 16);                                 /* Q15 */
        if (am[k] > 2) top = k + 1;
    }
    for (i = 0; i < n; i++) {
        const int32_t x = sine_i(ph);
        int32_t prev = 1 << 28, cur = x * 8192, sum = 0, tmp;
        ph += inc;
        for (k = 0; k < top; k++) {
            sum += am[k] * (cur >> 13);
            tmp = cur;
            cur = 2 * ((cur >> 15) * x + (((cur & 0x7FFF) * x) >> 15)) - prev;
            prev = tmp;
        }
        buf[i] = sum >> 15;
    }
    v->ph[0] = ph;
}

/* ------------------------------------------------------------------- 5 Wave --- */
/* wavetable_engine.cc, OUT: four banks of 8 x 8 waves (the fourth Plaits' shuffle of the others), integrated: read by Hermite,
 * crossfaded in x (TIMB), y (MORF) and the bank z (HARM: 0 .. 3 .. 0 there and back, its upper half quantized to the waves),
 * then differentiated (the band-limit). The first one-pole of the position is the part's (mc_block); the second, which follows
 * the note's pitch, the voice's. */
static const uint8_t BANK3[64] = {   /* Plaits' fourth bank: wave (192 + i) * 101 % 192 */
#define B3(i) (uint8_t)(((192 + (i)) * 101) % 192)
    B3(0), B3(1), B3(2), B3(3), B3(4), B3(5), B3(6), B3(7), B3(8), B3(9), B3(10), B3(11), B3(12), B3(13), B3(14), B3(15),
    B3(16), B3(17), B3(18), B3(19), B3(20), B3(21), B3(22), B3(23), B3(24), B3(25), B3(26), B3(27), B3(28), B3(29), B3(30), B3(31),
    B3(32), B3(33), B3(34), B3(35), B3(36), B3(37), B3(38), B3(39), B3(40), B3(41), B3(42), B3(43), B3(44), B3(45), B3(46), B3(47),
    B3(48), B3(49), B3(50), B3(51), B3(52), B3(53), B3(54), B3(55), B3(56), B3(57), B3(58), B3(59), B3(60), B3(61), B3(62), B3(63),
#undef B3
};
static inline const int16_t *wave_at(int x, int y, int z)
{
    int w = x + y * 8;
    if (z >= 4) z = 7 - z;
    w = z == 3 ? BANK3[w] : w + z * 64;
    return &MC_WAVES[w * 132];
}
static void wave_pos(const int16_t *p, MacroPart *part)
{
    int32_t *w = part->wpos;
    const int32_t tx = p15(p[P_E0 + E_TIMB]) * 14 - (p15(p[P_E0 + E_TIMB]) >> 12);   /* x 6.9999, Q16 */
    const int32_t ty = p15(p[P_E0 + E_MORF]) * 14 - (p15(p[P_E0 + E_MORF]) >> 12);
    const int32_t tz = p15(p[P_E0 + E_HARM]) * 14 - (p15(p[P_E0 + E_HARM]) >> 12);
    w[0] += mul15(tx - w[0], 15729);   /* ONE_POLE 0.2 at Plaits' rate: 0.48 a block */
    w[1] += mul15(ty - w[1], 15729);
    w[2] += mul15(tz - w[2], 4522);    /* 0.05: 0.138 */
}
static inline int32_t wclamp16(int32_t frac, int32_t q)   /* x += q (Clamp(x, 16) - x), Q16 */
{
    int32_t c = ((frac - 32768) * 16);
    c = clamp(c, -32768, 32768) + 32768;
    return frac + mul15(c - frac, q >> 1);
}
static void m_wave(voice_t *v, int32_t *buf, uint32_t n, const Ctl *c)
{
    const int32_t *w = c->part->wpos;
    const uint32_t inc = c->inc > 0x3C000000u ? 0x3C000000u : c->inc;   /* (under 0.95 x 0.25 cycle) */
    const int32_t q = clamp(w[2] - 3 * 65536, 0, 65536);               /* quantization, Q16 */
    int32_t tgt[3], lpc, cut, k, xl = v->s[0], yl = v->s[1], zl = v->s[2], prev = (int32_t)v->ph[1], lp = (int32_t)v->ph[2];
    uint32_t ph = v->ph[0], i, g2;
    for (k = 0; k < 3; k++) tgt[k] = (w[k] & ~0xFFFF) + wclamp16(w[k] & 0xFFFF, q);
    {   /* the second one-pole's coefficient: clamp(2 f0 (4 - 3 q), 0.01, 0.1), Q15 */
        const int64_t x = (int64_t)(UM(inc, 4 * 65536 - 3 * q) >> 32);    /* 2 f (4 - 3q), Q15 */
        lpc = clamp((int32_t)(x > 32767 ? 32767 : x), 328, 3277);       /* (a sample's, as Plaits': the rates are near enough) */
    }
    cut = inc >= (1u << 25) ? 32767 : (int32_t)(inc >> 10);           /* min(128 f0, 1), Q15 */
    /* the gain (0.95 - f0) / (f0 131072) for the table's units, times 2048 for its Q4 values -> Q15: (0.95 - f) / (64 f), Q12 */
    {
        const uint32_t fq = inc ? inc : 1;
        const uint64_t x = ((uint64_t)(4080218931u - fq) << 6) / fq;
        g2 = (uint32_t)(x > 0x7FFFFFFFull ? 0x7FFFFFFFull : x);
    }
    if (!(xl | yl | zl) && !ph) { xl = tgt[0]; yl = tgt[1]; zl = tgt[2]; }   /* (a new voice starts where the part is) */
    for (i = 0; i < n; i++) {
        int32_t x, y, z, xi, yi, zi, xf, yf, zf, pi, pf, z1, mix, a, b, d;
        xl += mul15(tgt[0] - xl, lpc);
        yl += mul15(tgt[1] - yl, lpc);
        zl += mul15(tgt[2] - zl, lpc);
        x = xl; y = yl; z = zl;
        xi = x >> 16; yi = y >> 16; zi = z >> 16;
        xf = (x & 0xFFFF) >> 1; yf = (y & 0xFFFF) >> 1; zf = (z & 0xFFFF) >> 1;
        if (xi > 6) { xi = 6; xf = 32767; }
        if (yi > 6) { yi = 6; yf = 32767; }
        if (zi > 6) { zi = 6; zf = 32767; }
        ph += inc;
        pi = (int32_t)(ph >> 25); pf = (int32_t)((ph >> 10) & 0x7FFF);
        z1 = zi + 1;
        {
            const int32_t a00 = hermite4(wave_at(xi, yi, zi) + pi, pf), a10 = hermite4(wave_at(xi + 1, yi, zi) + pi, pf);
            const int32_t a01 = hermite4(wave_at(xi, yi + 1, zi) + pi, pf), a11 = hermite4(wave_at(xi + 1, yi + 1, zi) + pi, pf);
            const int32_t b00 = hermite4(wave_at(xi, yi, z1) + pi, pf), b10 = hermite4(wave_at(xi + 1, yi, z1) + pi, pf);
            const int32_t b01 = hermite4(wave_at(xi, yi + 1, z1) + pi, pf), b11 = hermite4(wave_at(xi + 1, yi + 1, z1) + pi, pf);
            const int32_t a0 = a00 + mul15(a10 - a00, xf), a1 = a01 + mul15(a11 - a01, xf);
            const int32_t b0 = b00 + mul15(b10 - b00, xf), b1 = b01 + mul15(b11 - b01, xf);
            a = a0 + mul15(a1 - a0, yf);
            b = b0 + mul15(b1 - b0, yf);
            mix = a + mul15(b - a, zf);
        }
        d = mix - prev;                       /* the differentiator: (s - previous), one-pole at min(128 f0, 1) */
        prev = mix;
        lp += mul15(d - lp, cut);
        buf[i] = mulsh_32(lp, g2, 12);
    }
    v->ph[0] = ph; v->ph[1] = (uint32_t)prev; v->ph[2] = (uint32_t)lp;
    v->s[0] = xl; v->s[1] = yl; v->s[2] = zl;
}

/* ------------------------------------------------------------------ 6 Chord --- */
/* chord_engine.cc and chord_bank.cc: four notes of a chord (HARM: eleven chords) and their inversions (TIMB), each a divide-down
 * organ / string voice (string_synth_oscillator.h: saws an octave apart, band-limited) whose registration MORF sweeps across all
 * eight of Plaits' (Plaits' MORF goes on, past its middle, to wavetable voices: not here, their state would not fit a voice). */
static void m_chord(voice_t *v, int32_t *buf, uint32_t n, const Ctl *c, const vmod_t *m)
{
    const int chord = clamp((c->h * 1436) >> 22, 0, 10);   /* x 1.02 x 11 (1436: 11.22 x 2^7) */
    const int32_t inv = c->tm * 20, ii = inv >> 15, frac = inv & 0x7FFF;
    const int rot = ii / 4, rn = ii % 4;
    int32_t cents[5] = {0, 0, 0, 0, 0}, amp[5] = {0, 0, 0, 0, 0}, reg[7], g[5][4];
    uint32_t pinc[5], ph[5], i;
    int k, sh[5];
    int32_t next = v->s[2];
    Rcp r[5];
    for (k = 0; k < 4; k++) {   /* ComputeChordInversion */
        const int32_t base = MC_CHORDS[chord][k] + ((3 + ii - k) / 4 - 2) * 1200;
        const int tv = (k - rot + 25) % 5, pv = (tv + 4) % 5;
        if (k == rn) {
            cents[tv] = base; cents[pv] = base + 1200;
            amp[pv] = frac >> 2; amp[tv] = (ONE15 - frac) >> 2;
        } else if (k < rn) {
            cents[pv] = base; amp[pv] = 8192;
        } else {
            cents[tv] = base; amp[tv] = 8192;
        }
    }
    {   /* the registration: MORF across Plaits' eight (ComputeRegistration) */
        const int32_t rr = 7 * (ONE15 - c->mo) - ((ONE15 - c->mo) >> 10);   /* x 6.999 */
        const int ri = rr >> 15;
        const int32_t rf = rr & 0x7FFF;
        for (k = 0; k < 6; k++) reg[k] = MC_REGS[ri][k] + (((MC_REGS[ri + 1 > 7 ? 7 : ri + 1][k] - MC_REGS[ri][k]) * rf) >> 15);
        reg[6] = 0;
    }
    ph[0] = v->ph[0]; ph[1] = v->ph[1]; ph[2] = v->ph[2]; ph[3] = (uint32_t)v->s[0]; ph[4] = (uint32_t)v->s[1];
    for (k = 0; k < 5; k++) {
        uint32_t f = inc_ct(m, cents[k] - 3);                         /* f0 x 0.998 */
        int32_t dd = clamp(4 * ONE15 - (int32_t)(f >> 12), 0, ONE15), gain, rg[7], j, s = 0;
        gain = (amp[k] * dd) >> 15;
        while (f > 0x10000000u) { f >>= 1; s += 2; }   /* 8 f under 0.5: else play an octave lower, the registration up */
        sh[k] = s;
        pinc[k] = f;
        if (s >= 8 || !gain) { g[k][0] = g[k][1] = g[k][2] = g[k][3] = 0; sh[k] = 8; continue; }
        for (j = 0; j < 7; j++) rg[j] = j < s ? 0 : reg[j - s];
        g[k][0] = ((rg[0] + 2 * rg[1]) * gain) >> 15;
        g[k][1] = ((rg[2] - rg[1] + 2 * rg[3]) * gain) >> 15;
        g[k][2] = ((rg[4] - rg[3] + 2 * rg[5]) * gain) >> 15;
        g[k][3] = ((rg[6] - rg[5]) * gain) >> 15;
        rcp_set(&r[k], f);
    }
    for (i = 0; i < n; i++) {
        int32_t this_s = next;
        next = 0;
        for (k = 0; k < 5; k++) {
            const uint32_t o = ph[k], p = ph[k] + pinc[k];
            int32_t s8, s4, s2, s1;
            if (sh[k] >= 8) { ph[k] = p; continue; }
            ph[k] = p;
            if ((p >> 29) != (o >> 29)) {   /* a segment's edge: the saws that restart there */
                const uint32_t seg = p >> 29;
                int32_t disc = 0;
                if (p < o) disc -= g[k][0];
                if ((seg & 3) == 0) disc -= g[k][1];
                if ((seg & 1) == 0) disc -= g[k][2];
                disc -= g[k][3];
                if (disc) {
                    const int32_t t = rcp_t(&r[k], p & 0x1FFFFFFFu);
                    this_s += mul15(disc, blep_this(t));
                    next += mul15(disc, blep_next(t));
                }
            }
            s8 = (int32_t)(p >> 17) - 16384;
            s4 = (int32_t)((p << 1) >> 17) - 16384;
            s2 = (int32_t)((p << 2) >> 17) - 16384;
            s1 = (int32_t)((p << 3) >> 17) - 16384;
            next += (s8 * g[k][0] + s4 * g[k][1] + s2 * g[k][2] + s1 * g[k][3]) >> 15;
        }
        buf[i] = 2 * this_s;
    }
    v->ph[0] = ph[0]; v->ph[1] = ph[1]; v->ph[2] = ph[2]; v->s[0] = (int32_t)ph[3]; v->s[1] = (int32_t)ph[4];
    v->s[2] = next;
}

/* ------------------------------------------------------------------ 7 Noise --- */
/* noise_engine.cc, OUT: clocked noise (TIMB: its clock, from a sample hold up to plain white noise) through a resonant filter at
 * the note's pitch, low-pass to band-pass to high-pass (HARM), its resonance MORF. Plaits ends it with a limiter; here a soft
 * knee. The note restarts the clock (as Plaits' trigger). */
static int32_t tan_q16(uint32_t inc)   /* tan(pi f), Q16 (stmlib's FREQUENCY_ACCURATE series), f = inc / 2^32 held under 0.4 */
{
    static const int32_t C[6] = {205887, 677339, 2675225, 10564751, 5666376, 183493163};   /* x pi^k, Q16 */
    const int32_t f = (int32_t)((inc > 0x66666666u ? 0x66666666u : inc) >> 8);           /* Q24 */
    const int32_t f2 = (int32_t)(SM(f, f) >> 24);
    int32_t acc = C[5];
    int k;
    for (k = 4; k >= 0; k--) acc = C[k] + (int32_t)(SM(acc, f2) >> 24);
    return (int32_t)(SM(acc, f) >> 24);
}
static void m_noise(voice_t *v, int32_t *buf, uint32_t n, const Ctl *c)
{
    const uint32_t f0 = c->inc;
    const int32_t cp = ((c->tm * 152 * 16) >> 15) - 24 * 16;
    const uint32_t cf = inc16(cp);
    const int32_t raw_amt = cf <= 0x40000000u ? 0 : clamp((int32_t)((cf - 0x40000000u) >> 15), 0, ONE15);
    const int mi = clamp(c->mo >> 8, 0, 127);
    /* the SVF's coefficients, Q14 (mulsh_32's bound: a coefficient under 2^18 with a shift of 14) */
    const int32_t r16 = MC_NOISE_R[mi];
    const int32_t g16 = tan_q16(f0);
    const int32_t h14 = (int32_t)((1ull << 30) / (uint32_t)(65536 + (int32_t)(SM(r16, g16) >> 16) + (int32_t)(SM(g16, g16) >> 16)));
    const int32_t g14 = g16 >> 2, rg = (r16 + g16) >> 2;
    /* the input's gain MC_NOISE_G / sqrt(f0), Q12 */
    int32_t gain;
    const int32_t mode = c->h;
    const int32_t hp_g = mode > 16384 ? -(2 * mode - ONE15) : 0;                       /* min(1 - 2 mode, 0) */
    const int32_t bp_g = ONE15 - 2 * mc_abs(mode - 16384);
    const int32_t lp_g = mode < 16384 ? ONE15 - 2 * mode : 0;
    uint32_t ph = v->ph[0], i;
    int32_t st = v->s[0], sample = v->s[1], next = v->s[2], s1 = (int32_t)v->ph[1], s2 = (int32_t)v->ph[2];
    Rcp r;
    rcp_set(&r, cf ? cf : 1);
    if (!st) st = (int32_t)(0x9E3779B9u ^ (v->age * 2654435761u));   /* (xorshift never leaves 0: a seed, another a note) */
    if ((uint32_t)v->s[3] & F_SYNC) { ph = 0u - cf; v->s[3] = (int32_t)((uint32_t)v->s[3] & ~F_SYNC); }   /* the note restarts the clock */
    {
        uint32_t x = f0 ? f0 : 1, y = 1u << 16, z;
        for (z = 0; z < 20; z++) y = (y + x / y) >> 1;                                /* sqrt(inc) */
        while (UM(y, y) > x) y--;
        gain = (int32_t)((((uint32_t)MC_NOISE_G[mi] << 12)) / (y ? y : 1));       /* G 2^16 / sqrt(inc): Q12 */
    }
    for (i = 0; i < n; i++) {
        const int32_t raw = (int32_t)(noise32(&st) >> 17) - 16384;                    /* Q14: +-0.5 -> x2 below */
        int32_t this_s = next, in, hp, bp, lp, gh, gb, o;
        const uint32_t old = ph;
        next = 0;
        ph += cf;
        if (ph < old) {
            const int32_t t = rcp_t(&r, ph), disc = raw * 2 - sample;
            this_s += mul15(disc, blep_this(t));
            next += mul15(disc, blep_next(t));
            sample = raw * 2;
        }
        next += sample;
        o = this_s + mul15(raw * 2 - this_s, raw_amt);
        in = (o >> 4) * gain >> 8;   /* (o under 2^17, gain Q12 under 2^16) */
        hp = mulsh_32(in - mulsh_32(s1, (uint32_t)rg, 14) - s2, (uint32_t)h14, 14);
        gh = mulsh_32(hp, (uint32_t)g14, 14);
        bp = gh + s1; s1 = gh + bp;
        gb = mulsh_32(bp, (uint32_t)g14, 14);
        lp = gb + s2; s2 = gb + lp;
        buf[i] = soft_knee(mul15(hp, hp_g) + mul15(bp, bp_g) + mul15(lp, lp_g), 24000);
    }
    v->ph[0] = ph; v->ph[1] = (uint32_t)s1; v->ph[2] = (uint32_t)s2;
    v->s[0] = st; v->s[1] = sample; v->s[2] = next;
}

/* ------------------------------------------------------------- the voice --- */
/* each model's gain: Plaits' (0.8 0.7 0.6 0.7 0.8 0.6 0.8, Noise 1.0 with a limiter), evened out so that a held note sounds about
 * as loud in every model (Pair 1.0, Grain 1.0 and x 2 in m_grain, Wave 0.84, Chord 0.68, Noise 0.55: Fune's choice) */
static const int32_t OUT_GAIN[M_COUNT] = {32767, 22938, 19661, 32767, 26214, 27525, 22282, 18022};

static void mc_note_on(track_t *t, voice_t *v)
{
    (void)t;
    v->s[3] = (int32_t)(((uint32_t)v->s[3] & ~F_RAMP) | F_NEW | F_SYNC);
}

static void mc_block(track_t *t)
{
    const int model = clamp(t->p[P_E0 + E_MODL], 0, M_COUNT - 1);
    if (model == M_HARM) harm_amps(t->p, mc_part(t));
    else if (model == M_WAVE) wave_pos(t->p, mc_part(t));
}

/* the model alone, Q15 (Plaits' OUT, x 32768): the gate, the gains and the host's amplitude come after */
static void mc_model(int model, voice_t *v, int32_t *buf, uint32_t n, const Ctl *c, const vmod_t *m)
{
    uint32_t f = (uint32_t)v->s[3];
    if (!(f & F_SET) || F_MODEL(f) != (uint32_t)model) {   /* a new model on this voice: its state from rest */
        v->ph[0] = v->ph[1] = v->ph[2] = 0;
        v->s[0] = v->s[1] = v->s[2] = 0;
        f = (f & ~(0xF00u | F_HIGH1 | F_HIGH2)) | ((uint32_t)model << 8) | F_SET | (model == M_FOLD ? F_HIGH1 : 0);
        v->s[3] = (int32_t)f;
    }
    switch (model) {
    case M_PAIR: m_pair(v, buf, n, c, m); break;
    case M_FOLD: m_fold(v, buf, n, c); break;
    case M_FM: m_fm(v, buf, n, c, m); break;
    case M_GRAIN: m_grain(v, buf, n, c); break;
    case M_HARM: m_harm(v, buf, n, c); break;
    case M_WAVE: m_wave(v, buf, n, c); break;
    case M_CHORD: m_chord(v, buf, n, c, m); break;
    default: m_noise(v, buf, n, c); break;
    }
}

static void mc_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    const int model = clamp(p[P_E0 + E_MODL], 0, M_COUNT - 1);
    const int dcay = clamp(p[P_E0 + E_DCAY], 0, 127), colr = clamp(p[P_E0 + E_COLR], 0, 127);
    const int32_t tenv = clamp(p[P_E0 + E_TENV], -64, 63);
    const int32_t level = (OUT_GAIN[model] * p15(p[P_E0 + E_LVL])) >> 15;
    const int open = dcay >= 127;
    uint32_t f = (uint32_t)v->s[3], i;
    int32_t denv = (int32_t)((uint32_t)v->s[4] >> 16), g0 = (int32_t)((uint32_t)v->s[4] & 0xFFFF), g1;
    int32_t vac = v->s[5], s1 = v->s[6], s2 = v->s[7];
    int32_t buf[CTL];
    int32_t gq14 = 0, rgq14 = 0, hq14 = 0, bleed = 0;
    Ctl c;
    if (n > CTL) n = CTL;
    /* the trigger: the gate and the decay envelope (voice.cc) */
    if (f & F_NEW) {
        f = (f & ~F_NEW) | F_RAMP;
        denv = 32767;
        if (!(f & F_SET)) vac = 0, s1 = s2 = 0, g0 = 0;
    }
    denv = (int32_t)(((uint32_t)denv * MC_DENV[dcay]) >> 16);
    /* the controls: HARM, TIMB (+ the host's FLT modulation, + TENV x the decay envelope), MORF (+ the host's SHP) */
    c.h = p15(p[P_E0 + E_HARM]);
    c.tm = clamp(p15(p[P_E0 + E_TIMB]) + m->cutoff + ((tenv * denv) >> 6), 0, 32767);
    c.mo = clamp(p15(p[P_E0 + E_MORF]) + m->shape, 0, 32767);
    c.inc = m->inc; c.pitch16 = m->pitch16; c.fine = m->fine;
    c.part = mc_part(t);
    /* the gate (envelope.h LPGEnvelope, ProcessPing), its peak by the velocity: 0.4 .. 1 */
    if (open) {
        g1 = 32767;
        vac = ONE30;
        f &= ~F_RAMP;
    } else {
        const int32_t peak = (13107 + clamp(v->vel, 0, 127) * 155) << 15;
        const int32_t s2q = (int32_t)(SM(vac, vac) >> 30), s4 = (int32_t)(SM(s2q, s2q) >> 30);
        int64_t level_q = 0, coef;
        if (f & F_RAMP) {
            const uint32_t att = m->inc >= (1u << 27) ? (1u << 31) : m->inc << 4;   /* f x 2 x 32 a block, Q30 */
            vac = (uint32_t)vac + att >= (uint32_t)peak ? peak : vac + (int32_t)att;
            if (vac >= peak) f &= ~F_RAMP;
            level_q = (f & F_RAMP) ? vac : 0;
        }
        if (level_q - vac > 0) coef = 998600474;                               /* 0.6 at Plaits' rate: 0.93 */
        else {
            const int64_t short_d = MC_LPG_SHORT[dcay];
            /* (the tail is negative when the decay is short: the fall slows as the vactrol nears 0; short + tail stays > 0) */
            const int64_t tail = (SM(MC_LPG_TAIL[dcay], MC_HF2[colr]) >> 14) - short_d;
            coef = short_d + (SM(ONE30 - s4, tail) >> 30);   /* (|tail| under 2^30) */
            if (coef > 966367641) coef = 966367641;                            /* (0.9) */
        }
        vac = (int32_t)(vac + (SM(level_q - vac, coef) >> 30));
        if (vac < 0) vac = 0;
        g1 = (int32_t)(((uint64_t)(uint32_t)vac << 15) / (uint32_t)peak);
        if (g1 > 32767) g1 = 32767;
        {   /* the filter: f = 0.003 + 0.3 s^4 + 0.04 hf of Plaits' 48 kHz (x 48 / 44.1 here: the same in Hz); tan (FREQUENCY_DIRTY)
             * f (pi + 11.58 f^2); q 0.4 */
            const int32_t sv2 = (int32_t)(SM(vac, vac) >> 30), sv4 = (int32_t)(SM(sv2, sv2) >> 30);
            const int32_t hf = colr * 258;                                          /* Q15 */
            const int32_t fq = 3506096 + (int32_t)(SM(sv4, 350609575) >> 30) + (int32_t)(SM(hf, 46747943) >> 15);   /* Q30, under 0.4 */
            const int32_t f2 = (int32_t)(SM(fq, fq) >> 30);
            const uint32_t k = 1686629713u + (uint32_t)(UM(f2, 3109541550u) >> 29);  /* pi + 11.58 f^2, Q29 (3109541550: 11.58 / 4, Q30) */
            const int32_t gq = (int32_t)(UM(fq, k) >> 45), rg = 40960 + gq;      /* tan, Q14: r = 2.5 */
            const int32_t tailx = ONE30 - vac, t2 = (int32_t)(SM(tailx, tailx) >> 30);
            gq14 = (int32_t)gq; rgq14 = (int32_t)rg;
            hq14 = (int32_t)((1u << 28) / (uint32_t)(16384 + ((rg * gq) >> 14)));
            bleed = (((t2 + (int32_t)(SM(ONE30 - t2, hf) >> 15)) >> 15) * ((hf * hf) >> 15)) >> 15;   /* Q15 */
        }
        if (!(f & F_RAMP) && vac < (ONE30 >> 13) && g0 < 8) {   /* the gate has closed (-78 dB): nothing to play */
            v->s[3] = (int32_t)f;
            v->s[4] = (int32_t)(((uint32_t)denv << 16) | (uint32_t)g1);
            v->s[5] = vac; v->s[6] = 0; v->s[7] = 0;
            return;
        }
    }
    v->s[3] = (int32_t)f;
    mc_model(model, v, buf, n, &c, m);
    f = (uint32_t)v->s[3];
    for (i = 0; i < n; i++) {
        int32_t x = mul15(buf[i], level);
        if (!open) {
            const int32_t g = g0 + (((g1 - g0) * (int32_t)i) >> 5);
            int32_t hp, bp, lp, gh, gb;
            x = (x * g) >> 9;                     /* (x 64 inside the filter: its states keep the low cutoffs' small steps) */
            hp = mulsh_32(x - mulsh_32(s1, (uint32_t)rgq14, 14) - s2, (uint32_t)hq14, 14);
            gh = mulsh_32(hp, (uint32_t)gq14, 14);
            bp = gh + s1; s1 = gh + bp;
            gb = mulsh_32(bp, (uint32_t)gq14, 14);
            lp = gb + s2; s2 = gb + lp;
            x = (lp + mul15(x - lp, bleed)) >> 6;
        }
        out[i] += voice_amp(x * 2, m, i);   /* (x 2: Plaits' models peak near half scale; the SDK's level is about +-16000) */
    }
    v->s[3] = (int32_t)f;
    v->s[4] = (int32_t)(((uint32_t)denv << 16) | (uint32_t)(open ? 32767 : g1));
    v->s[5] = vac; v->s[6] = s1; v->s[7] = s2;
}

/* --------------------------------------------------------- the descriptor --- */
static const char *const MODELS[] = {"Pair", "Fold", "FM", "Grain", "Harm", "Wave", "Chord", "Noise", 0};

/* the factory presets: a name and its settings over the defaults, in the host's factory words (presets_factory.c); the host
 * takes them through KPluginExt.presets (interface 1.5), macro_ui.c */
static const KPreset MC_PRESETS[] = {
    {"Soft Saw", "MODL=Pair HARM=20 TIMB=40 MORF=50 DCAY=75 COLR=60 A=0 D=60 S=100 R=30"},
    {"Fold Bass", "MODL=Fold HARM=70 TIMB=75 MORF=35 DCAY=55 COLR=25 TENV=24 A=0 D=50 S=100 R=10"},
    {"Bell Keys", "MODL=FM HARM=88 TIMB=62 MORF=70 DCAY=85 COLR=95 TENV=-20 A=0 D=80 S=100 R=50"},
    {"Glass Pad", "MODL=Wave HARM=30 TIMB=50 MORF=80 DCAY=127 COLR=64 A=45 D=60 S=90 R=60 L.RATE=20 L.SHP=12"},
    {"Organ Chord", "MODL=Chord HARM=64 TIMB=30 MORF=100 DCAY=127 LVL=90 A=3 D=60 S=100 R=35"},
    {"Hat Noise", "MODL=Noise HARM=105 TIMB=127 MORF=60 DCAY=28 COLR=100 A=0 D=30 S=100 R=15"},
};

static const engine_t MC_DSP = {
    .name = "Macro",
    .page_title = {"MODEL", "GATE"},
    .knob = {P_E0 + E_HARM, P_E0 + E_TIMB, P_E0 + E_MORF, P_E0 + E_DCAY},
    .keep = (1u << 0) | (1u << 1) | (1u << 4) | (1u << 5) | (1u << 6) | (1u << 7),
    .note_on = mc_note_on,
    .render = mc_render,
    .block = mc_block,
    .mem = sizeof(MacroPart),
};

static const KEngine MC_DESC = {
    .name = "Macro",
    .uid = K_UID('m', 'a', 'c', 'r'),
    .page_title = {"", "MODEL", "GATE"},
    .edit = {
        {"MODL", KF_ENUM, 0, M_COUNT - 1, 0, MODELS, 0},
        {"HARM", KF_PCT, 0, 127, 64, 0, 0},
        {"TIMB", KF_PCT, 0, 127, 64, 0, 0},
        {"MORF", KF_PCT, 0, 127, 64, 0, 0},
        {"DCAY", KF_PCT, 0, 127, 70, 0, 0},
        {"COLR", KF_PCT, 0, 127, 64, 0, 0},
        {"TENV", KF_BIPCT, -64, 63, 0, 0, 0},
        {"LVL", KF_PCT, 0, 127, 100, 0, 0},
    },
    .macro = {E_HARM, E_TIMB, E_MORF, E_DCAY},
    .nparams = 8,
    .page2 = 4,
    .gate = 1,
};

#include "macro_ui.c"

static const KPlugin MACRO = {sizeof(KPlugin), K_PLUGIN_ABI, 1, K_PLUG_SYNTH, &MC_DESC, &MC_DSP,
                              FS, 0, 20, 0, 0, &MC_EXT};   /* (its state: the voices' and the part's memory) */

KREISEL_PLUGIN_EXPORT const KPlugin *kreisel_plugin(void) { return &MACRO; }
