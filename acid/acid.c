/* SPDX-License-Identifier: MIT
 * Copyright (c) 2009 Robin Schmidt (www.rs-met.com): Open303, the structure and the formulas this follows
 * Copyright (c) 2026 Leo Kuroshita (@kurogedelic), Hugelton Instruments: this integer port, the plugin */
/* ACID: a bass line synth for Fune, an outside engine plugin (docs/PLUGIN_SDK.md). Based on Open303 by Robin Schmidt
 * (https://github.com/RobinSchmidt/Open303, MIT: plugins/acid/LICENSE), ported to integers so it runs on every device, the FM-1
 * (no FPU) among them. The float original is not here: plugins/acid/dev/ compares the two (plugins/acid/README.md).
 *
 * One voice (poly 1). Open303's voice, sample by sample:
 *   a saw or a square (Open303's square is a tanh-shaped saw: a pulse of 53 %, at half the saw's level), negated
 *   -> a one-pole high-pass, 44.5 Hz -> the diode-ladder style filter (Open303's TB_303 mode, after mystran and kunn: four
 *   one-pole stages coupled to their neighbours, the last fed back through a 150 Hz high-pass; its coefficients a rational fit
 *   of the cutoff) -> a 24.2 Hz high-pass -> the amplitude.
 * The oscillator and the filter run at twice the host's rate (Open303: four times, and an elliptic anti-alias filter; here the two
 * samples are averaged). Open303's 14 Hz all-pass and its 7.5 Hz notch are left out (below hearing).
 *   The filter's envelope (the main one) jumps to 1 at a note and decays (DCY; 200 ms on an accented note); the cutoff is
 *   CUT x 2^(scaler (env - offset)), the scaler and offset from ENV and CUT by Open303's measured mapping, plus on an accented
 *   note ACC x the envelope through a 15 ms leaky integrator (the accent's sweep, its "wow"). The amplitude: 1 at a note, decaying
 *   over 1230 ms while held, released in 1 ms (50 ms accented), plus while held 0.45 x the main envelope, and ACC x 4 x it on an
 *   accent.
 *   Accent: a note's velocity above 110 (a Fune step's strength Hard, or a hard key). Slide: a note that comes while the last is
 *   still held (in the sequencer a note longer than its step overlaps the next: voice_t.legato): the pitch glides to it (a leaky
 *   integrator on the frequency, 0.2 x SLID; Open303's 60 ms by default), no envelope restarts, the accent is the new note's.
 *   Velocity sets nothing else: the level is the same at every velocity but the accent (the amp hook undoes the host's).
 * Additions of Fune's (none in Open303): TUNE in cents, DRV (a soft clip after the filter, off at 0).
 *
 * Fixed point. Signals Q20 (1.0 = 2^20) in the filter, so the stages' small steps at a low cutoff are not lost; every product is
 * a pair of 32-bit ones (mulsh_32: no 64-bit multiply, which the FM-1's CPU takes out of line). The filter's coefficients come
 * from tables made by tools/gen_acid_tables.py (acid_tables.h), looked up twice a block and ramped across it sample by sample.
 *
 * State. ph[0] the oscillator's phase, ph[1] the slid frequency (a phase increment at the host's rate), ph[2] the last high-pass;
 * s[0] s[1] s[4] s[5] the filter's four stages, s[6] the feedback high-pass, s[7] the first high-pass (kept over a retrigger:
 * keep); s[2] the main envelope (Q24), s[3] the amplitude (Q24) (note_on's: kept on a slide, set on a new note). A little more
 * for each voice in a table of its own (the accent's sweep, flags), found by the voice's address. */
#include "kreisel_sdk.h"
#include "acid_tables.h"

enum { A_WAVE, A_CUT, A_RES, A_ENV, A_DCY, A_ACC, A_TUNE, A_SLID, A_DRV, A_N };
#define ACID_ACCENT_VEL 110   /* a velocity above it is an accent */
#define U127_Q12(v) (((v) * 33026) >> 10)   /* 0 .. 127 -> 0 .. 4096 (x 4096 / 127, no divide) */

/* a voice's state beyond voice_t, found by its address (8: one a track with the host's mono cap; an older host's voices past
 * that share the least recently used) */
