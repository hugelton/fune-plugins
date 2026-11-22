/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* ENSEMBLE, an outside plugin (id `kens`): a divide-down string machine, not a circuit-level emulation. Built into Kreisel until
 * 2026-11-22 (src/eng_kensemble.c, switched off since 2026-11-07); the same sound bit for bit (tests/ens_vec_plugin_test.c).
 *
 * Four band-limited saw registers (bass -8ve, cello 8', viola 8' slightly detuned, violin +8ve) and a pulse "brass" registration
 * per voice; then, once a part on the sum of its voices (the SDK's post stage, interface 1.5), a tone filter, a high-pass and a
 * three-tap modulated short-delay ensemble. The chorus belongs to the part, not the voice: one 512-sample delay a part, kept in
 * the part's memory the host gives (KHostApi.part_mem). No allocation, no floating point, no 64-bit product.
 *
 * SOUND  0 BASS 1 CELLO 2 VIOLA 3 VLN 4 BRASS 5 TONE 6 ENS 7 RATE 8 DEP 9 DTN 10 LEVEL
 * EDIT   11 HPF 12 DRV 13 NOIS 14 ENV (the host's ENV 1 -> filter: it brightens the violin) 15 VEL
 * The host's LFO -> SHP blends the brass in. */
#include "kreisel_sdk.h"

static const KHostApi *host;
static void *(*part_mem)(const void *part);   /* the host's part memory (0: an older host; then no chorus) */
static void ens_attach(const KHostApi *h)
{
    host = h;
    part_mem = h && h->size >= offsetof(KHostApi, part_mem) + sizeof h->part_mem ? h->part_mem : 0;
}

/* ------------------------------------------------------------------------------------------------------------- the voices --- */
static void ens_note_on(track_t *t, voice_t *v)
{
    (void)t;
    /* the four registers start at independent phases; a retrigger keeps them (engine_t.keep) */
    if (!v->s[2]) v->s[2] = (int32_t)(0x51F15EEDu ^ v->age);
    if (!v->ph[0] && !v->ph[1] && !v->ph[2] && !v->s[0]) {
        v->ph[0] = v->age * 0x9E3779B9u;
        v->ph[1] = v->age * 0x7F4A7C15u;
        v->ph[2] = v->age * 0x6C8E9CF5u;
        v->s[0] = (int32_t)(v->age * 0xB5297A4Du);
    }
}
static void ens_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    const int32_t bass = clamp(p[P_E0], 0, 127) * 64;
    const int32_t cello = clamp(p[P_E1], 0, 127) * 64;
    const int32_t viola = clamp(p[P_E2], 0, 127) * 64;
    const int32_t violin = clamp(p[P_E3], 0, 127) * 64;
    const int32_t brass = clamp(p[P_E4] + ((m->shape - (64 << 8)) >> 9), 0, 127) * 64;
    const int32_t treble = clamp(32767 + (m->cutoff >> 1), 0, 49152);
    const int32_t level = clamp(p[P_E10], 0, 127) * 258;
    const int32_t drv = clamp(p[P_E12], 0, 127), nois = clamp(p[P_E13], 0, 127) * 45;
    const int32_t vel = clamp(p[P_E15], 0, 127);
    const int32_t velc = v->vel ? (int32_t)((((127 - vel) * 127 + vel * (int32_t)v->vel) << 12) / (127 * (int32_t)v->vel)) : 4096;
    const uint32_t inc0 = m->inc >> 1, inc1 = m->inc, inc2 = m->inc << 1;
    uint32_t incv = cents_inc(clamp(m->pitch16, 0, 2047), clamp(p[P_E9], 0, 50), 0);
    uint32_t ph0 = v->ph[0], ph1 = v->ph[1], ph2 = v->ph[2], phv = (uint32_t)v->s[0];
    int32_t nst = v->s[2];
    uint32_t i;
    if (m->fine) incv += (uint32_t)((int32_t)(incv >> 12) * m->fine);
    for (i = 0; i < n; i++) {
        int32_t s = 0;
        if (bass) s += mulq15(osc_saw(ph0, inc0), bass);
        if (cello) s += mulq15(osc_saw(ph1, inc1), cello);
        if (viola) s += mulq15(osc_saw(phv, incv), viola);
        if (violin) s += (mulq15(osc_saw(ph2, inc2), violin) * treble) >> 15;   /* (|saw x violin| < 2^14, treble < 2^16: 32 bits) */
        if (brass) s += mulq15(osc_pulse(phv, incv, 0x80000000u), brass);
        if (nois) s += mulq15((int32_t)(noise32(&nst) >> 17) - 16384, nois);
        ph0 += inc0; ph1 += inc1; ph2 += inc2; phv += incv;
        if (drv) s = softclip(((s >> 2) * ((32768 + drv * 320) >> 2)) >> 11);
        s = mulq15(s, level);
        if (velc != 4096) s = vel_mul(s, velc);
        out[i] += voice_amp(s, m, i);
    }
    v->ph[0] = ph0; v->ph[1] = ph1; v->ph[2] = ph2;
    v->s[0] = (int32_t)phv;
    v->s[2] = nst;
}

/* ------------------------------------------------------------------------------------------- the part: tone and chorus --- */
#define KE_LEN 512u
#define KE_MASK (KE_LEN - 1u)
typedef struct {
    int16_t delay[KE_LEN];
    uint32_t slow, fast;
    uint16_t wr;
    int32_t lp, hp;
    int32_t lpcoef, hpcoef, ens, depth;
    uint32_t si, fi;
    uint8_t ready;
} Chorus;   /* 1068 bytes, in the part's memory (K_PART_MEM: 2048) */
_Static_assert(sizeof(Chorus) <= K_PART_MEM, "the chorus fits a part's memory");

/* a fractional, interpolated read of the 512-sample circular delay; the three taps have different base delays so their LFOs
 * never coincide */
static inline int32_t ke_tap(const Chorus *c, uint32_t w, uint32_t dq8)
{
    uint32_t d = dq8 >> 8, f = dq8 & 255u;
    int32_t a = c->delay[(w - d) & KE_MASK], b = c->delay[(w - d - 1u) & KE_MASK];
    return a + (int32_t)(((b - a) * (int32_t)f) >> 8);
}
static void ke_run(Chorus *c, int32_t *out, uint32_t n)
{
    const int32_t lpcoef = c->lpcoef, hpcoef = c->hpcoef, ens = c->ens, depth = c->depth;
    const uint32_t si = c->si, fi = c->fi;
    uint32_t p0, p1, p2, d0[3], d1[3], i;
    p0 = c->slow; p1 = c->fast; p2 = p1 + 0x55555555u;
    d0[0] = (uint32_t)(206 * 256 + ((sine_i(p0) * depth * 256) >> 15));
    d0[1] = (uint32_t)(276 * 256 + ((sine_i(p1) * depth * 256) >> 15));
    d0[2] = (uint32_t)(348 * 256 + ((sine_i(p2) * depth * 256) >> 15));
    d1[0] = (uint32_t)(206 * 256 + ((sine_i(p0 + si) * depth * 256) >> 15));
    d1[1] = (uint32_t)(276 * 256 + ((sine_i(p1 + fi) * depth * 256) >> 15));
    d1[2] = (uint32_t)(348 * 256 + ((sine_i(p2 + fi) * depth * 256) >> 15));
    for (i = 0; i < n; i++) {
        uint32_t w = c->wr;
        int32_t x = softclip(out[i]), y, wet = 0;
        c->lp += ((x - c->lp) * lpcoef) >> 15;
        c->hp += ((c->lp - c->hp) * hpcoef) >> 15;
        y = clamp(c->lp - c->hp, -32767, 32767);
        c->delay[w] = (int16_t)y;
        c->wr = (uint16_t)((w + 1u) & KE_MASK);
        if (ens) {
            uint32_t k;
            for (k = 0; k < 3; k++) {
                /* the host renders CTL = 32 samples a block: a shift, not a divide a sample */
                int32_t dq = (int32_t)d0[k] + (((int32_t)d1[k] - (int32_t)d0[k]) * (int32_t)(i + 1u) >> 5);
                wet += ke_tap(c, w, (uint32_t)dq);
            }
            wet = (wet * 10923) >> 15;                          /* / 3 */
            y = (y * (127 - ens) + wet * ens) * 258 >> 15;      /* / 127 (258 / 32768: within 0.1 %) */
        }
        out[i] = y;
    }
    c->slow += si; c->fast += fi;
}
/* floor(a 2^32 / d) for a < d < 2^24, in 32-bit steps (no 64-bit divide): a byte of the quotient at a time */
static uint32_t frac32(uint32_t a, uint32_t d)
{
    uint32_t q = 0, r = a % d, k;
    for (k = 0; k < 4; k++) { const uint32_t x = r << 8; q = (q << 8) | (x / d); r = x % d; }
    return q;
}
/* once a block on the sum of the part's voices (also with none: the chorus keeps its phase, its delay rings out) */
static uint32_t ens_post(track_t *t, int32_t *out, uint32_t n)
{
    Chorus *c = part_mem ? (Chorus *)part_mem(t) : 0;
    const int16_t *p = t->p;
    const int32_t tone = clamp(p[P_E5], 0, 127), hpf = clamp(p[P_E11], 0, 127);
    const uint32_t rate = (uint32_t)clamp(p[P_E7], 0, 127);
    uint32_t i;
    if (!c) return 0;
    if (!c->ready) {   /* (the host zeroes the memory when the track turns to this engine) */
        c->ready = 1; c->slow = 0x20000000u; c->fast = 0x80000000u;
    }
    c->lpcoef = 512 + 2 * tone * tone; c->hpcoef = 12 + 18 * hpf;
    c->ens = clamp(p[P_E6], 0, 127); c->depth = clamp(p[P_E8], 0, 127) * 70 / 127;
    c->si = frac32((35u + rate) * n, 100u * FS);          /* 0.35 .. 1.62 Hz */
    c->fi = frac32((400u + 3u * rate) * n, 100u * FS);    /* 4 .. 7.8 Hz */
    ke_run(c, out, n);
    for (i = 0; i < n; i++) out[i] = mulsh_32(out[i], 30000u, 12);   /* its loudness: ~-16 dB RMS for a chord at its defaults, as RETRO */
    return 1;   /* (always: the chorus runs on, as a string machine's does, so its phase is the same whenever a note comes) */
}

static const engine_t ENS_DSP = {
    .name = "Ensemble",
    .page_title = {"STRINGS", "ENSEMBLE"},
    .note_on = ens_note_on,
    .render = ens_render,
    .knob = {P_E3, P_E5, P_E6, P_E8},
    .keep = 0x01,
    .mem = sizeof(Chorus),
    .post = ens_post,
};

/* ------------------------------------------------------------------------------------------------------- what it shows --- */
static const KEngine ENS_DESC = {
    .name = "Ensemble",
    .uid = K_UID('k', 'e', 'n', 's'),
    .page_title = {"", "STRINGS", "ENSEMBLE"},
    .edit = {
        {"BASS", KF_PCT, 0, 127, 38, 0, 0},
        {"CELLO", KF_PCT, 0, 127, 70, 0, 0},
        {"VIOLA", KF_PCT, 0, 127, 95, 0, 0},
        {"VLN", KF_PCT, 0, 127, 80, 0, 0},
        {"BRASS", KF_PCT, 0, 127, 0, 0, 0},
        {"TONE", KF_CUTOFF, 0, 127, 90, 0, 0},
        {"ENS", KF_PCT, 0, 127, 100, 0, 0},
        {"RATE", KF_PCT, 0, 127, 48, 0, 0},
        {"DEP", KF_PCT, 0, 127, 75, 0, 0},
        {"DTN", KF_INT, 0, 50, 5, 0, "ct"},
        {"LEVEL", KF_PCT, 0, 127, 100, 0, 0},
        {"HPF", KF_PCT, 0, 127, 5, 0, 0},
        {"DRV", KF_PCT, 0, 127, 0, 0, 0},
        {"NOIS", KF_PCT, 0, 127, 0, 0, 0},
        {"ENV", KF_LINK_FENV, 0, 100, 50, 0, 0},
        {"VEL", KF_PCT, 0, 127, 0, 0, 0},
    },
    .macro = {3, 5, 6, 8},   /* HOME's cards: VLN TONE ENS DEP */
    .nparams = 16,
    .page2 = 11,             /* BASS .. LEVEL on SOUND; HPF DRV NOIS ENV VEL on EDIT */
};

/* its factory presets (interface 1.5), as they were in the host's table */
static const KPreset PRESETS[] = {
    {"Strings", "A=30 D=50 S=90 R=50"},
    {"Cellos", "BASS=60 CELLO=110 VIOLA=40 VLN=0 TONE=70 A=30 D=50 S=90 R=50"},
    {"Brass Str", "BRASS=90 VLN=60 TONE=100 A=15 D=50 S=80 R=35"},
    {"Bright", "BASS=0 CELLO=30 VIOLA=90 VLN=120 TONE=120 HPF=30 A=25 D=50 S=90 R=45"},
};

/* The screens. Picture 0: the registers, a string machine's tabs (16' 8' 8' 4' and the brass), each as tall as its value, the
 * brass's in the line's colour. Picture 1: the ensemble, its three taps' sweeps a third apart, as deep as DEP, as fast as RATE, bright as
 * far as ENS mixes them in. Picture 2 (EDIT): the tone section, HPF's corner and TONE's, and the drive's knee. */
#define KN(k, x, y, w, h) {K_W_KNOB, 0, k, x, y, w, h, 0}
#define PIC(id, x, y, w, h) {K_W_PICTURE, K_ST_FLAT | K_ST_FRAME, id, x, y, w, h, 0}
/* the FM-1's 240 x 240: the two pictures over the three rows of knobs (K1 .. K4 a row) */
static const KWidget SOUND_SQ[] = {
    PIC(0, 0, 0, 2, 2), PIC(1, 2, 0, 2, 2),
    KN(0, 0, 2, 1, 1), KN(1, 1, 2, 1, 1), KN(2, 2, 2, 1, 1), KN(3, 3, 2, 1, 1),
    KN(4, 0, 3, 1, 1), KN(5, 1, 3, 1, 1), KN(6, 2, 3, 1, 1), KN(7, 3, 3, 1, 1),
    KN(8, 0, 4, 1, 1), KN(9, 1, 4, 1, 1), KN(10, 2, 4, 1, 1),
};
static const KWidget EDIT_SQ[] = {
    PIC(2, 0, 0, 4, 2),
    KN(11, 0, 2, 1, 1), KN(12, 1, 2, 1, 1), KN(13, 2, 2, 1, 1), KN(14, 3, 2, 1, 1),
    KN(15, 0, 3, 1, 1),
};
/* a wide screen (the Zero, a handheld, a desktop, the web): the pictures one over the other at the left, two rows of knobs */
static const KWidget SOUND_W[] = {
    PIC(0, 0, 0, 2, 1), PIC(1, 0, 1, 2, 1),
    KN(0, 2, 0, 1, 1), KN(1, 3, 0, 1, 1), KN(2, 4, 0, 1, 1), KN(3, 5, 0, 1, 1), KN(4, 6, 0, 1, 1), KN(5, 7, 0, 1, 1),
    KN(6, 2, 1, 1, 1), KN(7, 3, 1, 1, 1), KN(8, 4, 1, 1, 1), KN(9, 5, 1, 1, 1), KN(10, 6, 1, 1, 1),
};
static const KWidget EDIT_W[] = {
    PIC(2, 0, 0, 2, 2),
    KN(11, 2, 0, 1, 1), KN(12, 3, 0, 1, 1), KN(13, 4, 0, 1, 1), KN(14, 5, 0, 1, 1), KN(15, 6, 0, 1, 1),
};
static const KLayout LAYOUTS[] = {
    {K_SCR_SQUARE, K_LPAGE_SOUND, 4, 5, (uint8_t)(sizeof SOUND_SQ / sizeof SOUND_SQ[0]), SOUND_SQ},
    {K_SCR_SQUARE, K_LPAGE_EDIT, 4, 4, (uint8_t)(sizeof EDIT_SQ / sizeof EDIT_SQ[0]), EDIT_SQ},
    {K_SCR_ANY, K_LPAGE_SOUND, 8, 2, (uint8_t)(sizeof SOUND_W / sizeof SOUND_W[0]), SOUND_W},
    {K_SCR_ANY, K_LPAGE_EDIT, 8, 2, (uint8_t)(sizeof EDIT_W / sizeof EDIT_W[0]), EDIT_W},
};

static int val(const int16_t *v, int n, int k, int def) { return k < n ? clamp(v[k], 0, 127) : def; }
static void ens_draw(const KDraw *g, int id, const int16_t *v, int n)
{
    const int col = g->lit ? K_COL_ACCENT : K_COL_MID, sq = g->screen == K_SCR_SQUARE || (host && host->screen == K_SCR_SQUARE);
    int k, x, y;
    if (g->w < 16 || g->h < 16) return;
    if (id == 0) {   /* the registers */
        static const char *const TAB[5] = {"16'", "8'", "8'", "4'", "Br"}, *const SHORT[5] = {"16", "8", "8", "4", "B"};
        const int base = g->h - g->text_h - 3, top = 3, tw = (g->w - 6) / 5;
        for (k = 0; k < 5; k++) {
            const int lv = val(v, n, k, 0), bx = 3 + k * tw, bw = tw - 4, bh = (base - top) * lv / 127;
            g->fill(g, bx, top, bw, base - top, K_COL_FAINT);
            if (bh > 0) g->fill(g, bx, base - bh, bw, bh, k == 4 ? K_COL_LINE : col);
            const char *tab = g->text_w(g, TAB[0]) <= tw - 3 ? TAB[k] : SHORT[k];   /* (a narrow frame: the feet without their marks) */
            const int tx = bx + (bw - g->text_w(g, tab)) / 2;
            if (g->text_w(g, SHORT[0]) < tw - 3) g->text(g, tx < 0 ? 0 : tx, base + 2, tab, K_COL_TEXT);   /* (none where they would touch) */
        }
        return;
    }
    if (id == 1) {   /* the ensemble: three sweeps a third apart */
        const int ens = val(v, n, 6, 100), rate = val(v, n, 7, 48), dep = val(v, n, 8, 75);
        const int mid = g->h / 2, amp = (g->h / 2 - 4) * dep / 127, cycles = 1 + rate / 48, step = sq ? 4 : 2;
        g->line(g, 2, mid, g->w - 3, mid, K_COL_FAINT);
        for (k = 0; k < 3; k++) {
            const int c = ens > 64 || k == 0 ? (k == 0 ? col : K_COL_LINE) : K_COL_FAINT;
            int px = -1, py = 0;
            for (x = 2; x < g->w - 2; x += step) {
                const uint32_t ph = (uint32_t)((x - 2) * cycles) * (0xFFFFFFFFu / (uint32_t)(g->w - 4)) + (uint32_t)k * 0x55555555u;
                y = mid - ((sine_i(ph) >> 8) * amp >> 7);
                if (px >= 0) g->line(g, px, py, x, y, c);
                px = x; py = y;
            }
        }
        if (!ens) g->text(g, 3, 2, "Off", K_COL_MID);
        return;
    }
    if (id == 2) {   /* the tone: the high-pass's corner rising, the low-pass falling; the drive's knee as a step */
        const int hpf = val(v, n, 11, 5), tone = val(v, n, 5, 90), drv = val(v, n, 12, 0);
        const int lo = 2 + (g->w - 4) * hpf / 254, hi = 2 + (g->w - 4) * (64 + tone) / 191, top = 4 + g->h / 6, bot = g->h - 4;
        g->line(g, 2, bot, lo, top, col);
        g->line(g, lo, top, hi, top, col);
        g->line(g, hi, top, hi + (g->w - hi) / 2, bot, col);
        if (drv) g->fill(g, 2, 2, (g->w - 4) * drv / 127, 2, K_COL_LINE);
        g->text(g, 3, bot - g->text_h, "HP", K_COL_MID);
        g->text(g, g->w - 3 - g->text_w(g, "Tone"), 2 + 4, "Tone", K_COL_MID);
    }
}

static const KPluginExt ENS_EXT = {sizeof(KPluginExt), K_PLUGIN_EXT_VERSION, ens_attach, LAYOUTS,
                                   (uint8_t)(sizeof LAYOUTS / sizeof LAYOUTS[0]), ens_draw, PRESETS,
                                   (uint8_t)(sizeof PRESETS / sizeof PRESETS[0])};
static const KPlugin ENSEMBLE = {sizeof(KPlugin), K_PLUGIN_ABI, 1, K_PLUG_SYNTH, &ENS_DESC, &ENS_DSP,
                                 FS, 0, 15, 0, 0, &ENS_EXT};   /* its own state is the part's memory (engine_t.mem); ~15 us a voice */

KREISEL_PLUGIN_EXPORT const KPlugin *kreisel_plugin(void) { return &ENSEMBLE; }
