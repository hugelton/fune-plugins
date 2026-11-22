/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* VECTOR, an outside plugin (id `kvec`): four selectable band-limited wavetables at the corners of a square, mixed by an X / Y
 * position (bilinear), which a slow circle (MOVE, RATE) can move. Built into Kreisel until 2026-11-22 (src/eng_kvector.c, switched
 * off since 2026-11-06); the same sound bit for bit (tests/ens_vec_plugin_test.c).
 *
 * One morphed table a part, three harmonic tiers (3 x 128 x 2 = 768 bytes), kept in the part's memory the host gives
 * (KHostApi.part_mem, interface 1.5); remixed in block() only when the position or the waves change (at most once every eight
 * blocks while it moves). Each voice reads it with two phases, SPRD apart. Uses the host's ADSR, LFO and voice allocator; no
 * filter. No allocation, no floating point, no 64-bit product, no divide in a sample loop.
 *
 * SOUND  0 X 1 Y 2 MOVE 3 SPRD 4 RATE 5 LEVEL
 * EDIT   6 A 7 B 8 C 9 D: the corners' waves (top left, top right, bottom left, bottom right) */
#include "kreisel_sdk.h"
#include "vector_tables.h"

static const KHostApi *host;
static void *(*part_mem)(const void *part);   /* the host's part memory (0: an older host; then it is silent) */
static void vec_attach(const KHostApi *h)
{
    host = h;
    part_mem = h && h->size >= offsetof(KHostApi, part_mem) + sizeof h->part_mem ? h->part_mem : 0;
}

typedef struct {
    int16_t morphed[KV_MIPS][KV_LEN];
    uint32_t phase;
    uint8_t x, y, wave[4], valid;
    uint8_t base_x, base_y, last_motion, last_rate, ticks;
} Morph;   /* 784 bytes, in the part's memory (K_PART_MEM: 2048) */
_Static_assert(sizeof(Morph) <= K_PART_MEM, "the morphed table fits a part's memory");

static void vec_block(track_t *t)
{
    Morph *q = part_mem ? (Morph *)part_mem(t) : 0;
    const int16_t *p = t->p;
    int32_t x = clamp(p[P_E0], 0, 127), y = clamp(p[P_E1], 0, 127);
    int32_t motion = clamp(p[P_E2], 0, 127), rate = clamp(p[P_E4], 0, 127);
    unsigned w[4], m, i, k;
    int32_t weight[4];
    if (!q) return;
    q->phase += (uint32_t)rate * 49000u;  /* up to ~2 Hz at 44.1 kHz, 32 samples a block */
    if (motion && rate) {
        x = clamp(x + ((sine_i(q->phase) * motion) >> 16), 0, 127);
        y = clamp(y + ((sine_i(q->phase + 0x40000000u) * motion) >> 16), 0, 127);
    }
    for (i = 0; i < 4; i++) w[i] = (unsigned)clamp(p[P_E6 + i], 0, KV_WAVES - 1);
    {
        const int manual_x = clamp(p[P_E0], 0, 127), manual_y = clamp(p[P_E1], 0, 127);
        const int unchanged = q->valid && q->base_x == manual_x && q->base_y == manual_y &&
            q->last_motion == motion && q->last_rate == rate &&
            q->wave[0] == w[0] && q->wave[1] == w[1] && q->wave[2] == w[2] && q->wave[3] == w[3];
        /* the motion remixes at most once every eight blocks (~172 a second): the phase still moves each block, a hand on X or Y
         * is heard at once */
        if (unchanged && motion && rate && ++q->ticks < 8u) return;
        q->ticks = 0;
        if (unchanged && q->x == x && q->y == y) return;
        q->base_x = (uint8_t)manual_x;
        q->base_y = (uint8_t)manual_y;
        q->last_motion = (uint8_t)motion;
        q->last_rate = (uint8_t)rate;
    }
    /* the bilinear weights: positive, summing to 127^2 = 16129 (near Q14) */
    weight[0] = (127 - x) * (127 - y);  /* top left, A */
    weight[1] = x * (127 - y);          /* top right, B */
    weight[2] = (127 - x) * y;          /* bottom left, C */
    weight[3] = x * y;                  /* bottom right, D */
    for (m = 0; m < KV_MIPS; m++) for (i = 0; i < KV_LEN; i++) {
        int32_t sum = 0;
        for (k = 0; k < 4; k++) sum += ((int32_t)KV_TABLE[m][w[k]][i] * weight[k]) >> 14;
        q->morphed[m][i] = (int16_t)clamp(sum, -30000, 30000);
    }
    q->x = (uint8_t)x; q->y = (uint8_t)y;
    for (i = 0; i < 4; i++) q->wave[i] = (uint8_t)w[i];
    q->valid = 1;
}