enum { F_ACC = 1, F_SNAP = 2, F_GATE = 4, F_JOIN = 8 };
/* alast, llast: the amplitude (Q13) and the cutoff (log2 Hz Q16) at the end of the last block; a new note struck while the voice
 * still sounds (F_JOIN) starts its first block from them, ramping to its own across the block (0.7 ms), not in a step: its
 * envelopes jump to the top at once, which clicked (+31 dB over 6 kHz at a retrigger, version 1) */
typedef struct { const voice_t *v; int32_t rc2; uint32_t flags, used; int32_t alast, llast; } AcidX;
#define ACID_NX 8
static AcidX acid_x[ACID_NX];
static uint32_t acid_clock;
/* what the picture shows live (the last voice rendered): its cutoff (log2 Hz, Q16), blocks since its note, 0 none */
static volatile int32_t acid_live_l, acid_live_age;
static volatile uint32_t acid_live_tick;   /* counts the blocks rendered: the picture shows the dot only while it moves */

static AcidX *acid_slot(const voice_t *v)
{
    AcidX *old = &acid_x[0];
    int i;
    for (i = 0; i < ACID_NX; i++) {
        if (acid_x[i].v == v) { acid_x[i].used = ++acid_clock; return &acid_x[i]; }
        if (acid_x[i].used < old->used) old = &acid_x[i];
    }
    old->v = v; old->rc2 = 0; old->flags = F_SNAP; old->used = ++acid_clock; old->alast = 0; old->llast = ACID_L_MIN;
    return old;
}

static void acid_note_on(track_t *t, voice_t *v)
{
    AcidX *x = acid_slot(v);
    const int acc = v->vel > ACID_ACCENT_VEL;
    (void)t;
    /* the filter's states and the phases from rest; the host puts back the kept ones (keep, ph[]) when the voice still sounds */
    v->ph[0] = 0; v->ph[2] = 0;
    v->s[0] = v->s[1] = v->s[4] = v->s[5] = v->s[6] = v->s[7] = 0;
    x->flags = (x->flags & F_GATE) | (acc ? F_ACC : 0);
    if (!v->legato) {   /* a new note: the pitch at once, both envelopes from the top (Open303's triggerNote) */
        x->flags |= F_SNAP | (v->env_out ? F_JOIN : 0);   /* (env_out: the host's level, 0 for a voice that was silent) */
        v->s[2] = 1 << 24;
        v->s[3] = 1 << 24;
    }   /* a slide (slideToNote): the envelopes run on; the accent, the decay and the release are the new note's */
}

/* the cutoff as log2(Hz) Q16 from the main envelope and the accent's sweep (Q24), the block's constants */
typedef struct { int32_t base, scale, off, accg; } CutK;
static int32_t acid_cut(const CutK *c, int32_t menv, int32_t rc2)
{
    int32_t l = c->base + mulsh_32((menv - c->off) >> 8, (uint32_t)c->scale, 12)   /* (Q16 x Q12) >> 12 */
                + mulsh_32(rc2 >> 8, (uint32_t)c->accg, 12);
    return clamp(l, ACID_L_MIN, ACID_L_MAX);
}
/* the filter's coefficients at a cutoff: b0 Q24, k Q12 (with the feedback high-pass's gain), 2g Q12 */
typedef struct { int32_t b0, k, g2; } Coef;
static void acid_coef(Coef *c, int32_t l, int32_t r)   /* r: the skewed resonance, Q12 */
{
    const int32_t d = l - ACID_L_MIN, i = d >> ACID_L_SHIFT, f = d & ((1 << ACID_L_SHIFT) - 1);
    const int32_t j = i + 1 < (int32_t)(sizeof ACID_B0 / sizeof ACID_B0[0]) ? i + 1 : i;
    const int32_t b0 = ACID_B0[i] + mulsh_32(ACID_B0[j] - ACID_B0[i], (uint32_t)f, ACID_L_SHIFT);
    const int32_t kb = ACID_K[i] + mulsh_32(ACID_K[j] - ACID_K[i], (uint32_t)f, ACID_L_SHIFT);
    /* Open303: g = ((k / 17 - 1) r + 1)(1 + r), k = k r */
    const int32_t g = ((mulsh_32(((kb * 241) >> 12) - 4096, (uint32_t)r, 12) + 4096) * (4096 + r)) >> 12;
    c->b0 = b0;
    c->k = mulsh_32(mulsh_32(kb, (uint32_t)r, 12), ACID_FB_G, 12);
    c->g2 = g << 1;
}

