/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* EPNO, an electric piano as an outside plugin (docs/PLUGIN_SDK.md), id `epno`. An original design, not modelled on any product.
 *
 * A struck tine, a tonebar beside it, a pickup that hears them:
 *   tine     a sine carrier phase-modulated by a sine at the same pitch (2-operator FM, ratio 1). The index is the hammer: the
 *            velocity raises it (harder is brighter), it falls fast after the strike to a floor set by TONE. Held under Nyquist
 *            for high notes (the index's sidebands stay below ~18 kHz).
 *   strike   a short-lived partial at 6.94 x the pitch (the bell of the attack), its level from TINE and the velocity; left out
 *            where it would pass ~17 kHz, faded in below that.
 *   tonebar  the modulator itself, a pure sine at the pitch (in phase with the tine: no drifting DC), heard on its own at BODY:
 *            it swells in over ~25 ms and outlasts the tine (the tine's part decays twice as fast). More of it low on the keys.
 *   pickup   an asymmetric soft clip (tanh with an offset), its drive from BARK and the velocity: even harmonics, the bark;
 *            less of it at the top of the keys (aliasing). A DC blocker after it.
 *   tremolo  the amplitude, from one knob (TREM: depth, and the rate with it, 2.5 .. 7 Hz), one phase for every voice of a part.
 *            (A voice is mono in the SDK: no autopan.)
 * Envelopes are in a log domain (1/64 octave steps, a 64-entry table): each decays at a rate set once a block, so a decay is
 * exponential and key scaling is an addition. Higher notes decay sooner, lower ones ring longer. Released: a damper fade (REL),
 * the index falling with it (the damper takes the highs first); the amplitude ramps over the block (the host's), no click.
 * The amplitude curve is the engine's (the amp hook) times the host's ENV 1, so an attack or a sustain set there still shapes it.
 *
 * Parameters, on P_E0 .. :
 *   SOUND  0 TINE (strike and its hardness), 1 BARK (the pickup's drive), 2 DCAY (the decay), 3 TREM (tremolo)
 *   EDIT   4 TONE (brightness: the index's floor), 5 BODY (the tonebar), 6 REL (the damper), 7 VEL (how far the velocity
 *          moves level, brightness and bark; 0: every note as a firm one)
 * Integers only; no division and no 64-bit product in the sample loop. Per voice: ph[0..2], s[0..7]; the plugin's own state is
 * the tremolo's phase for each part (64 bytes). */
#include "kreisel_sdk.h"

enum { E_TINE, E_BARK, E_DCAY, E_TREM, E_TONE, E_BODY, E_REL, E_VEL, E_N };

/* v->s[]: 0 the amplitude's attenuation, 1 the tine's own (over the amplitude's), 2 the index's, 3 the strike's (each Q12 of
 * 1/64 octave), 4 and 5 the DC blocker (kept over a retrigger), 6 the tonebar's swell (0 .. 32767), 7 the
 * host's velocity taken back */
#define ATT_SHIFT 12
#define ATT_MAX (1024 << ATT_SHIFT)     /* 16 octaves down: silence */
#define ATT_GONE (960 << ATT_SHIFT)     /* ~ -90 dB: a released voice no longer heard */
#define EP_FIRST (1 << 20)              /* s[7]: the note's first block to come (over the velocity's Q10, < 2^17) */

/* 2^(-i/64) in Q16, i = 0 .. 63 (the first held at 65535) */
static const uint16_t EXP2N[64] = {
    65535, 64830, 64132, 63441, 62757, 62081, 61413, 60751, 60097, 59449, 58809, 58176, 57549, 56929, 56316, 55709,
    55109, 54515, 53928, 53347, 52773, 52204, 51642, 51085, 50535, 49991, 49452, 48920, 48393, 47871, 47356, 46846,
    46341, 45842, 45348, 44859, 44376, 43898, 43425, 42958, 42495, 42037, 41584, 41136, 40693, 40255, 39821, 39392,
    38968, 38548, 38133, 37722, 37316, 36914, 36516, 36123, 35734, 35349, 34968, 34591, 34219, 33850, 33486, 33125,
};
/* 32768 x 2^(-q/64), q >= 0 (in 1/64 octave) */
static inline int32_t ex2(int32_t q)
{
    if (q <= 0) return 32767;
    if (q >= 64 * 15) return 0;
    return (int32_t)((uint32_t)EXP2N[q & 63] >> (1 + (q >> 6)));
}
static inline int32_t att_amp(int32_t att) { return ex2(att >> ATT_SHIFT); }
/* the rate a block that falls 60 dB in 20 ms x 2^(lt/64): lt = 0 is 20 ms, each 64 doubles the time */
static inline int32_t rate_of(int32_t lt)
{
    const int32_t r = (11889 * ex2(lt < 0 ? 0 : lt)) >> 12;   /* 60 dB = 640 << 12; 20 ms = 27.6 blocks */
    return r < 1 ? 1 : r;
}

/* the times, as lt (above), by the note (key: semitones from middle C) */
static inline int32_t lt_main(const int16_t *p, int32_t key)  /* the amplitude: 6 s at DCAY 64 on middle C; 0.6x an octave up */
{
    return 398 + 2 * clamp(p[P_E0 + E_DCAY], 0, 127) - 4 * key;
}
static inline int32_t lt_rel(const int16_t *p) { return 85 + clamp(p[P_E0 + E_REL], 0, 127) * 5 / 2; }   /* 50 ms .. 1.5 s */

/* the tremolo: a phase for each part that plays the engine (its track_t's address finds it) */
#define EP_PARTS 8
static struct { const track_t *t; uint32_t ph; } trem[EP_PARTS];
static uint32_t *trem_of(const track_t *t)
{
    int i;
    for (i = 0; i < EP_PARTS; i++) if (trem[i].t == t) return &trem[i].ph;
    for (i = 0; i < EP_PARTS; i++) if (!trem[i].t) { trem[i].t = t; return &trem[i].ph; }
    return &trem[0].ph;
}
static void ep_block(track_t *t)
{
    const int32_t k = clamp(t->p[P_E0 + E_TREM], 0, 127);
    if (k) *trem_of(t) += (uint32_t)(640 + k * 9) * 12174u;   /* 2.5 .. 7 Hz (Q8 Hz x 2^32 x CTL / FS / 256) */
}

static void ep_note_on(track_t *t, voice_t *v)
{
    (void)t;
    v->ph[0] = v->ph[1] = v->ph[2] = 0;   /* (a retrigger puts the phases back: no step) */
    v->s[0] = v->s[1] = v->s[2] = v->s[3] = 0;
    v->s[6] = 0;
    v->s[7] = ((127 << 10) / (v->vel ? v->vel : 1)) | EP_FIRST;   /* the host's velocity taken back (Q10): once a note, not a block */
}

/* the amplitude, once a control tick: the engine's decay (the damper's after the key), the tremolo, times the host's ENV 1 */
static int32_t ep_amp(track_t *t, voice_t *v, int32_t adsr)
{
    const int16_t *p = t->p;
    int32_t rate = rate_of(lt_main(p, (int32_t)v->note - 60)), a, k;
    if (!v->gate) rate += rate_of(lt_rel(p));
    v->s[0] = v->s[0] + rate > ATT_MAX ? ATT_MAX : v->s[0] + rate;
    a = att_amp(v->s[0]);
    if ((k = clamp(p[P_E0 + E_TREM], 0, 127)) != 0) {
        const int32_t depth = (k * k * 179) >> 7, lfo = sine_i(*trem_of(t));   /* up to ~0.7 */
        a = mulq15(a, 32767 - mulq15(depth, (32767 - lfo) >> 1));
    }
    return mulq15(a, adsr);
}

#define BELL_LIM 235000000u   /* the strike's partial (6.94 x) at ~16.8 kHz: none above */
/* (512 << 10) / (512 + 16 i): the drive's gain taken back, by (drive - 1) in 1/16 */
static const uint16_t DRIVE_BACK[49] = {
    1024, 992, 963, 936, 910, 885, 862, 840, 819, 799, 780, 762, 744, 728, 712, 697,
    682, 668, 655, 642, 630, 618, 606, 595, 585, 574, 564, 555, 546, 537, 528, 520,
    512, 504, 496, 489, 481, 474, 468, 461, 455, 448, 442, 436, 431, 425, 420, 414,
    409,
};
#define SB_GAIN 135           /* a voice's level for each step of velocity (Q15: 127 -> ~0.47) */

static void ep_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    const int32_t tine = clamp(p[P_E0 + E_TINE], 0, 127), bark = clamp(p[P_E0 + E_BARK], 0, 127);
    const int32_t tone = clamp(p[P_E0 + E_TONE], 0, 127), body = clamp(p[P_E0 + E_BODY], 0, 127);
    const int32_t vs = clamp(p[P_E0 + E_VEL], 0, 127), key = (int32_t)v->note - 60, vel = v->vel ? v->vel : 1;
    const int32_t ve = ((vel * vs + 100 * (127 - vs)) * 258) >> 15;   /* the velocity the sound hears, 1 .. 127 (over 127) */
    const int32_t va = ve * SB_GAIN, rel = !v->gate;
    const uint32_t inc = m->inc, incb = inc * 7u - (inc >> 4);
    int32_t lt, ai0, ai1, at, ab0, ab1, ilim, ifloor, iatt, idx, didx, gt, gb, gl, dgl, dr, bias, sb, kf, sh, post, i32;
    int32_t x1 = v->s[4], y1 = v->s[5];
    uint32_t phc = v->ph[0], phb = v->ph[2], i;
    const int first = (v->s[7] & EP_FIRST) != 0;   /* (the note's first block) */
    if (rel && v->s[0] >= ATT_GONE) return;   /* (damped: nothing left to hear) */
    v->s[7] &= ~EP_FIRST;

    /* the envelopes, once a block: the index's (fast, faster up the keys; the damper darkens it), the tine's own, the strike's */
    lt = 314 - 5 * key;
    ai0 = att_amp(v->s[2]);
    v->s[2] = clamp(v->s[2] + rate_of(lt) + (rel ? rate_of(lt_rel(p)) : 0), 0, ATT_MAX);
    ai1 = att_amp(v->s[2]);
    v->s[1] = clamp(v->s[1] + rate_of(lt_main(p, key)), 0, ATT_MAX);   /* (the amplitude's rate again: the tine twice as fast) */
    at = att_amp(v->s[1]);
    lt = 165 - 4 * key + tine / 2;                                         /* the strike: ~0.12 s on middle C */
    ab0 = att_amp(v->s[3]);
    v->s[3] = clamp(v->s[3] + rate_of(lt) + (rel ? rate_of(0) : 0), 0, ATT_MAX);
    ab1 = att_amp(v->s[3]);
    if (v->s[6] < 32767) v->s[6] = v->s[6] + 960 > 32767 ? 32767 : v->s[6] + 960;   /* the tonebar's swell: ~25 ms */

    /* the index (radians Q8): a floor by TONE (higher notes a little brighter), the strike's part by the velocity and TINE */
    ifloor = 40 + tone * 3 / 2 + 2 * key;
    if (ifloor < 0) ifloor = 0;
    iatt = (ve * (100 + tine * 3) * 258) >> 15;
    ilim = clamp(((17612 * ex2((clamp(m->pitch16, 0, 2047) * 85) >> 8)) >> 10) - 512, 0, 1024);   /* (index + 2) x pitch under
                                                                                   * ~18 kHz: 256 x 18000 over f, f by the pitch */
    /* the first block rises from the floor to the strike, as the strike's partial rises from none: a retrigger's phases go on
     * (the host's), and the full index and partial at once on them stepped the wave (+27 dB over 6 kHz, version 1) */
    i32 = clamp(ifloor + (first ? 0 : mulq15(iatt, ai0)), 0, ilim);
    idx = (i32 * 20861) >> 8;                                              /* radians Q8 -> phase a unit of the modulator */
    i32 = clamp(ifloor + mulq15(iatt, ai1), 0, ilim);
    didx = (((i32 * 20861) >> 8) - idx) >> CTL_LOG2;   /* (a block is CTL long) */

    /* the levels: the tine's part, the tonebar (more of it low on the keys), the strike (harder, more), faded near its limit */
    gt = mulq15(va, at);
    gb = (va * clamp(body * clamp(128 - 2 * key, 64, 192) >> 7, 0, 190)) >> 8;
    gb = mulq15(gb, v->s[6]);
    gl = dgl = 0;
    if (inc < BELL_LIM && tine) {
        const int32_t s = mulq15(va, tine * ve * 2), fade = clamp(((int32_t)((BELL_LIM - inc) >> 12) * 73) >> 16, 0, 16);   /* 0 at the limit, full half an octave under */
        const int32_t l0 = (mulq15(s, ab0) * fade) >> 5, l1 = (mulq15(s, ab1) * fade) >> 5;   /* (half of s at most) */
        gl = first ? 0 : l0;
        dgl = (l1 - gl) >> CTL_LOG2;
    }

    /* the pickup: drive 1 .. 4x by BARK and the velocity, an offset for the asymmetry; less of both high on the keys */
    kf = v->note > 84 ? clamp(((108 - (int32_t)v->note) * 171) >> 7, 0, 32) : 32;
    dr = 256 + (((bark * ve) >> 6) * 3 * kf >> 5);
    bias = bark * 80 * kf >> 5;
    sh = v->note > 98 ? 2 : v->note > 90 ? 1 : 0;   /* the top keys into the clip's linear part (its odd harmonics would fold) */
    sb = softclip(bias);
    /* after it: the host's velocity taken back (it scales by vel; the sound did already by ve), the drive's gain taken back */
    post = ((v->s[7] & (EP_FIRST - 1)) * DRIVE_BACK[clamp((dr - 256) >> 4, 0, 48)]) >> 10;   /* Q10 */
    post = clamp(post, 0, 32767);

    for (i = 0; i < n; i++) {
        const int32_t mod = sine_i(phc);   /* (the modulator at the carrier's phase: ratio 1, odd about 0, so no DC) */
        const int32_t car = sine_i(phc + (uint32_t)mod * (uint32_t)idx);   /* (modular: a phase wraps) */
        int32_t x = mulq15(car, gt) + mulq15(mod, gb), y, hp;
        if (gl | dgl) { x += mulq15(sine_i(phb), gl); gl += dgl; }
        y = (softclip((((x * dr) >> 8) >> sh) + bias) - sb) << sh;
        hp = y - x1 + ((y1 * 32604) >> 15);   /* DC blocker, ~35 Hz */
        x1 = y; y1 = hp;
        out[i] += voice_amp((hp * post) >> 10, m, i);
        phc += inc; phb += incb;
        idx += didx;
    }
    v->ph[0] = phc; v->ph[2] = phb;
    v->s[4] = x1; v->s[5] = y1;
}

static const engine_t EP_DSP = {
    .name = "EPNO",
    .note_on = ep_note_on,
    .render = ep_render,
    .amp = ep_amp,
    .block = ep_block,
    .keep = (1u << 4) | (1u << 5),   /* the DC blocker over a retrigger */
};

static const KEngine EP_DESC = {
    .name = "EPNO",
    .uid = K_UID('e', 'p', 'n', 'o'),
    .page_title = {"", "", ""},
    .edit = {
        {"TINE", KF_PCT, 0, 127, 64, 0, 0},
        {"BARK", KF_PCT, 0, 127, 40, 0, 0},
        {"DCAY", KF_PCT, 0, 127, 64, 0, 0},
        {"TREM", KF_PCT, 0, 127, 0, 0, 0},
        {"TONE", KF_PCT, 0, 127, 64, 0, 0},
        {"BODY", KF_PCT, 0, 127, 64, 0, 0},
        {"REL", KF_PCT, 0, 127, 40, 0, 0},
        {"VEL", KF_PCT, 0, 127, 100, 0, 0},
    },
    .macro = {E_TONE, E_BARK, E_TREM, E_DCAY},   /* live: brightness, bark, tremolo, decay */
    .nparams = E_N,
    .page2 = 4,
    .nomod = K_NOMOD_FILTER | K_NOMOD_SHAPE,
};

/* The screens: the picture (the tine over its pickup) and the knobs, the host's */
#define KN(k, x, y, w, h) {K_W_KNOB, 0, k, x, y, w, h, 0}
static const KWidget SQ_SOUND[] = {   /* the FM-1: the picture over K1 .. K4 */
    {K_W_PICTURE, K_ST_FLAT, 0, 0, 0, 4, 2, 0},
    KN(0, 0, 2, 1, 2), KN(1, 1, 2, 1, 2), KN(2, 2, 2, 1, 2), KN(3, 3, 2, 1, 2),
};
static const KWidget SQ_EDIT[] = {
    {K_W_PICTURE, K_ST_FLAT, 0, 0, 0, 4, 2, 0},
    KN(4, 0, 2, 1, 2), KN(5, 1, 2, 1, 2), KN(6, 2, 2, 1, 2), KN(7, 3, 2, 1, 2),
};
static const KWidget SM_SOUND[] = {   /* wide and short: the picture beside the knobs */
    {K_W_PICTURE, K_ST_FLAT, 0, 0, 0, 2, 2, 0},
    KN(0, 2, 0, 1, 2), KN(1, 3, 0, 1, 2), KN(2, 4, 0, 1, 2), KN(3, 5, 0, 1, 2),
};
static const KWidget SM_EDIT[] = {
    {K_W_PICTURE, K_ST_FLAT, 0, 0, 0, 2, 2, 0},
    KN(4, 2, 0, 1, 2), KN(5, 3, 0, 1, 2), KN(6, 4, 0, 1, 2), KN(7, 5, 0, 1, 2),
};
static const KWidget LG_SOUND[] = {   /* a desktop (SOUND and EDIT one over the other): the picture, then the sound itself */
    {K_W_PICTURE, K_ST_FLAT | K_ST_FRAME, 0, 0, 0, 4, 2, 0},
    KN(0, 4, 0, 1, 2), KN(1, 5, 0, 1, 2), KN(2, 6, 0, 1, 2), KN(3, 7, 0, 1, 2),
};
static const KWidget LG_EDIT[] = {
    {K_W_SCOPE, K_ST_FLAT | K_ST_FRAME, -1, 0, 0, 4, 2, 0},
    KN(4, 4, 0, 1, 2), KN(5, 5, 0, 1, 2), KN(6, 6, 0, 1, 2), KN(7, 7, 0, 1, 2),
};
#define NW(a) (uint8_t)(sizeof(a) / sizeof((a)[0]))
static const KLayout LAYOUTS[] = {
    {K_SCR_SQUARE, K_LPAGE_SOUND, 4, 4, NW(SQ_SOUND), SQ_SOUND}, {K_SCR_SQUARE, K_LPAGE_EDIT, 4, 4, NW(SQ_EDIT), SQ_EDIT},
    {K_SCR_SMALL, K_LPAGE_SOUND, 6, 2, NW(SM_SOUND), SM_SOUND}, {K_SCR_SMALL, K_LPAGE_EDIT, 6, 2, NW(SM_EDIT), SM_EDIT},
    {K_SCR_LARGE, K_LPAGE_SOUND, 8, 2, NW(LG_SOUND), LG_SOUND}, {K_SCR_LARGE, K_LPAGE_EDIT, 8, 2, NW(LG_EDIT), LG_EDIT},
};

static int ci(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* picture 0: the tine, fixed at its mount, the tonebar over it, the pickup at its tip. The tine bends with the track's sound
 * (its last sample), its swing (faint) with the sound's peak; the pickup shakes a little and its lead carries the wave. */
static void ep_draw(const KDraw *g, int id, const int16_t *val, int n)
{
    int8_t sc[48];
    const int W = g->w, H = g->h, sq = g->screen == K_SCR_SQUARE;
    const int col = g->lit ? K_COL_ACCENT : K_COL_TEXT;
    const int tv = n > E_TINE ? ci(val[E_TINE], 0, 127) : 64, bv = n > E_BODY ? ci(val[E_BODY], 0, 127) : 64;
    const int mid = H / 2 + H / 10, x0 = W / 14 + 2, xl = W * 62 / 100, A = H / 4, thick = H >= 120 ? 2 : 1;
    const int segs = sq ? 6 : 10;
    int cnt = 0, peak = 0, tip = 0, i, k, s;
    if (id != 0 || W < 32 || H < 16) return;
    if (g->scope) cnt = g->scope(g, sc, sq ? 24 : 48);
    for (i = 0; i < cnt; i++) { const int a = sc[i] < 0 ? -sc[i] : sc[i]; if (a > peak) peak = a; }
    if (cnt) tip = sc[cnt - 1];

    /* the mount; the tonebar over the tine, joined to it at the mount as a fork: thicker with BODY */
    g->fill(g, 0, ci(mid - H / 3, 0, H - 1), x0, ci(H * 2 / 3, 1, H), K_COL_LINE);
    {
        const int th = 2 + bv * (H / 8) / 127, y = ci(mid - H / 4 - th, 0, H - th), fw = W / 60 > 2 ? W / 60 : 2;
        g->fill(g, x0, y, (xl - x0) * 7 / 10, th, K_COL_MID);
        g->fill(g, x0, y, fw, mid - y + 1, K_COL_MID);
    }
    /* the swing (faint, both sides), then the tine where it is now */
    for (s = 0; s < 3; s++) {
        const int d = s == 0 ? peak * A / 128 : s == 1 ? -peak * A / 128 : tip * A / 128;
        int px = x0, py = mid;
        if (s < 2 && peak < 3) continue;
        for (k = 1; k <= segs; k++) {
            const int x = x0 + (xl - x0) * k / segs, y = ci(mid - d * k * k / (segs * segs), 0, H - 1);
            g->line(g, px, py, x, y, s < 2 ? K_COL_FAINT : col);
            if (s == 2 && thick > 1) g->line(g, px, ci(py + 1, 0, H - 1), x, ci(y + 1, 0, H - 1), col);
            px = x; py = y;
        }
    }
    /* the tip's weight: larger with TINE */
    {
        const int mw = 2 + tv * 4 / 127, y = ci(mid - tip * A / 128 - mw / 2, 0, H - mw);
        g->fill(g, ci(xl - mw / 2, 0, W - mw), y, mw, mw, col);
    }
    /* the pickup: its pole toward the tip, the coil, its lead to the right carrying the sound */
    {
        const int sh = tip * 2 / 128, pw = W / 10 > 4 ? W / 10 : 4, ph = H / 2, px = ci(xl + W / 14 + sh, 0, W - pw - 1);
        const int py = ci(mid - ph / 2, 0, H - ph), lx = px + pw, step = sq ? 2 : 1;
        g->line(g, ci(px - W / 28, 0, W - 1), mid, px, mid, K_COL_MID);
        g->fill(g, px, py, pw, ph, K_COL_LINE);
        for (k = py + 2; k < py + ph - 1; k += ph / 16 > 3 ? ph / 16 : 3) g->line(g, px + 1, k, px + pw - 2, k, K_COL_FAINT);
        if (peak >= 3)   /* the field: lines from the pole toward the tine, more with the sound */
            for (k = 0; k < 1 + peak / 40 && k < 4; k++) {
                const int dy = (k + 1) * H / 16;
                g->line(g, ci(xl + 2, 0, W - 1), ci(mid - dy, 0, H - 1), ci(px - W / 28, 0, W - 1), mid, K_COL_FAINT);
                g->line(g, ci(xl + 2, 0, W - 1), ci(mid + dy, 0, H - 1), ci(px - W / 28, 0, W - 1), mid, K_COL_FAINT);
            }
        if (lx + 4 < W) {
            int qx = lx, qy = mid;
            const int len = W - 1 - lx;
            for (k = 0; k < cnt; k += step) {
                const int x = lx + len * (k + 1) / cnt, y = ci(mid - sc[k] * (H / 4) / 128, 0, H - 1);
                g->line(g, qx, qy, x, y, peak >= 3 ? col : K_COL_FAINT);
                qx = x; qy = y;
            }
            if (!cnt) g->line(g, lx, mid, W - 1, mid, K_COL_FAINT);
        }
    }
}

/* factory presets: plain names, the host's words (A D S R: ENV 1; its release long, so REL is the damper) */
static const KPreset PRESETS[] = {
    {"Mellow", "TINE=40 BARK=12 DCAY=72 TONE=38 BODY=82 REL=40 VEL=90 A=0 D=100 S=100 R=70"},
    {"Bright", "TINE=92 BARK=40 DCAY=60 TONE=96 BODY=48 REL=35 VEL=100 A=0 D=100 S=100 R=70"},
    {"Bark", "TINE=72 BARK=120 DCAY=56 TONE=74 BODY=60 REL=32 VEL=127 A=0 D=100 S=100 R=70"},
    {"Bell Tine", "TINE=127 BARK=18 DCAY=84 TONE=60 BODY=30 REL=60 VEL=100 A=0 D=100 S=100 R=80"},
    {"Shimmer", "TINE=60 BARK=30 DCAY=76 TREM=72 TONE=56 BODY=70 REL=55 VEL=100 A=0 D=100 S=100 R=80"},
    {"Felt", "TINE=14 BARK=0 DCAY=36 TONE=20 BODY=104 REL=24 VEL=70 A=0 D=100 S=100 R=60"},
};

static const KPluginExt EP_EXT = {sizeof(KPluginExt), K_PLUGIN_EXT_VERSION, 0, LAYOUTS, NW(LAYOUTS), ep_draw,
                                  PRESETS, NW(PRESETS), "E.Piano"};   /* (its full name where there is room: SDK 1.6) */
static const KPlugin EPNO = {sizeof(KPlugin), K_PLUGIN_ABI, 2, K_PLUG_SYNTH, &EP_DESC, &EP_DSP,
                             FS, sizeof trem, 20, 0, 0, &EP_EXT};   /* ~20 us a voice a block: fm1-emu, 0.84 x Retro (memos/2026-11-22_epiano.md) */

KREISEL_PLUGIN_EXPORT const KPlugin *kreisel_plugin(void) { return &EPNO; }
