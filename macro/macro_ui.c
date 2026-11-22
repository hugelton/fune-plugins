/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * The full MIT notice: plugins/macro/LICENSE-MIT. (Fune's own: the screens; nothing here is from Plaits.) */
/* Macro's screens (docs/PLUGIN_SDK.md: Screens), included by macro.c. SOUND: the model's name and its small picture, the voice's
 * waveform in a frame, then MODL HARM TIMB MORF. EDIT: the gate's picture (how the note falls with DCAY, how the colour closes
 * with COLR, TENV's sweep), then DCAY COLR TENV LVL. Pictures: 0 the model, 1 the waveform, 2 the gate. Integers only. */

#define KN(k, x, y, w, h) {K_W_KNOB, 0, k, x, y, w, h, 0}
#define PIC(id, x, y, w, h) {K_W_PICTURE, K_ST_FLAT | K_ST_FRAME, id, x, y, w, h, 0}
static const KWidget SOUND_W[] = {   /* a square screen (the FM-1) and a large one: the pictures over the knobs */
    PIC(0, 0, 0, 2, 2), PIC(1, 2, 0, 2, 2),
    KN(E_MODL, 0, 2, 1, 3), KN(E_HARM, 1, 2, 1, 3), KN(E_TIMB, 2, 2, 1, 3), KN(E_MORF, 3, 2, 1, 3),
};
static const KWidget EDIT_W[] = {
    PIC(2, 0, 0, 4, 2),
    KN(E_DCAY, 0, 2, 1, 3), KN(E_COLR, 1, 2, 1, 3), KN(E_TENV, 2, 2, 1, 3), KN(E_LVL, 3, 2, 1, 3),
};
static const KWidget SOUND_S[] = {   /* a small wide screen (the Zero): the pictures beside the knobs */
    PIC(0, 0, 0, 2, 2), PIC(1, 2, 0, 2, 2),
    KN(E_MODL, 4, 0, 1, 2), KN(E_HARM, 5, 0, 1, 2), KN(E_TIMB, 6, 0, 1, 2), KN(E_MORF, 7, 0, 1, 2),
};
static const KWidget EDIT_S[] = {
    PIC(2, 0, 0, 4, 2),
    KN(E_DCAY, 4, 0, 1, 2), KN(E_COLR, 5, 0, 1, 2), KN(E_TENV, 6, 0, 1, 2), KN(E_LVL, 7, 0, 1, 2),
};
static const KLayout MC_LAYOUTS[] = {
    {K_SCR_SMALL, K_LPAGE_SOUND, 8, 2, 6, SOUND_S},
    {K_SCR_SMALL, K_LPAGE_EDIT, 8, 2, 5, EDIT_S},
    {K_SCR_ANY, K_LPAGE_SOUND, 4, 5, 6, SOUND_W},
    {K_SCR_ANY, K_LPAGE_EDIT, 4, 5, 5, EDIT_W},
};

static const KHostApi *mc_host;
static void mc_attach(const KHostApi *h)
{   /* the device, and the part's memory (interface 1.5; an older host has none: one part's state for all) */
    mc_host = h;
    mc_part_mem = h && h->size >= offsetof(KHostApi, part_mem) + sizeof h->part_mem ? h->part_mem : 0;
}

/* a picture's points: x, y in 1/16 of a 16 x 16 box, -1 lifts the pen (each model's own drawing, Fune's) */
static const int8_t ICON_PAIR[] = {0, 12, 4, 4, 4, 12, 8, 4, 8, 12, -1, -1, 8, 12, 8, 6, 12, 6, 12, 12, 16, 12, 16, 6};
static const int8_t ICON_FOLD[] = {0, 8, 2, 4, 4, 2, 5, 5, 6, 2, 8, 4, 10, 8, 12, 12, 13, 10, 14, 13, 15, 11, 16, 8};
static const int8_t ICON_FM[] = {1, 6, 5, 6, 5, 10, 1, 10, 1, 6, -1, -1, 5, 8, 10, 8, -1, -1, 8, 6, 10, 8, 8, 10, -1, -1,
                                 10, 5, 15, 5, 15, 11, 10, 11, 10, 5, -1, -1, 12, 3, 13, 1, 14, 3};
static const int8_t ICON_GRAIN[] = {0, 12, 1, 9, 2, 12, 3, 15, 4, 12, -1, -1, 5, 12, 6, 7, 7, 12, 8, 15, 9, 12, -1, -1,
                                    10, 12, 11, 9, 12, 12, 13, 14, 14, 12, -1, -1, 0, 12, 16, 12};
static const int8_t ICON_HARM[] = {1, 14, 1, 2, -1, -1, 4, 14, 4, 6, -1, -1, 7, 14, 7, 9, -1, -1, 10, 14, 10, 11, -1, -1,
                                   13, 14, 13, 12, -1, -1, 15, 14, 15, 13};
static const int8_t ICON_WAVE[] = {0, 10, 3, 4, 6, 10, 9, 4, 12, 10, -1, -1, 3, 7, 5, 2, 8, 7, 11, 2, 14, 7, -1, -1,
                                   6, 4, 8, 0, 11, 4, 14, 0, 16, 4};
static const int8_t ICON_CHORD[] = {0, 4, 16, 4, -1, -1, 0, 8, 16, 8, -1, -1, 0, 12, 16, 12, -1, -1, 5, 13, 7, 11, 9, 13,
                                    7, 15, 5, 13, -1, -1, 5, 9, 7, 7, 9, 9, 7, 11, 5, 9, -1, -1, 5, 5, 7, 3, 9, 5, 7, 7, 5, 5};
static const int8_t ICON_NOISE[] = {0, 8, 1, 3, 2, 11, 3, 6, 4, 14, 5, 2, 6, 9, 7, 5, 8, 12, 9, 1, 10, 10, 11, 6,
                                    12, 13, 13, 4, 14, 9, 15, 7, 16, 8};
static const struct { const int8_t *p; uint8_t n; } ICONS[M_COUNT] = {
    {ICON_PAIR, sizeof ICON_PAIR}, {ICON_FOLD, sizeof ICON_FOLD}, {ICON_FM, sizeof ICON_FM}, {ICON_GRAIN, sizeof ICON_GRAIN},
    {ICON_HARM, sizeof ICON_HARM}, {ICON_WAVE, sizeof ICON_WAVE}, {ICON_CHORD, sizeof ICON_CHORD}, {ICON_NOISE, sizeof ICON_NOISE},
};

static void mc_icon(const KDraw *g, int model, int x0, int y0, int size, int col)
{
    const int8_t *p = ICONS[model].p;
    int i, px = -1, py = 0;
    for (i = 0; i + 1 < ICONS[model].n; i += 2) {
        int x, y;
        if (p[i] < 0) { px = -1; continue; }
        x = x0 + p[i] * size / 16; y = y0 + p[i + 1] * size / 16;
        if (px >= 0) g->line(g, px, py, x, y, col);
        px = x; py = y;
    }
}

static void mc_draw(const KDraw *g, int id, const int16_t *val, int n)
{
    const int square = (mc_host && mc_host->screen == K_SCR_SQUARE) || g->screen == K_SCR_SQUARE;
    const int col = g->lit ? K_COL_ACCENT : K_COL_MID;
    if (g->w < 12 || g->h < 12) return;
    if (id == 0) {   /* the model: its picture, its name under it */
        const int model = n > 0 ? clamp(val[E_MODL], 0, M_COUNT - 1) : 0;
        const char *name = MODELS[model];
        int size = g->h - g->text_h - 6, x;
        if (size > g->w - 8) size = g->w - 8;
        if (size < 8) size = 8;
        mc_icon(g, model, (g->w - size) / 2, 2, size, col);
        x = (g->w - g->text_w(g, name)) / 2;
        g->text(g, x < 0 ? 0 : x, g->h - g->text_h - 1, name, K_COL_TEXT);
    } else if (id == 1) {   /* the voice's waveform (the track's sound, as a scope shows it) */
        int8_t pts[160];
        const int want = g->w < 160 ? g->w : 160;
        const int got = g->scope(g, pts, want), mid = g->h / 2, step = square ? 2 : 1;
        int i, px = -1, py = 0, top = 16;
        g->line(g, 0, mid, g->w - 1, mid, K_COL_FAINT);
        for (i = 0; i < got; i++) top = pts[i] > top ? pts[i] : -pts[i] > top ? -pts[i] : top;   /* (the frame fits the wave) */
        for (i = 0; i < got; i += step) {
            const int x = got > 1 ? i * (g->w - 1) / (got - 1) : 0;
            int y = mid - pts[i] * (g->h / 2 - 2) / top;
            y = clamp(y, 0, g->h - 1);
            if (px >= 0) g->line(g, px, py, x, y, col);
            px = x; py = y;
        }
    } else if (id == 2) {   /* the gate: the level's fall (DCAY) and, dimmer, the colour's (COLR); TENV's sweep as an arrow */
        const int dcay = n > E_DCAY ? clamp(val[E_DCAY], 0, 127) : 70, colr = n > E_COLR ? clamp(val[E_COLR], 0, 127) : 64;
        const int tenv = n > E_TENV ? clamp(val[E_TENV], -64, 63) : 0;
        const int step = square ? 3 : 2, top = 3, bot = g->h - 3;
        /* the time the level takes to fall to a tenth, in the picture's width: 2^(dcay / 16) of a sixteenth of it */
        const int span = dcay >= 127 ? 0x7FFF : (g->w * (1 + ((dcay * dcay) >> 6))) / 32;
        int x, px = -1, py = 0, qx = -1, qy = 0;
        g->line(g, 0, bot, g->w - 1, bot, K_COL_FAINT);
        for (x = 0; x < g->w; x += step) {
            int lv, cl, y;
            if (dcay >= 127) lv = 256;                               /* open: held while the key is */
            else {
                const int k = span > 0 ? x * 256 / span : 1024;     /* a power of ten a span: 256 at 0 */
                lv = k >= 1024 ? 0 : 256 - (k * 256 / (k + 64)) * (1024 - k) / 1024 - (k > 256 ? 0 : 0);
                lv = clamp(lv, 0, 256);
            }
            y = bot - lv * (bot - top) / 256;
            if (px >= 0) g->line(g, px, py, x, y, col);
            px = x; py = y;
            cl = (lv * lv / 256) * (128 + colr) / 255;               /* the brightness: closes sooner than the level, the colour keeps it */
            cl = clamp(cl + colr * (256 - cl) / 512, 0, 256);
            y = bot - cl * (bot - top) / 256;
            if (qx >= 0 && (x / step) % 2 == 0) g->line(g, qx, qy, x, y, K_COL_FAINT);
            qx = x; qy = y;
        }
        if (tenv) {   /* TENV: an arrow at the left, up or down, as long as the amount */
            const int len = mc_abs(tenv) * (g->h - 8) / 64, y0 = g->h / 2 + (tenv > 0 ? len / 2 : -len / 2), y1 = y0 - (tenv > 0 ? len : -len);
            g->line(g, 4, y0, 4, y1, K_COL_TEXT);
            g->line(g, 2, y1 + (tenv > 0 ? 2 : -2), 4, y1, K_COL_TEXT);
            g->line(g, 6, y1 + (tenv > 0 ? 2 : -2), 4, y1, K_COL_TEXT);
        }
    }
}

static const KPluginExt MC_EXT = {sizeof(KPluginExt), K_PLUGIN_EXT_VERSION, mc_attach, MC_LAYOUTS,
                                  (uint8_t)(sizeof MC_LAYOUTS / sizeof MC_LAYOUTS[0]), mc_draw,
                                  MC_PRESETS, (uint8_t)(sizeof MC_PRESETS / sizeof MC_PRESETS[0])};