static void acid_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    AcidX *x = acid_slot(v);
    const int sqr = p[P_E0 + A_WAVE] == 1, acc = (x->flags & F_ACC) != 0, gate = v->gate != 0, was = (x->flags & F_GATE) != 0;
    const int32_t cut = clamp(p[P_E0 + A_CUT], 0, 127), envm = clamp(p[P_E0 + A_ENV], 0, 127);
    const int32_t accq = U127_Q12(clamp(p[P_E0 + A_ACC], 0, 127));   /* Q12 */
    const int32_t r = ACID_RSKEW[clamp(p[P_E0 + A_RES], 0, 127)];
    const int32_t drv = clamp(p[P_E0 + A_DRV], 0, 127), g4 = 4 + drv / 4;   /* DRV's gain x 4 */
    const uint32_t dcomp = ACID_DRV_COMP[drv];                               /* its make-up, 2 / sqrt(g4), Q12 */
    const uint32_t sk = ACID_SLD[clamp(p[P_E0 + A_SLID], ACID_SLD_MIN, ACID_SLD_MAX) - ACID_SLD_MIN];
    const uint32_t dc = acc ? ACID_DCY_ACCENT : ACID_DCY[clamp(p[P_E0 + A_DCY], ACID_DCY_MIN, ACID_DCY_MAX) - ACID_DCY_MIN];
    const int32_t tune = p[P_E0 + A_TUNE];
    const uint32_t target = tune ? cents_inc(m->pitch16, tune, m->fine) : m->inc;
    CutK ck;
    Coef c0, c1;
    int32_t menv0 = v->s[2], aenv0 = v->s[3], menv1, aenv1, rc20 = x->rc2, rc21, l0, l1, a0, a1;
    int32_t b0q, kq, gq, db0, dk, dg, da;
    int32_t y1 = v->s[0], y2 = v->s[1], y3 = v->s[4], y4 = v->s[5], fbl = v->s[6], hpl = v->s[7], hp2 = (int32_t)v->ph[2];
    uint32_t ph = v->ph[0], inc = v->ph[1], i;
    {   /* Open303's measured mapping (calculateEnvModScalerAndOffset): e = ENV, c = CUT, both 0 .. 1 */
        const int32_t e = U127_Q12(envm), cc = U127_Q12(cut);   /* Q12 */
        const int32_t slo = ((e * 15458) >> 12) + 3019, shi = ((e * 17181) >> 12) + 3540;   /* 3.774 e + 0.737, 4.195 e + 0.864 */
        ck.scale = ((4096 - cc) * slo + cc * shi) >> 12;                                  /* Q12 */
        ck.off = (int32_t)(((uint32_t)cc * 3165u) >> 4) + 4939038;   /* (0.04829 c + 0.29439) Q24: 3165 = 0.04829 x 2^16 */
        ck.base = ACID_L_314 + mulsh_32(ACID_L_SPAN, (uint32_t)cc, 12);
        ck.accg = accq;
    }
    /* the block's envelopes: their values at its start (0) and end (1) */
    menv1 = mulsh_32(menv0, dc, 16);
    rc21 = (acc ? menv1 : 0) + mulsh_32(rc20 - (acc ? menv1 : 0), ACID_RC2, 16);
    aenv1 = mulsh_32(aenv0, gate ? ACID_AMP_DEC : acc ? ACID_AMP_REL_ACC : ACID_AMP_REL, 16);
    l0 = acid_cut(&ck, menv0, rc20);
    l1 = acid_cut(&ck, menv1, rc21);
    acid_coef(&c0, l0, r);
    acid_coef(&c1, l1, r);
    /* the amplitude (Q24 -> Q13 with the level): 1 + while held 0.45 env + ACC 4 env */
    {
        const int32_t k0 = was ? 1843 + (accq * 4 * acc) : 0, k1 = gate ? 1843 + (accq * 4 * acc) : 0;   /* Q12: 0.45 = 1843 */
        a0 = aenv0 + mulsh_32(menv0, (uint32_t)k0, 12);
        a1 = aenv1 + mulsh_32(menv1, (uint32_t)k1, 12);
        a0 >>= 11; a1 >>= 11;   /* Q13 */
    }
    if (x->flags & F_JOIN) {   /* struck while it sounds: from where the last note was */
        x->flags &= ~(uint32_t)F_JOIN;
        a0 = x->alast;
        acid_coef(&c0, x->llast, r);
    }
    x->alast = a1; x->llast = l1;
    v->s[2] = menv1; v->s[3] = aenv1; x->rc2 = rc21;
    x->flags = (x->flags & ~(uint32_t)F_GATE) | (gate ? F_GATE : 0);
    if (x->flags & F_SNAP) acid_live_age = 0;   /* (a new note: its sweep from the start) */
    acid_live_l = l1; acid_live_age = gate || aenv1 > (1 << 18) ? acid_live_age + 1 : 0;
    acid_live_tick++;
    /* the ramps across the block (n is the host's block, CTL: >> 5) */
    b0q = c0.b0; kq = c0.k << 5; gq = c0.g2 << 5;
    db0 = (c1.b0 - c0.b0) >> 5; dk = c1.k - c0.k; dg = c1.g2 - c0.g2; da = a1 - a0;
    if (x->flags & F_SNAP) { inc = target; x->flags &= ~(uint32_t)F_SNAP; }
    for (i = 0; i < n; i++) {
        int32_t acc2 = 0, s, j;
        const int32_t b = b0q >> 8, b2 = b0q >> 7, k = kq >> 5, g2 = gq >> 5;   /* b0 Q16, 2 b0 Q16, k Q12, 2g Q12 */
        const int32_t a = a0 + ((da * (int32_t)i) >> 5);
        {   /* the slide: a leaky integrator on the frequency, at the host's rate */
            const int32_t d = (int32_t)(target - inc);
            if (d > 2048 || d < -2048) inc += (uint32_t)mulsh_32(d, sk, 18); else inc = target;
        }
        for (j = 0; j < ACID_OS; j++) {
            const uint32_t io = inc >> 1;
            int32_t o, fb;
            o = sqr ? (osc_saw(ph, io) - osc_saw(ph + 2280627630u, io)) >> 1   /* a 53 % pulse (0.531 x 2^32), at half the saw's level */
                    : osc_saw(ph, io);
            ph += io;
            o = -(o << 5);                                   /* Q20, negated as Open303's */
            hpl += mulsh_32(o - hpl, ACID_HP1_K, 16);        /* the pre-filter high-pass: o - its low-pass */
            o -= hpl;
            fb = mulsh_32(y4, (uint32_t)k, 12);              /* the feedback through its high-pass */
            fbl += mulsh_32(fb - fbl, ACID_FB_K, 16);
            o -= fb - fbl;
            y1 += mulsh_32(o - y1 + y2, (uint32_t)b2, 16);
            y2 += mulsh_32(y1 - 2 * y2 + y3, (uint32_t)b, 16);
            y3 += mulsh_32(y2 - 2 * y3 + y4, (uint32_t)b, 16);
            y4 += mulsh_32(y3 - 2 * y4, (uint32_t)b, 16);
            y1 = clamp(y1, -(1 << 26), 1 << 26); y2 = clamp(y2, -(1 << 26), 1 << 26);   /* (a guard: 64 x full scale) */
            y3 = clamp(y3, -(1 << 26), 1 << 26); y4 = clamp(y4, -(1 << 24), 1 << 24);
            acc2 += mulsh_32(y4, (uint32_t)g2, 12) >> 1;
        }
        hp2 += mulsh_32(acc2 - hp2, ACID_HP2_K, 16);         /* the post-filter high-pass */
        s = mulsh_32(acc2 - hp2, (uint32_t)(a < 0 ? 0 : a), 13) >> 5;   /* Q20 x amp Q13 -> 32768 at 1.0 (~ Retro's loudness) */
        if (drv) s = mulsh_32(softclip(((s >> 1) * g4) >> 2), dcomp, 12) << 1;   /* DRV: a soft clip, x 1 .. 8.9 in, 1 / sqrt out */
        out[i] += voice_amp(soft_knee(s, 49152), m, i);      /* (the peaks over 1.5 bent under the host's 65535: no hard clip) */
        b0q += db0; kq += dk; gq += dg;
    }
    v->ph[0] = ph; v->ph[1] = inc; v->ph[2] = (uint32_t)hp2;
    v->s[0] = y1; v->s[1] = y2; v->s[4] = y3; v->s[5] = y4; v->s[6] = fbl; v->s[7] = hpl;
}

