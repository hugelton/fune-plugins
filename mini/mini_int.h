/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Part of MINI (plugins/mini), based on CA-72 by Idle Foundry (copyright (C) 2026 Idle Foundry Ltd., GPL-3.0-or-later). */
/* MINI's integer path (the FM-1: no FPU), included by mini.c: CA-72's light voice (ca72/src/light.rs) ported to integers, its
 * structure kept and its laws read from tables CA-72's own functions made (mini_int_tables.h, light/src/bin/mini_gen.rs):
 *   - the keys: last-note priority, single triggering, the trigger following the keys' gate late (on 7.8 ms, off 14.1 ms) and
 *     re-armed 11.5 ms after the last key; GLIDE a constant-rate slide (falling at 0.535 of rising);
 *   - three oscillators (band-limited: the SDK's polyBLEP, BLAMP on the triangle) at the circuit's levels and duty cycles, each
 *     through its mixer channel (the loaded VOLUME pot against its source), oscillator 3 free of the keys (OSC. 3 CONTROL off);
 *     white noise through its channel;
 *   - the ladder: the input pair (a tanh off its centre by the bias, its limit raised as it rings) on the input less the loop's
 *     feedback, solved without delay (straight, then the pair's tanh), four trapezoidal one-poles; its cutoff through the
 *     control node's law (CUTOFF's current, the contour's, the keyboard's share), EMPHASIS's loop gain by the cutoff, the corner,
 *     the passband; the output's coupling; at the voice's rate (CA-72 runs it at eight times);
 *   - the contours: RC segments a block with the circuit's time laws, sustain and release levels, the DECAY switch; the VCA's law;
 *   - the modulation: oscillator 3 (its swing, centre and the line's offset by its waveform) and the noise, MODULATION MIX, the
 *     wheel's depth, to the oscillators and the cutoff.
 * Left out of the port: the contours' absorbed charge, the waveforms' droop, the noise's bands, the VCA's thump, FILTER MODE and
 * POLES. Divisions: one a sub-block (the loop's reciprocal), none in the sample loop. */
#ifndef MINI_INT_H
#define MINI_INT_H
#include "mini_int_tables.h"

#define MINI_COST 7   /* (us a voice: one sounds, ~10.8-12.7 k instructions a block on fm1-emu, spread over the room's six) */

/* (x g) >> 16 for g in 0 .. 65536, x signed, in 32-bit pieces (no 64-bit product: the FM-1's erratum) */
static inline int32_t m16(int32_t x, uint32_t g)
{
    return (x >> 16) * (int32_t)g + (int32_t)(((uint32_t)x & 0xFFFFu) * g >> 16);
}

typedef struct {
    /* the panel, worked out when it changes */
    int32_t offset[3];             /* each oscillator's pitch from its key (semitones x 4096) */
    int32_t gain[3], ngain;        /* each channel into the bus, Q15 (its waveform's level in) */
    uint8_t wave[3], follows3, osc_mod, filter_mod, kind3;
    int32_t kc, cc;                /* CUTOFF's current, the contour's a volt (nA x 16) */
    int32_t track;                 /* KEYBOARD CONTROL, Q12 */
    const uint16_t *ek;            /* EMPHASIS's loop gain on the grid */
    int32_t corner;                /* Q15 */
    uint8_t fk[3], lk[3], decay;
    int32_t glide;                 /* semitones x 4096 a block */
    uint32_t beyond_k;             /* Q16 */
    int32_t depth, offd, mix;      /* Q15 */
    int32_t swbias, mswing, mcentre;
    /* the keys */
    uint8_t held[8], nheld, gate, trigger, attack;
    int32_t pending, open;         /* samples */
    int32_t pitch, beyond;         /* the keyboard's pitch and what lies beyond its keys, semitones x 4096 */
    /* the oscillators, the noise, the contours, the ladder */
    uint32_t ph[3];
    int32_t o3, nst, pk;
    int32_t cv[2];                 /* volts Q16 */
    uint8_t attacking[2], fresh;
    int32_t vca;                   /* the last block's VCA gain, Q15 */
    int32_t s[4], y, cx, cy;
    int32_t off16, offoct;         /* the host's: pitch (1/16 semitone), cutoff (log2 x 3072) */
} MiniInt;