static inline int32_t vec_lookup(const int16_t *tab, uint32_t ph)
{
    uint32_t i = ph >> 25, f = (ph >> 17) & 255u;
    int32_t a = tab[i], b = tab[(i + 1u) & 127u];
    return a + (((b - a) * (int32_t)f) >> 8);
}
static void vec_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const Morph *q = part_mem ? (const Morph *)part_mem(t) : 0;
    const int16_t *p = t->p;
    const unsigned mip = m->pitch16 <= 82 * 16 ? 0u : m->pitch16 <= 100 * 16 ? 1u : 2u;   /* (MIDI 82, 100: the tiers' tops) */
    const int16_t *tab;
    const int32_t lvl = clamp(p[P_E5], 0, 127) * 258;
    uint32_t p0 = v->ph[0], p1 = v->ph[1], inc = m->inc, i;
    /* two phases on one table, up to +-3.4 cents apart */
    const uint32_t det = ((inc >> 12) * (uint32_t)clamp(p[P_E3], 0, 127)) >> 4;
    const uint32_t inc0 = inc - det, inc1 = inc + det;
    if (!q) return;
    tab = q->morphed[mip];
    for (i = 0; i < n; i++) {
        const int32_t a = vec_lookup(tab, p0), b = vec_lookup(tab, p1);
        out[i] += voice_amp(mulq15((a + b) >> 1, lvl), m, i);
        p0 += inc0;
        p1 += inc1;
    }
    v->ph[0] = p0; v->ph[1] = p1;
}

static const engine_t VEC_DSP = {
    .name = "Vector",
    .page_title = {"MORPH", "WAVES"},
    .block = vec_block,
    .render = vec_render,
    .knob = {P_E0, P_E1, P_E2, P_E3},
    .mem = sizeof(Morph),
};

/* ------------------------------------------------------------------------------------------------------- what it shows --- */
static const char *const WAVES[] = {"SINE", "SAW", "PULSE", "TRI", "VOX", "GLASS", "STRING", "BELL", 0};
static const KEngine VEC_DESC = {
    .name = "Vector",
    .uid = K_UID('k', 'v', 'e', 'c'),
    .page_title = {"", "MORPH", "WAVES"},
    .edit = {
        {"X", KF_PCT, 0, 127, 64, 0, 0},
        {"Y", KF_PCT, 0, 127, 64, 0, 0},
        {"MOVE", KF_PCT, 0, 127, 0, 0, 0},
        {"SPRD", KF_PCT, 0, 127, 22, 0, 0},
        {"RATE", KF_PCT, 0, 127, 32, 0, 0},
        {"LEVEL", KF_PCT, 0, 127, 103, 0, 0},
        {"A", KF_ENUM, 0, 7, 0, WAVES, 0},
        {"B", KF_ENUM, 0, 7, 1, WAVES, 0},
        {"C", KF_ENUM, 0, 7, 4, WAVES, 0},
        {"D", KF_ENUM, 0, 7, 5, WAVES, 0},
    },
    .macro = {0, 1, 2, 3},   /* HOME's cards: X Y MOVE SPRD */
    .nparams = 10,
    .page2 = 6,              /* X .. LEVEL on SOUND; the corners' waves on EDIT */
    .nomod = K_NOMOD_FILTER | K_NOMOD_SHAPE,
};