/* the host's amplitude from ENV 1 (a gate in the presets) and the velocity: the velocity undone (the accent is the dynamics) */
static int32_t acid_amp(track_t *t, voice_t *v, int32_t adsr)
{
    (void)t;
    return mulsh_32(adsr, ACID_VINV[v->vel & 127], 12);
}

static const engine_t ACID_DSP = {
    .name = "Acid",
    .note_on = acid_note_on,
    .render = acid_render,
    .amp = acid_amp,
    .poly = 1,          /* mono: every note on one voice (interface 1.7) */
    .keep = 0xF3,       /* the filter's states: s[0] s[1] s[4..7] */
};

static const char *const WAVES[] = {"Saw", "Sqr", 0};
static const KEngine ACID_DESC = {
    .name = "Acid",
    .uid = K_UID('a', 'c', 'i', 'd'),
    .page_title = {"", "", ""},
    .edit = {
        {"WAVE", KF_ENUM, 0, 1, 0, WAVES, 0},
        {"CUT", KF_PCT, 0, 127, 40, 0, 0},          /* 314 .. 2394 Hz, exponential (Open303's range) */
        {"RES", KF_PCT, 0, 127, 85, 0, 0},
        {"ENV", KF_PCT, 0, 127, 50, 0, 0},          /* the envelope's depth */
        {"DCY", KF_TIME, ACID_DCY_MIN, ACID_DCY_MAX, 83, 0, 0},   /* 200 .. 2000 ms (400) */
        {"ACC", KF_PCT, 0, 127, 80, 0, 0},          /* the accent's depth */
        {"TUNE", KF_INT, -100, 100, 0, 0, "ct"},
        {"SLID", KF_TIME, ACID_SLD_MIN, ACID_SLD_MAX, 56, 0, 0},  /* the slide's time, 10 .. 510 ms (60, Open303's) */
        {"DRV", KF_PCT, 0, 127, 0, 0, 0},
    },
    .macro = {A_CUT, A_RES, A_ENV, A_DCY},
    .nparams = A_N,
    .page2 = A_TUNE,
    .nomod = K_NOMOD_FILTER | K_NOMOD_SHAPE,   /* its own envelopes move the filter; the host's do not reach it */
};