static void mi_rest(MiniInt *m)
{
    uint8_t *b = (uint8_t *)m;
    uint32_t i;
    for (i = 0; i < sizeof *m; i++) b[i] = 0;
    m->pitch = MI_PITCH_REST;
    m->fresh = 1;
    m->nst = 0x2545F491;
    m->open = 1 << 20;
}

static void mi_panel(MiniInt *m, const MiniKnobs *k)
{
    static const uint8_t SRC[7] = {0, 3, 1, 1, 2, 2, 2};   /* each waveform's source (triangle, sawtooth, rectangle, shark tooth) */
    int o;
    for (o = 0; o < 3; o++) {
        const int w = k->wave[o] > 6 ? 3 : k->wave[o];
        int32_t g = k->on[o] ? (int32_t)MI_CH[SRC[w] * 128 + (k->volume[o] & 127)] : 0;
        if (o == 2) g = (g * MI_OSC3_LEVEL) >> 15;
        m->gain[o] = (((g * MI_WLEVEL[w]) >> 15) * MI_DRIVE) >> 15;   /* (the pair's DRIVE in: the bus as the ladder takes it) */
        m->wave[o] = (uint8_t)w;
        m->offset[o] = MI_RANGE[k->range[o] > 5 ? 3 : k->range[o]];
    }
    m->offset[1] += MI_FRQ2[k->freq[0] + 64];
    m->follows3 = k->osc3_control;
    m->offset[2] += k->osc3_control ? MI_FRQ3[k->freq[1] + 64] : MI_FRQ3FREE[k->freq[1] + 64];
    m->ngain = k->noise_on ? (MI_NOISE[k->noise_volume & 127] * MI_DRIVE) >> 15 : 0;
    m->kc = MI_KC[k->cutoff & 127];
    m->cc = MI_CC[k->amount & 127];
    m->track = k->kbd1 && k->kbd2 ? MI_TRACK[2] : k->kbd2 ? MI_TRACK[1] : k->kbd1 ? MI_TRACK[0] : 0;
    m->ek = &MI_EK[(k->emphasis & 127) * MI_GRIDN];
    m->corner = MI_ECORNER[k->emphasis & 127];
    for (o = 0; o < 3; o++) { m->fk[o] = k->fadsr[o] & 127; m->lk[o] = k->ladsr[o] & 127; }
    m->decay = k->decay;
    m->glide = MI_GLIDE[k->glide & 127];
    m->beyond_k = MI_BEYOND[k->glide & 127];
    m->depth = MI_DEPTH[k->wheel & 127];
    m->offd = MI_OFFD[k->wheel & 127];
    m->mix = k->mod_mix >= 127 ? 32767 : k->mod_mix * 258;
    m->osc_mod = k->osc_mod;
    m->filter_mod = k->filter_mod;
    m->kind3 = MI_MKIND[m->wave[2]];
    m->swbias = MI_SWBIAS[m->wave[2]];
    m->mswing = MI_MSWING[m->kind3];
    m->mcentre = MI_MCENTRE[m->kind3];
}

static void mi_note(MiniInt *m, int note, int on)
{
    int i, f = -1;
    for (i = 0; i < m->nheld; i++) if (m->held[i] == note) f = i;
    if (f >= 0) { for (i = f; i + 1 < m->nheld; i++) m->held[i] = m->held[i + 1]; m->nheld--; }   /* (the order kept) */
    if (on && note >= 0 && note < 128) {
        if (m->nheld >= 8) { for (i = 0; i < 7; i++) m->held[i] = m->held[i + 1]; m->nheld = 7; }
        m->held[m->nheld++] = (uint8_t)note;   /* (the newest last: it sounds) */
    }
}
static void mi_offsets(MiniInt *m, int32_t pitch16, int32_t oct)
{
    m->off16 = pitch16;
    m->offoct = (oct * 16);   /* (1/192 octave to log2 x 3072) */
}
static int mi_sounding(MiniInt *m) { return m->nheld || m->cv[1] > -26214 || m->vca > 8; }   /* (keys, or the loudness above -0.4 V) */