/* The screens. Picture 0: the square, its corners named by their waves, the point at X / Y and the circle MOVE draws round it.
 * Picture 1 (EDIT): the four corners' waves as they are in the table, where they sit on the square. */
#define KN(k, x, y, w, h) {K_W_KNOB, 0, k, x, y, w, h, 0}
#define PIC(id, x, y, w, h) {K_W_PICTURE, K_ST_FLAT | K_ST_FRAME, id, x, y, w, h, 0}
static const KWidget SOUND_SQ[] = {   /* the FM-1: the square over X Y MOVE SPRD (K1 .. K4), RATE LEVEL under them */
    PIC(0, 0, 0, 4, 3),
    KN(0, 0, 3, 1, 1), KN(1, 1, 3, 1, 1), KN(2, 2, 3, 1, 1), KN(3, 3, 3, 1, 1),
    KN(4, 0, 4, 1, 1), KN(5, 1, 4, 1, 1),
};
static const KWidget EDIT_SQ[] = {
    PIC(1, 0, 0, 4, 3),
    KN(6, 0, 3, 1, 1), KN(7, 1, 3, 1, 1), KN(8, 2, 3, 1, 1), KN(9, 3, 3, 1, 1),
};
static const KWidget SOUND_W[] = {    /* a wide screen: the square at the left, the knobs beside it */
    PIC(0, 0, 0, 3, 2),
    KN(0, 3, 0, 1, 1), KN(1, 4, 0, 1, 1), KN(2, 5, 0, 1, 1), KN(3, 6, 0, 1, 1),
    KN(4, 3, 1, 1, 1), KN(5, 4, 1, 1, 1),
};
static const KWidget EDIT_W[] = {
    PIC(1, 0, 0, 3, 2),
    KN(6, 3, 0, 1, 1), KN(7, 4, 0, 1, 1), KN(8, 3, 1, 1, 1), KN(9, 4, 1, 1, 1),
};
static const KLayout LAYOUTS[] = {
    {K_SCR_SQUARE, K_LPAGE_SOUND, 4, 5, (uint8_t)(sizeof SOUND_SQ / sizeof SOUND_SQ[0]), SOUND_SQ},
    {K_SCR_SQUARE, K_LPAGE_EDIT, 4, 4, (uint8_t)(sizeof EDIT_SQ / sizeof EDIT_SQ[0]), EDIT_SQ},
    {K_SCR_ANY, K_LPAGE_SOUND, 7, 2, (uint8_t)(sizeof SOUND_W / sizeof SOUND_W[0]), SOUND_W},
    {K_SCR_ANY, K_LPAGE_EDIT, 5, 2, (uint8_t)(sizeof EDIT_W / sizeof EDIT_W[0]), EDIT_W},
};