/* The screens: a small box of six knobs in a row (picture 1 draws the box behind them), over it the filter's sweep (picture 0).
 * K1 .. K4 take WAVE CUT RES ENV, then DCY ACC (the row in hand lit). EDIT: the same box with its three. */
#define KN(k, x) {K_W_KNOB, 0, k, x, 6, 1, 4, 0}
static const KWidget SOUND_W[] = {
    {K_W_PICTURE, K_ST_FLAT, 0, 0, 0, 6, 6, 0},   /* the sweep */
    {K_W_PICTURE, K_ST_FLAT, 1, 0, 6, 6, 4, 0},   /* the box (behind the knobs: drawn first) */
    KN(A_WAVE, 0), KN(A_CUT, 1), KN(A_RES, 2), KN(A_ENV, 3), KN(A_DCY, 4), KN(A_ACC, 5),
};
static const KWidget EDIT_W[] = {
    {K_W_PICTURE, K_ST_FLAT, 0, 0, 0, 6, 6, 0},
    {K_W_PICTURE, K_ST_FLAT, 2, 0, 6, 6, 4, 0},   /* the box, half as wide */
    KN(A_TUNE, 0), KN(A_SLID, 1), KN(A_DRV, 2),
};
/* a large screen (a desktop, the web): the sweep at the left, the box of knobs beside it; EDIT under them (a desktop shows the two
 * one over the other): the sound's scope at the left, its three knobs under the first three */