/* a grid table (1/3 octave from log2 4) at log2 x 3072 */
static int32_t mi_grid(const int32_t *t, int32_t l)
{
    int32_t i, f;
    l -= MI_GRID0;
    if (l <= 0) return t[0];
    i = l >> 10; f = l & 1023;
    if (i >= MI_GRIDN - 1) return t[MI_GRIDN - 1];
    return t[i] + (((t[i + 1] - t[i]) * f) >> 10);
}
static int32_t mi_grid16(const uint16_t *t, int32_t l)
{
    int32_t i, f;
    l -= MI_GRID0;
    if (l <= 0) return t[0];
    i = l >> 10; f = l & 1023;
    if (i >= MI_GRIDN - 1) return t[MI_GRIDN - 1];
    return t[i] + ((((int32_t)t[i + 1] - t[i]) * f) >> 10);
}
/* the control node's law: log2 x 3072 of the ladder's ring for a current (nA x 16) */
static int32_t mi_node(int32_t c)
{
    int k = 0;
    while (k < 19 && c >= MI_PTS[k + 1]) k++;
    return MI_PLOG2[k] + ((((c - MI_PTS[k]) >> 8) * (MI_PSLOPE[k] >> 4)) >> 12);   /* (32-bit: the FM-1) */
}
static uint32_t mi_inc(int32_t st)   /* semitones x 4096 from MIDI 0 to a phase increment; under 0 in octaves down */
{
    int32_t p16 = st >> 8;
    const int32_t fine = ((st & 255) * 15) >> 8;
    uint32_t sh = 0, inc;
    while (p16 < 0 && sh < 24) { p16 += 192; sh++; }
    if (p16 > 2047) p16 = 2047;
    inc = pitch_inc((uint32_t)p16) >> sh;
    if (fine) inc += (inc >> 12) * (uint32_t)fine;
    return inc;
}
static inline int32_t mi_wave(int w, uint32_t ph, uint32_t inc)
{
    switch (w) {
    case 0: return osc_tri_bl(ph, inc);
    case 1: return (osc_tri_bl(ph, inc) * MI_SHARK_TRI + osc_saw(ph, inc) * MI_SHARK_SAW) >> 15;   /* (|x| < 2^15: 32 bits) */
    case 2: return -osc_saw(ph, inc);
    case 3: return osc_saw(ph, inc);
    default: return osc_pulse(ph, inc, MI_DUTY[w - 4]) * 2;   /* (the SDK's pulse swings +-0.5: the rectangle at the sawtooth's +-1) */
    }
}
/* a contour, a block on (which 0 the filter's, 1 the loudness's) */
static void mi_contour(MiniInt *m, int w)
{
    const uint8_t *k = w ? m->lk : m->fk;
    int32_t v = m->cv[w];
    if (m->trigger) {
        if (m->attacking[w]) {
            const int32_t aim = w ? MI_LAIM : MI_FAIM, stop = w ? MI_LSTOP[k[0]] : MI_FSTOP[k[0]];
            const int32_t next = v + mulq16(aim - v, w ? MI_LATK[k[0]] : MI_FATK[k[0]]);
            if (next >= stop) { v = stop > v ? stop : v; m->attacking[w] = 0; }
            else v = next;
        } else {
            v -= mulq16(v - (w ? MI_LSUS[k[2]] : MI_FSUS[k[2]]), w ? MI_LDEC[k[1]] : MI_FDEC[k[1]]);
        }
    } else {
        const int32_t rel = m->decay ? (w ? MI_LRELEASED : MI_FRELEASED) : (w ? MI_LRELDOFF[k[1]] : MI_FRELDOFF[k[1]]);
        if (m->fresh) v = rel;
        else {
            m->attacking[w] = 0;
            v += mulq16(rel - v, m->decay ? (w ? MI_LRELON[k[1]] : MI_FRELON[k[1]]) : (w ? MI_LRELOFF[k[1]] : MI_FRELOFF[k[1]]));
        }
    }
    m->cv[w] = v;
}
static int32_t mi_vca(int32_t v)   /* the VCA's gain on the loudness contour's volts, Q15 */
{
    int32_t i, f;
    if (v <= 0) return 0;
    v *= 10;                       /* (a step a tenth of a volt) */
    i = v >> 16; f = (v >> 4) & 4095;
    if (i >= 53) return MI_VCA[53];
    return MI_VCA[i] + (((MI_VCA[i + 1] - MI_VCA[i]) * f) >> 12);
}