static int val(const int16_t *v, int n, int k, int lo, int hi, int def) { return k < n ? clamp(v[k], lo, hi) : def; }
/* a wave of the table (tier 0) in a box, every `step` points */
static void wave_box(const KDraw *g, int bx, int by, int bw, int bh, int w, int col, int step)
{
    int x, px = -1, py = 0;
    for (x = 0; x < bw; x += step) {
        const int y = by + bh / 2 - KV_TABLE[0][w][x * KV_LEN / bw] * (bh / 2 - 1) / 32768;
        if (px >= 0) g->line(g, px, py, bx + x, y, col);
        px = bx + x; py = y;
    }
}
static void vec_draw(const KDraw *g, int id, const int16_t *v, int n)
{
    const int col = g->lit ? K_COL_ACCENT : K_COL_MID, sq = g->screen == K_SCR_SQUARE || (host && host->screen == K_SCR_SQUARE);
    int k;
    if (g->w < 24 || g->h < 24) return;
    if (id == 0) {   /* the square, the point, the circle */
        const int s = (g->h < g->w ? g->h : g->w) - 2 * g->text_h - 6, x0 = (g->w - s) / 2, y0 = g->text_h + 3;
        const int px = x0 + s * val(v, n, 0, 0, 127, 64) / 127, py = y0 + s * val(v, n, 1, 0, 127, 64) / 127;
        const int r = s * val(v, n, 2, 0, 127, 0) / 254, rate = val(v, n, 4, 0, 127, 32);
        g->line(g, x0, y0, x0 + s, y0, K_COL_LINE); g->line(g, x0, y0 + s, x0 + s, y0 + s, K_COL_LINE);
        g->line(g, x0, y0, x0, y0 + s, K_COL_LINE); g->line(g, x0 + s, y0, x0 + s, y0 + s, K_COL_LINE);
        g->line(g, x0 + s / 2, y0 + 2, x0 + s / 2, y0 + s - 2, K_COL_FAINT); g->line(g, x0 + 2, y0 + s / 2, x0 + s - 2, y0 + s / 2, K_COL_FAINT);
        for (k = 0; k < 4; k++) {   /* the corners' waves, A B over the square, C D under it */
            const char *nm = WAVES[val(v, n, 6 + k, 0, 7, k)];
            const int tw = g->text_w(g, nm), ty = k < 2 ? 1 : y0 + s + 2;
            g->text(g, k & 1 ? x0 + s - tw : x0, ty, nm, K_COL_TEXT);
        }
        if (r > 1 && rate) {   /* the motion's circle (clipped to the square, as the sound is) */
            const int steps = sq ? 16 : 32;
            int ox = -1, oy = 0;
            for (k = 0; k <= steps; k++) {
                const uint32_t ph = (uint32_t)k * (0xFFFFFFFFu / (uint32_t)steps);
                const int cx = clamp(px + (sine_i(ph) >> 8) * r / 128, x0, x0 + s), cy = clamp(py + (sine_i(ph + 0x40000000u) >> 8) * r / 128, y0, y0 + s);
                if (ox >= 0) g->line(g, ox, oy, cx, cy, K_COL_LINE);
                ox = cx; oy = cy;
            }
        }
        g->fill(g, px - 3, py - 3, 7, 7, col);
        g->fill(g, px - 1, py - 1, 3, 3, K_COL_BG);
        return;
    }
    if (id == 1) {   /* the four waves, where they sit */
        const int bw = (g->w - 9) / 2, bh = (g->h - 9) / 2, step = sq ? 3 : 2;
        for (k = 0; k < 4; k++) {
            const int w = val(v, n, 6 + k, 0, 7, k), bx = 3 + (k & 1) * (bw + 3), by = 3 + (k >> 1) * (bh + 3);
            static const char *const C[4] = {"A", "B", "C", "D"};
            g->fill(g, bx, by, bw, bh, K_COL_FAINT);
            wave_box(g, bx + 2, by + g->text_h / 2, bw - 4, bh - g->text_h / 2 - 2, w, col, step);
            g->text(g, bx + 2, by + 1, C[k], K_COL_TEXT);
            g->text(g, bx + bw - 2 - g->text_w(g, WAVES[w]), by + 1, WAVES[w], K_COL_MID);
        }
    }
}

static const KPluginExt VEC_EXT = {sizeof(KPluginExt), K_PLUGIN_EXT_VERSION, vec_attach, LAYOUTS,
                                   (uint8_t)(sizeof LAYOUTS / sizeof LAYOUTS[0]), vec_draw, 0, 0};
static const KPlugin VECTOR = {sizeof(KPlugin), K_PLUGIN_ABI, 1, K_PLUG_SYNTH, &VEC_DESC, &VEC_DSP,
                               FS, 0, 8, 0, 0, &VEC_EXT};   /* its own state is the part's memory (engine_t.mem); ~8 us a voice */

KREISEL_PLUGIN_EXPORT const KPlugin *kreisel_plugin(void) { return &VECTOR; }