#define KL(k, x) {K_W_KNOB, 0, k, x, 1, 1, 4, 0}
static const KWidget SOUND_L[] = {
    {K_W_PICTURE, K_ST_FLAT, 0, 0, 0, 6, 6, 0},
    {K_W_PICTURE, K_ST_FLAT, 1, 6, 1, 6, 4, 0},
    KL(A_WAVE, 6), KL(A_CUT, 7), KL(A_RES, 8), KL(A_ENV, 9), KL(A_DCY, 10), KL(A_ACC, 11),
};
static const KWidget EDIT_L[] = {
    {K_W_SCOPE, K_ST_FLAT, -1, 0, 0, 6, 6, 0},
    {K_W_PICTURE, K_ST_FLAT, 2, 6, 1, 6, 4, 0},
    KL(A_TUNE, 6), KL(A_SLID, 7), KL(A_DRV, 8),
};
/* a small wide screen (the Zero, a handheld): the sweep shorter, the knobs taller */
#define KS(k, x) {K_W_KNOB, 0, k, x, 4, 1, 6, 0}
static const KWidget SOUND_S[] = {
    {K_W_PICTURE, K_ST_FLAT, 0, 0, 0, 6, 4, 0},
    {K_W_PICTURE, K_ST_FLAT, 1, 0, 4, 6, 6, 0},
    KS(A_WAVE, 0), KS(A_CUT, 1), KS(A_RES, 2), KS(A_ENV, 3), KS(A_DCY, 4), KS(A_ACC, 5),
};
static const KWidget EDIT_S[] = {
    {K_W_PICTURE, K_ST_FLAT, 0, 0, 0, 6, 4, 0},
    {K_W_PICTURE, K_ST_FLAT, 2, 0, 4, 6, 6, 0},
    KS(A_TUNE, 0), KS(A_SLID, 1), KS(A_DRV, 2),
};
static const KLayout LAYOUTS[] = {
    {K_SCR_LARGE, K_LPAGE_SOUND, 12, 6, (uint8_t)(sizeof SOUND_L / sizeof SOUND_L[0]), SOUND_L},
    {K_SCR_LARGE, K_LPAGE_EDIT, 12, 6, (uint8_t)(sizeof EDIT_L / sizeof EDIT_L[0]), EDIT_L},
    {K_SCR_SMALL, K_LPAGE_SOUND, 6, 10, (uint8_t)(sizeof SOUND_S / sizeof SOUND_S[0]), SOUND_S},
    {K_SCR_SMALL, K_LPAGE_EDIT, 6, 10, (uint8_t)(sizeof EDIT_S / sizeof EDIT_S[0]), EDIT_S},
    {K_SCR_ANY, K_LPAGE_SOUND, 6, 10, (uint8_t)(sizeof SOUND_W / sizeof SOUND_W[0]), SOUND_W},
    {K_SCR_ANY, K_LPAGE_EDIT, 6, 10, (uint8_t)(sizeof EDIT_W / sizeof EDIT_W[0]), EDIT_W},
};

static const KHostApi *host;
static void acid_attach(const KHostApi *h) { host = h; }

/* the cutoff (log2 Hz Q16) after `ms` of a note, accented or not, from the values (as the DSP, at block steps of ~0.73 ms) */
static int32_t acid_sweep_at(const int16_t *val, int n, int ms, int acc)
{
    CutK ck;
    const int32_t cut = n > A_CUT ? clamp(val[A_CUT], 0, 127) : 40, envm = n > A_ENV ? clamp(val[A_ENV], 0, 127) : 50;
    const int32_t accq = n > A_ACC ? U127_Q12(clamp(val[A_ACC], 0, 127)) : 0;
    const uint32_t dc = acc ? ACID_DCY_ACCENT : ACID_DCY[clamp(n > A_DCY ? val[A_DCY] : 83, ACID_DCY_MIN, ACID_DCY_MAX) - ACID_DCY_MIN];
    int32_t menv = 1 << 24, rc2 = 0, b, blocks = ms * 1378 / 1000;   /* 1.378 blocks a ms */
    const int32_t e = U127_Q12(envm), cc = U127_Q12(cut);
    const int32_t slo = ((e * 15458) >> 12) + 3019, shi = ((e * 17181) >> 12) + 3540;
    ck.scale = ((4096 - cc) * slo + cc * shi) >> 12;
    ck.off = (int32_t)(((uint32_t)cc * 3165u) >> 4) + 4939038;
    ck.base = ACID_L_314 + mulsh_32(ACID_L_SPAN, (uint32_t)cc, 12);
    ck.accg = accq;
    for (b = 0; b < blocks; b += 8) {   /* (eight blocks a step: the picture's resolution) */
        int q;
        for (q = 0; q < 8; q++) {
            menv = mulsh_32(menv, dc, 16);
            rc2 = (acc ? menv : 0) + mulsh_32(rc2 - (acc ? menv : 0), ACID_RC2, 16);
        }
    }
    return acid_cut(&ck, menv, rc2);
}