#define MI_SUB 8
#define MI_SSH 8   /* the ladder's states over the pair's Q15: x 256 (|y| < 2^17: the states under 2^25, every m16 in 32 bits) */
static void mi_render(MiniInt *m, int32_t *dst, uint32_t n)
{
    int32_t vca0, vca1, vo0, vo1, base[3], node, i, j;
    uint32_t inc[3];
    /* the keys: the gate, the trigger late; the keyboard's pitch through GLIDE */
    {
        const int gate = m->nheld > 0;
        if (gate != m->gate) {
            m->gate = (uint8_t)gate;
            if (gate) { m->attack = m->open >= MI_REARM; m->pending = m->attack || !m->trigger ? MI_TRIG_ON : 0; }
            else m->pending = MI_TRIG_OFF;
            if (!m->pending) m->trigger = (uint8_t)gate;
        }
        if (!gate) m->open += (int32_t)n; else m->open = 0;
        if (m->pending > 0 && (m->pending -= (int32_t)n) <= 0) {
            m->pending = 0;
            m->trigger = m->gate;
            if (m->gate) { m->attacking[0] = m->attacking[1] = m->attack; m->fresh = 0; }
        }
        if (gate) {   /* the keyboard's keys slide (GLIDE); beyond them the rest moves as an RC through GLIDE's resistance */
            const int32_t key = (int32_t)m->held[m->nheld - 1] << 12;
            const int32_t target = clamp(key, MI_KEYS_LO, MI_KEYS_HI), d = target - m->pitch, foot = key - target;
            const int32_t up = m->glide, down = mulsh_32(m->glide, MI_GLIDE_FALL, 15);
            m->pitch += d > up ? up : d < -down ? -down : d;
            m->beyond = foot + mulq16(m->beyond - foot, m->beyond_k);
        }
    }
    mi_contour(m, 0);
    mi_contour(m, 1);
    vca0 = m->vca;
    vca1 = mi_vca(m->cv[1]);
    m->vca = vca1;
    vo0 = (vca0 * MI_OUT) >> 15;
    vo1 = (vca1 * MI_OUT) >> 15;
    for (j = 0; j < 3; j++)   /* each oscillator's pitch (semitones x 4096 from MIDI 0): the keys, its offset, the host's */
        base[j] = (j == 2 && !m->follows3 ? MI_OSC3_FREE_KEY : m->pitch + m->beyond + (m->off16 << 8)) + m->offset[j];
    {   /* the cutoff's node: CUTOFF and the contour's currents */
        const int32_t cur = m->kc + (((m->cv[0] >> 6) * (m->cc >> 4)) >> 6);
        node = mi_node(cur) + ((((m->pitch + m->beyond - MI_TRACK_FROM) >> 4) * m->track) >> 12) + m->offoct;
    }
    for (j = 0; j < (int32_t)n; j += MI_SUB) {
        const int32_t e = j + MI_SUB < (int32_t)n ? j + MI_SUB : (int32_t)n;
        int32_t l = node, pm = 0, g, h, k, top, inv, rb, pass, corner, lg;
        uint32_t gi;
        if (m->depth && (m->osc_mod || m->filter_mod)) {   /* the modulation: oscillator 3 and the noise through the wheel */
            const int32_t o = (((m->o3 + m->swbias * 8) * m->mswing) >> 15) + m->mcentre;   /* oscillator 3's semitones (x 4096) */
            const int32_t nz = clamp(m->pk * 10, -98304, 98304), mx = m->mix, ox = 32767 - mx;
            if (m->osc_mod)
                pm = mulsh_32(((ox * (o >> 1)) >> 14) + (((nz * MI_NOISE_ST_RMS) >> 15) * (mx >> 3) >> 12), (uint32_t)m->depth, 15)
                     + ((((ox * MI_MOD_OFFSET0 + mx * MI_NOISE_OFFSET0) >> 15) * m->offd) >> 15);
            if (m->filter_mod)
                l += mulsh_32((((ox >> 3) * ((((o * MI_FILTER_PER_ST) >> 12) * 3) >> 2)) >> 12) + (((nz * MI_NOISE_OCT_RMS) >> 15) * (mx >> 3) >> 12),
                              (uint32_t)m->depth, 15)
                     + ((((ox * MI_MOD_OFFSET1 + mx * MI_NOISE_OFFSET1) >> 15) * m->offd) >> 15);
        }
        if (j == 0 || pm) {
            inc[0] = mi_inc(base[0] + pm); inc[1] = mi_inc(base[1] + pm);
            inc[2] = mi_inc(base[2] + (m->follows3 ? pm : 0));
        }
        /* the ladder's coefficients: the corner above the ring (or the ring's raise), saturation, g, k, the pair's limit, the passband */
        corner = m->corner;
        lg = l + ((mi_grid(MI_CORNER, l) * corner + mi_grid(MI_RAISE, l) * (32767 - corner)) >> 15);   /* (|corner| < 2^9 x 2^15) */
        if (lg > MI_SAT0) {
            const int32_t d = lg - MI_SAT0, q = d >> 10;
            lg = q >= 30 ? MI_SAT[30] : MI_SAT[q] + (((MI_SAT[q + 1] - MI_SAT[q]) * (d & 1023)) >> 10);
        }
        gi = (uint32_t)(lg < MI_G_LOG2_0 ? 0 : lg - MI_G_LOG2_0);
        g = (gi >> 8) >= 131 ? MI_G[131] : MI_G[gi >> 8] + ((((int32_t)MI_G[(gi >> 8) + 1] - MI_G[gi >> 8]) * (int32_t)(gi & 255)) >> 8);
        if (g > 65470) g = 65470;
        h = 65536 - g;
        k = mi_grid16(m->ek, l);                            /* Q8 */
        {   /* the pair's limit: raised as it rings (1 - corner of the way to the ring's) */
            const int32_t ring = 32767 - corner;
            top = 32768 + (((mi_grid(MI_TOP, l) - 32768) * ring) >> 15);   /* (differences under 2^15) */
            inv = 32768 + (((mi_grid(MI_TOPINV, l) - 32768) * ring) >> 15);
            rb = MI_RB1 + (((mi_grid(MI_TOPRB, l) - MI_RB1) * ring) >> 15);
        }
        pass = mi_grid(MI_PASS, l);
        {
            const int32_t g2 = (int32_t)(((uint32_t)g * (uint32_t)g) >> 16), g4 = (int32_t)(((uint32_t)g2 * (uint32_t)g2) >> 16);
            const int32_t q = (k * (g4 >> 4)) >> 8;         /* k g^4, Q12 */
            const int32_t inv1q = (int32_t)((32768u << 12) / (uint32_t)(4096 + q));   /* 1 / (1 + q), Q15 */
            int32_t s0 = m->s[0], s1 = m->s[1], s2 = m->s[2], s3 = m->s[3], y = m->y, o3 = m->o3, pk = m->pk, nst = m->nst;
            int32_t cx = m->cx, cy = m->cy;
            const int need3 = m->gain[2] || (m->depth && (m->osc_mod || m->filter_mod) && m->mix < 32767);
            const int needn = m->ngain || (m->depth && (m->osc_mod || m->filter_mod) && m->mix > 0);
            uint32_t p0 = m->ph[0], p1 = m->ph[1], p2 = m->ph[2];
            for (i = j; i < e; i++) {
                int32_t bus = 0, x, v, rest, a, w3;
                const int32_t vca = vo0 + (((vo1 - vo0) * i) >> 5);   /* (the VCA's gain times OUT, along the block) */
                if (m->gain[0]) bus += (mi_wave(m->wave[0], p0, inc[0]) * m->gain[0]) >> 15;
                if (m->gain[1]) bus += (mi_wave(m->wave[1], p1, inc[1]) * m->gain[1]) >> 15;
                if (need3) {   /* (oscillator 3: heard, or the modulation's) */
                    w3 = mi_wave(m->wave[2], p2, inc[2]);
                    o3 = w3;
                    if (m->gain[2]) bus += (w3 * m->gain[2]) >> 15;
                }
                if (needn) {
                    const int32_t wn = (int32_t)(noise32(&nst) >> 16) - 32768;
                    pk += (wn - pk) >> 4;                   /* (the darker noise: the modulation's) */
                    if (m->ngain) bus += (wn * m->ngain) >> 15;
                    bus += wn >> 13;                        /* (the mixer's own hiss, which the ladder sings from) */
                } else if (i == j) bus += ((int32_t)(noise32(&nst) >> 16) - 32768) >> 11;   /* (no noise in use: the hiss a kick a sub-block) */
                p0 += inc[0]; p1 += inc[1]; p2 += inc[2];
                /* the input pair and the loop, solved: y = top tanh((v - q y) / top) - rb, v the input less k times the stages' rest */
                x = bus;
                rest = m16(m16(m16(m16(s0, (uint32_t)g) + s1, (uint32_t)g) + s2, (uint32_t)g) + s3, (uint32_t)h) >> MI_SSH;   /* (the states x 256: Q23) */
                v = x - mulsh_32(rest, (uint32_t)k, 8) + MI_BIAS;
                /* the loop solved as if the pair were straight (one reciprocal a sub-block), then the pair's tanh once on the input
                 * less the feedback (CA-72 solves the pair exactly, by Newton's steps: as close here on every preset, and cheaper) */
                y = mulsh_32(v, (uint32_t)inv1q, 15);
                a = mulsh_32(v - mulsh_32(y, (uint32_t)q, 12), (uint32_t)inv, 15);
                y = ((softclip(a) * top) >> 15) - rb;
                {   /* the four stages, at 256 times the pair's resolution: low cutoffs move their states by less than a Q15 step.
                     * Version 2 kept them at 8 times: under ~-50 dB at the lowest cutoffs a stage's step truncated to nothing, so
                     * it froze (a quiet tail stuck, the ladder unable to start ringing from the hiss: the Whistle preset silent) */
                    const int32_t y8 = y * (1 << MI_SSH);
                    int32_t u = m16(y8 - s0, (uint32_t)g), o = u + s0; s0 = o + u;
                    u = m16(o - s1, (uint32_t)g); o = u + s1; s1 = o + u;
                    u = m16(o - s2, (uint32_t)g); o = u + s2; s2 = o + u;
                    u = m16(o - s3, (uint32_t)g); o = u + s3; s3 = o + u;
                    o = mulsh_32(o >> MI_SSH, (uint32_t)pass, 15);   /* (back to Q15) */
                    cy = cy - m16(cy, MI_COUPLING) + (o - cx);  /* (the output's coupling, 13 Hz) */
                    cx = o;
                }
                /* (the stages need no clamp: the pair's tanh bounds what enters them) */
                dst[i] = mulsh_32(cy, (uint32_t)vca, 17);   /* (Q15 to MINI_ONE, Q13) */
            }
            m->s[0] = s0; m->s[1] = s1; m->s[2] = s2; m->s[3] = s3; m->y = y; m->o3 = o3; m->pk = pk; m->nst = nst;
            m->cx = cx; m->cy = cy;
            m->ph[0] = p0; m->ph[1] = p1; m->ph[2] = p2;
        }
    }
}
#endif