/* picture 0: the filter's sweep, the cutoff (100 Hz .. 12.8 kHz up) against the time (0 .. 800 ms along): a plain note's as a
 * filled shape, an accented note's (shorter, with its wow) as a line over it, the note sounding now a dot where it is on its
 * sweep. Pictures 1, 2: the box behind the knobs (2: half as wide, EDIT's three) */
static void acid_draw(const KDraw *g, int id, const int16_t *val, int n)
{
    const int sq = (host && host->screen == K_SCR_SQUARE) || g->screen == K_SCR_SQUARE;
    if (id == 1 || id == 2) {   /* the box: a raised panel framed by a line, the knobs on it */
        const int w = id == 2 ? g->w / 2 : g->w;
        g->fill(g, 1, 1, w - 2, g->h - 2, K_COL_FAINT);
        g->line(g, 2, 0, w - 3, 0, K_COL_LINE);
        g->line(g, 2, g->h - 1, w - 3, g->h - 1, K_COL_LINE);
        g->line(g, 0, 2, 0, g->h - 3, K_COL_LINE);
        g->line(g, w - 1, 2, w - 1, g->h - 3, K_COL_LINE);
        return;
    }
    if (id != 0 || g->w < 16 || g->h < 12) return;
    {
        const int top = 1, bot = g->h - 1, span = bot - top;
        int step = sq ? 4 : 2;   /* (three calls a step: within the frame's 400 however wide it is) */
        const int32_t lo = 435412;   /* log2(100 Hz) Q16; seven octaves up the frame */
        const int acc = n > A_ACC && val[A_ACC] > 0;
        int x, pax = -1, pay = 0, k;
        while (g->w / step > 120) step++;
        for (k = 1; k < 7; k += 2) g->line(g, 0, bot - span * k / 7, g->w - 1, bot - span * k / 7, K_COL_FAINT);   /* 200, 800, 3.2k */
        for (x = 0; x < g->w; x += step) {
            const int ms = x * 800 / (g->w - 1);
            int y = bot - mulsh_32(acid_sweep_at(val, n, ms, 0) - lo, (uint32_t)span, 16) / 7;
            y = y < top ? top : y > bot ? bot : y;
            g->fill(g, x, y, step, bot - y + 1, g->lit ? K_COL_LINE : K_COL_FAINT);
            g->line(g, x, y, x + step - 1 < g->w ? x + step - 1 : g->w - 1, y, g->lit ? K_COL_ACCENT : K_COL_MID);
            if (acc) {
                int ya = bot - mulsh_32(acid_sweep_at(val, n, ms, 1) - lo, (uint32_t)span, 16) / 7;
                ya = ya < top ? top : ya > bot ? bot : ya;
                if (pax >= 0) g->line(g, pax, pay, x, ya, K_COL_TEXT);
                pax = x; pay = ya;
            }
        }
        static uint32_t seen;   /* (the UI's: the tick at the last frame) */
        const uint32_t tick = acid_live_tick;
        const int moving = tick != seen;
        seen = tick;
        if (moving && acid_live_age > 0) {   /* the note sounding now: where on its sweep */
            const int ms = acid_live_age * 1000 / 1378;
            if (ms < 800) {
                const int lx = ms * (g->w - 1) / 800;
                int ly = bot - mulsh_32(acid_live_l - lo, (uint32_t)span, 16) / 7;
                ly = ly < top + 2 ? top + 2 : ly > bot - 2 ? bot - 2 : ly;
                g->fill(g, lx - 2, ly - 2, 5, 5, K_COL_TEXT);
            }
        }
    }
}

#include "acid_presets.h"

static const KPluginExt ACID_EXT = {
    sizeof(KPluginExt), K_PLUGIN_EXT_VERSION, acid_attach, LAYOUTS, (uint8_t)(sizeof LAYOUTS / sizeof LAYOUTS[0]), acid_draw,
    ACID_PRESETS, (uint8_t)(sizeof ACID_PRESETS / sizeof ACID_PRESETS[0]),   /* its factory presets (version 2) */
};
static const KPlugin ACID = {sizeof(KPlugin), K_PLUGIN_ABI, 2, K_PLUG_SYNTH, &ACID_DESC, &ACID_DSP,
                             FS, sizeof acid_x + 24, 50, 0, 0, &ACID_EXT};   /* ~47 us a block on the FM-1 (fm1-emu: ~15 k instructions; README) */

KREISEL_PLUGIN_EXPORT const KPlugin *kreisel_plugin(void) { return &ACID; }
