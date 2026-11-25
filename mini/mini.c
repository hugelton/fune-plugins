/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 *
 * MINI, based on CA-72 by Idle Foundry (https://github.com/idlefoundry/ca-72), copyright (C) 2026 Idle Foundry Ltd.
 * This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. It is
 * distributed WITHOUT ANY WARRANTY; see the GNU General Public License (plugins/mini/LICENSE) for more details.
 * (Fune's core is MPL-2.0; this folder, MINI, is GPL-3.0-or-later as a whole and nothing of it is in the core.) */
/* MINI, an outside engine plugin (id `mini`): a monophonic three-oscillator synthesizer of the early 1970s, drawn as its panel
 * (wooden end cheeks, a dark panel, black knobs with white skirts).
 *
 * Its sound follows CA-72's light voice (ca72/src/light.rs: "Potato mode"), ported to C integers (mini_int.h): the same code on
 * every device, the FM-1 included. Its laws are tables made from CA-72's own functions (tools/mini_gen, mini_int_tables.h).
 * The host is polyphonic: every key is a host voice, MINI's instrument (one a part) takes their keys (last-note priority, single
 * triggering, glide) and one of them, the render voice, plays it; the others are silent. The loudness is the instrument's own VCA:
 * the render voice is kept alive while it sounds.
 * Parameters (P_E0 ..):
 *   SOUND  0 RNG1  1 WAV1  2 RNG2  3 WAV2 | 4 FRQ2  5 RNG3  6 WAV3  7 FRQ3 | 8 OSC2  9 OSC3  10 NOIS  11 GLID
 *   EDIT   12 CUT  13 EMPH  14 AMT  15 KTRK | 16 DCY  17 SUS  18 MOD  19 MODW
 * Folded into 20: oscillator 1's VOLUME fixed (8; WAV1 Off), the loudness contour's knobs are the track's ENV 1 (A D S; R above 0
 * is the DECAY switch), the filter contour's ATTACK is ENV 1's A, OSC. 3 CONTROL is RNG3's LFO, the noise white, MODULATION MIX
 * and its two switches one list (MOD), the MODULATION wheel a value (MODW). */
#include "kreisel_sdk.h"
#include <stddef.h>
#include "mini_knobs.h"

/* ------------------------------------------------------------------ the instrument: one a part --- */
#define MINI_ONE 8192  /* the instrument's output, 1.0 = 5 V (Q13) */

#include "mini_int.h"
typedef MiniInt Eng;   /* (in the part's memory: MiniPart.eng) */
#define eng_panel mi_panel
#define eng_note mi_note
#define eng_offsets mi_offsets
#define eng_render mi_render
#define eng_sounding mi_sounding
#define eng_rest mi_rest

/* A part's state, in the part's memory (interface 1.5: zeroed when the track turns to MINI): its keys as host voices, the voice
 * that plays it, its knobs; the integer path's instrument */
#define MINI_KEYS 8
typedef struct { voice_t *v; uint32_t age; uint8_t note; } Key;
typedef struct {
    voice_t *rv;                   /* the render voice */
    uint32_t rv_age;
    Key keys[MINI_KEYS];
    uint8_t nkeys, ready;
    MiniKnobs k;
    MiniInt eng;
} MiniPart;
_Static_assert(sizeof(MiniPart) <= K_PART_MEM, "MINI's part fits the part's memory");

static void *(*part_mem)(const void *part);
/* the part's state and its instrument; 0 on a host without part memory (then MINI is silent) */
static MiniPart *part_of(const track_t *t, Eng **e)
{
    MiniPart *P = part_mem ? (MiniPart *)part_mem(t) : 0;
    if (!P) return 0;
    *e = &P->eng;
    if (!P->ready) { eng_rest(*e); P->ready = 1; }   /* (fresh: the instrument at rest) */
    return P;
}

/* the track's values as the instrument's knobs */
static uint8_t knob(int v) { return (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v); }
static int isqrt_i(int v) { int r = 0; while ((r + 1) * (r + 1) <= v) r++; return r; }
static void knobs_of(const int16_t *p, MiniKnobs *k)
{
    static const uint8_t W12[7] = {0, 1, 3, 4, 5, 6, 3}, W3[6] = {0, 2, 3, 4, 5, 6};   /* MINI's lists to the CA-72's order */
    static const uint8_t KT[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    const int w1 = clamp(p[P_E1], 0, 6), r3 = clamp(p[P_E5], 0, 6), mod = clamp(p[P_E18], 0, 6), kt = clamp(p[P_E15], 0, 3);
    k->range[0] = (uint8_t)clamp(p[P_E0], 0, 5); k->range[1] = (uint8_t)clamp(p[P_E2], 0, 5); k->range[2] = (uint8_t)(r3 == 6 ? 0 : r3);
    k->wave[0] = W12[w1]; k->wave[1] = W12[clamp(p[P_E3], 0, 5)]; k->wave[2] = W3[clamp(p[P_E6], 0, 5)];
    k->volume[0] = 102; k->volume[1] = knob(p[P_E8]); k->volume[2] = knob(p[P_E9]);
    k->on[0] = w1 != 6; k->on[1] = k->volume[1] != 0; k->on[2] = k->volume[2] != 0;
    k->freq[0] = (int8_t)clamp(p[P_E4], -64, 63); k->freq[1] = (int8_t)clamp(p[P_E7], -64, 63);
    k->osc3_control = r3 != 6;
    k->noise_volume = knob(p[P_E10]); k->noise_on = k->noise_volume != 0;
    k->glide = knob(p[P_E11]);
    k->cutoff = knob(p[P_E12]); k->emphasis = knob(p[P_E13]); k->amount = knob(p[P_E14]);
    k->kbd1 = KT[kt][0]; k->kbd2 = KT[kt][1];
    /* ENV 1 as the loudness contour's knobs: the host keeps A as v^2 127 / 10000 and D, S as v 127 / 100 (v its 0 .. 100) */
    k->ladsr[0] = knob(isqrt_i(clamp(p[P_ATK], 0, 127) * 127)); k->ladsr[1] = knob(p[P_DEC]); k->ladsr[2] = knob(p[P_SUS]);
    k->fadsr[0] = k->ladsr[0]; k->fadsr[1] = knob(p[P_E16]); k->fadsr[2] = knob(p[P_E17]);
    k->decay = p[P_REL] > 0;
    k->mod_mix = (uint8_t)(mod <= 2 ? 0 : mod <= 4 ? 127 : 64);
    k->osc_mod = mod == 0 || mod == 2 || mod == 4 || mod == 6;
    k->filter_mod = mod != 0;
    k->wheel = knob(p[P_E19]);
}

/* ------------------------------------------------------------------ the host's voices --- */
static void key_up(MiniPart *P, Eng *e, int i)
{
    eng_note(e, P->keys[i].note, 0);
    P->keys[i] = P->keys[--P->nkeys];
}
static void mini_note_on(track_t *t, voice_t *v)
{
    Eng *e;
    MiniPart *P = part_of(t, &e);
    int i;
    if (!P) return;
    for (i = 0; i < P->nkeys; i++) if (P->keys[i].v == v) { key_up(P, e, i); break; }   /* (the same voice again: its key let go first) */
    if (P->nkeys >= MINI_KEYS) key_up(P, e, 0);
    P->keys[P->nkeys++] = (Key){v, v->age, v->note};
    eng_note(e, v->note, 1);
}
static void mini_block(track_t *t)
{
    Eng *e;
    MiniPart *P = part_of(t, &e);
    MiniKnobs k;
    int i;
    if (!P) return;
    for (i = P->nkeys - 1; i >= 0; i--)   /* keys whose voice is gone (a steal, an engine's switch): let go */
        if (!P->keys[i].v->active || P->keys[i].v->age != P->keys[i].age) key_up(P, e, i);
    knobs_of(t->p, &k);
    {
        const uint8_t *a = (const uint8_t *)&k, *b = (const uint8_t *)&P->k;
        uint32_t j;
        for (j = 0; j < sizeof k && a[j] == b[j]; j++) {}
        if (j < sizeof k || P->ready == 1) { P->k = k; eng_panel(e, &k); P->ready = 2; }
    }
}
/* the host's ADSR for each voice: the render voice plays at full (the instrument's own VCA shapes it) and is kept alive while
 * the instrument sounds; the others are silent, a key let go gone at once */
static int32_t mini_amp(track_t *t, voice_t *v, int32_t adsr)
{
    Eng *e;
    MiniPart *P = part_of(t, &e);
    int i;
    if (!P) return 0;
    if (!v->gate) for (i = 0; i < P->nkeys; i++) if (P->keys[i].v == v) { key_up(P, e, i); break; }
    if (!P->rv || !P->rv->active || P->rv->age != P->rv_age) { P->rv = v; P->rv_age = v->age ? v->age : 1; }
    if (v == P->rv) {
        if (v->stage == 4) return adsr;                  /* (given up by the host: its fade) */
        if (eng_sounding(e)) { v->env = 1 << 24; return 32767; }
        P->rv = 0;
    }
    if (!v->gate) { v->active = 0; v->stage = 0; v->env = 0; }
    return 0;
}
static void mini_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    Eng *e;
    MiniPart *P = part_of(t, &e);
    int32_t y[CTL], velc;
    uint32_t i;
    if (!P || v != P->rv || n > CTL) return;
    /* the host's pitch (bend, its LFO, ENV 1 to the pitch) and cutoff (its LFO and envelopes to FLT): offsets */
    eng_offsets(e, m->pitch16 - (int32_t)v->note * 16 + (m->fine * 270) / 4000,   /* (fine: 1/4096 of the increment, 0.0676 of a 1/16 st) */
                (m->cutoff * 55) >> 10);                                        /* (0..127 << 8 over 9.06 octaves: 1/192 octave) */
    eng_render(e, y, n);
    /* the instrument has no velocity: the host's (of the voice that plays) taken back out */
    velc = v->vel ? (127 << 12) / v->vel : 4096;
    for (i = 0; i < n; i++) out[i] += voice_amp(mulsh_32(y[i], (uint32_t)velc, 12) * 6, m, i);
}

static const engine_t MINI_DSP = {
    .name = "Mini",
    .note_on = mini_note_on,
    .render = mini_render,
    .amp = mini_amp,
    .block = mini_block,
    .mem = sizeof(MiniPart),
};

/* ------------------------------------------------------------------ what the host shows --- */
static const char *const RANGES[] = {"Lo", "32'", "16'", "8'", "4'", "2'", 0};
static const char *const RANGES3[] = {"Lo", "32'", "16'", "8'", "4'", "2'", "LFO", 0};
static const char *const WAVES1[] = {"Tri", "Shrk", "Saw", "Sqr", "Wide", "Nrrw", "Off", 0};
static const char *const WAVES2[] = {"Tri", "Shrk", "Saw", "Sqr", "Wide", "Nrrw", 0};
static const char *const WAVES3[] = {"Tri", "Ramp", "Saw", "Sqr", "Wide", "Nrrw", 0};
static const char *const KTRACK[] = {"Off", "1/3", "2/3", "Full", 0};
static const char *const MODS[] = {"3>Pit", "3>Flt", "3>Both", "N>Flt", "N>Both", "M>Flt", "M>Both", 0};
static const KEngine MINI_DESC = {
    .name = "Mini",
    .uid = K_UID('m', 'i', 'n', 'i'),
    .page_title = {"OSC 1 2", "OSC 3", "MIXER"},
    .edit = {
        {"RNG1", KF_ENUM, 0, 5, 3, RANGES, 0},
        {"WAV1", KF_ENUM, 0, 6, 2, WAVES1, 0},
        {"RNG2", KF_ENUM, 0, 5, 3, RANGES, 0},
        {"WAV2", KF_ENUM, 0, 5, 2, WAVES2, 0},
        {"FRQ2", KF_BIPCT, -64, 63, 4, 0, 0},
        {"RNG3", KF_ENUM, 0, 6, 2, RANGES3, 0},
        {"WAV3", KF_ENUM, 0, 5, 2, WAVES3, 0},
        {"FRQ3", KF_BIPCT, -64, 63, -4, 0, 0},
        {"OSC2", KF_PCT, 0, 127, 100, 0, 0},
        {"OSC3", KF_PCT, 0, 127, 0, 0, 0},
        {"NOIS", KF_PCT, 0, 127, 0, 0, 0},
        {"GLID", KF_PCT, 0, 127, 0, 0, 0},
        {"CUT", KF_PCT, 0, 127, 64, 0, 0},
        {"EMPH", KF_PCT, 0, 127, 40, 0, 0},
        {"AMT", KF_PCT, 0, 127, 60, 0, 0},
        {"KTRK", KF_ENUM, 0, 3, 1, KTRACK, 0},
        {"DCY", KF_PCT, 0, 127, 51, 0, 0},
        {"SUS", KF_PCT, 0, 127, 40, 0, 0},
        {"MOD", KF_ENUM, 0, 6, 0, MODS, 0},
        {"MODW", KF_PCT, 0, 127, 0, 0, 0},
    },
    .macro = {12, 13, 14, 11},   /* HOME's cards: CUT EMPH AMT GLID */
    .nparams = 20,
    .page2 = 12,
    .nomod = K_NOMOD_SHAPE,      /* (it reads the cutoff's: ENV 1 and the LFO to FLT) */
};


/* ------------------------------------------------------------------ the panel --- */
/* The screens: the panel is a picture under the host's knobs (the first widget of each table: drawn first). The picture draws the
 * wood, the panel, the sections; the host draws each knob as an instrument's (K_ST_SKIRT: a cream skirt with its dial, a black cap,
 * one pointer, its rim lit for the row the knobs hold), its label and its value. The two switches (KTRK, MOD) are rockers in the picture, the
 * host's widget there a label alone. Three tables:
 *   SQUARE (the FM-1, 240 x 240): SOUND's three rows of four, EDIT's two, a cheek at each side; no room for the sections' names
 *     beside a label, a knob and a value in 15-pixel type, so the panel's seams part the sections;
 *   SMALL (the Zero, the handhelds, ~320 x 170): two rows;
 *   LARGE (a desktop, the web): the sections side by side under their names, SOUND's twelve in a row, EDIT's eight under it. */
#ifndef K_ST_SKIRT
#define K_ST_SKIRT 64   /* (interface 1.8: the host draws an instrument's knob; an older host its plain knob) */
#endif
#define KN(k, x, y, w, h) {K_W_KNOB, K_ST_SKIRT, k, x, y, w, h, 0}
#define SW(k, x, y, w, h) {K_W_VALUE, K_ST_NOVALUE, k, x, y, w, h, 0}
#define PIC(id, w, h) {K_W_PICTURE, K_ST_FLAT, id, 0, 0, w, h, 0}
static const KWidget SQ_SOUND[] = {
    PIC(0, 14, 3),
    KN(0, 1, 0, 3, 1), KN(1, 4, 0, 3, 1), KN(2, 7, 0, 3, 1), KN(3, 10, 0, 3, 1),
    KN(4, 1, 1, 3, 1), KN(5, 4, 1, 3, 1), KN(6, 7, 1, 3, 1), KN(7, 10, 1, 3, 1),
    KN(8, 1, 2, 3, 1), KN(9, 4, 2, 3, 1), KN(10, 7, 2, 3, 1), KN(11, 10, 2, 3, 1),
};
static const KWidget SQ_EDIT[] = {
    PIC(1, 14, 3),
    KN(12, 1, 0, 3, 1), KN(13, 4, 0, 3, 1), KN(14, 7, 0, 3, 1), SW(15, 10, 0, 3, 1),
    KN(16, 1, 1, 3, 1), KN(17, 4, 1, 3, 1), SW(18, 7, 1, 3, 1), KN(19, 10, 1, 3, 1),
};
static const KWidget SM_SOUND[] = {
    PIC(2, 14, 2),
    KN(0, 1, 0, 2, 1), KN(1, 3, 0, 2, 1), KN(2, 5, 0, 2, 1), KN(3, 7, 0, 2, 1), KN(4, 9, 0, 2, 1), KN(5, 11, 0, 2, 1),
    KN(6, 1, 1, 2, 1), KN(7, 3, 1, 2, 1), KN(8, 5, 1, 2, 1), KN(9, 7, 1, 2, 1), KN(10, 9, 1, 2, 1), KN(11, 11, 1, 2, 1),
};
static const KWidget SM_EDIT[] = {
    PIC(3, 14, 2),
    KN(12, 1, 0, 3, 1), KN(13, 4, 0, 3, 1), KN(14, 7, 0, 3, 1), SW(15, 10, 0, 3, 1),
    KN(16, 1, 1, 3, 1), KN(17, 4, 1, 3, 1), SW(18, 7, 1, 3, 1), KN(19, 10, 1, 3, 1),
};
static const KWidget LG_SOUND[] = {
    PIC(4, 14, 4),
    KN(0, 1, 1, 1, 3), KN(1, 2, 1, 1, 3), KN(2, 3, 1, 1, 3), KN(3, 4, 1, 1, 3), KN(4, 5, 1, 1, 3), KN(5, 6, 1, 1, 3),
    KN(6, 7, 1, 1, 3), KN(7, 8, 1, 1, 3), KN(8, 9, 1, 1, 3), KN(9, 10, 1, 1, 3), KN(10, 11, 1, 1, 3), KN(11, 12, 1, 1, 3),
};
static const KWidget LG_EDIT[] = {
    PIC(5, 14, 4),
    KN(12, 1, 1, 1, 3), KN(13, 2, 1, 1, 3), KN(14, 3, 1, 1, 3), SW(15, 4, 1, 1, 3),
    KN(16, 6, 1, 1, 3), KN(17, 7, 1, 1, 3), SW(18, 9, 1, 1, 3), KN(19, 10, 1, 1, 3),
};
#define NW(a) (uint8_t)(sizeof a / sizeof a[0])
static const KLayout LAYOUTS[] = {
    {K_SCR_SQUARE, K_LPAGE_SOUND, 14, 3, NW(SQ_SOUND), SQ_SOUND}, {K_SCR_SQUARE, K_LPAGE_EDIT, 14, 3, NW(SQ_EDIT), SQ_EDIT},
    {K_SCR_SMALL, K_LPAGE_SOUND, 14, 2, NW(SM_SOUND), SM_SOUND}, {K_SCR_SMALL, K_LPAGE_EDIT, 14, 2, NW(SM_EDIT), SM_EDIT},
    {K_SCR_LARGE, K_LPAGE_SOUND, 14, 4, NW(LG_SOUND), LG_SOUND}, {K_SCR_LARGE, K_LPAGE_EDIT, 14, 4, NW(LG_EDIT), LG_EDIT},
};
/* a large screen's sections: the first column of each and its name (0 ends) */
typedef struct { uint8_t x0, x1; const char *name; } Sect;
static const Sect LG_SECT[2][4] = {
    {{1, 8, "OSCILLATOR BANK"}, {9, 11, "MIXER"}, {12, 12, ""}, {0, 0, 0}},
    {{1, 4, "FILTER"}, {6, 7, "CONTOUR"}, {9, 10, "MODULATION"}, {0, 0, 0}},
};

static const KHostApi *host;
static void mini_attach(const KHostApi *h)
{
    host = h;
    part_mem = h && h->size >= offsetof(KHostApi, part_mem) + sizeof h->part_mem ? h->part_mem : 0;   /* (an older host: none) */
}

/* The colours: a host that draws a picture's own colours (KDraw version 2) gets the instrument's (walnut, a near-black panel); an
 * older host the theme's. */
#ifndef K_COL_RGB
#define K_COL_RGB(r, g, b) (0x1000000 | ((r) << 16) | ((g) << 8) | (b))
#endif
typedef struct { int wood, grain, edge, panel, seam, ink, rock, rockon; } Pal;
static Pal palette(const KDraw *g)
{
    Pal c;
    if (g->version >= 2) {
        c.wood = K_COL_RGB(96, 56, 30); c.grain = K_COL_RGB(72, 40, 20); c.edge = K_COL_RGB(140, 92, 54);
        c.panel = K_COL_RGB(26, 26, 28); c.seam = K_COL_RGB(70, 70, 74); c.ink = K_COL_RGB(222, 218, 204);
        c.rock = K_COL_RGB(44, 44, 48);
        c.rockon = K_COL_RGB(222, 218, 204);
    } else {
        c.wood = K_COL_FAINT; c.grain = K_COL_LINE; c.edge = K_COL_MID; c.panel = K_COL_BG; c.seam = K_COL_LINE;
        c.ink = K_COL_MID; c.rock = K_COL_FAINT; c.rockon = K_COL_TEXT;
    }
    return c;
}
/* where the host puts a knob in a widget's cells (plug_widget: the label's line over it, the value's under), in the picture's frame */
typedef struct { int cx, cy, r, x0, x1, vy; } Spot;
static Spot spot(const KDraw *g, const KLayout *L, const KWidget *w)
{
    const int aw = g->w + 4, ah = g->h + 4;   /* (the grid's area: the picture's frame is it less 2 at each side) */
    const int x0 = aw * w->x / L->cols, x1 = aw * (w->x + w->w) / L->cols, y0 = ah * w->y / L->rows, y1 = ah * (w->y + w->h) / L->rows;
    const int lh = g->text_h, top = y0 + 2 + lh + 1, bot = y1 - 2 - lh - 2, rw = x1 - x0 - 4;
    Spot s;
    s.r = ((bot - top) < rw ? (bot - top) : rw) / 2 - 1;
    if (s.r > 20) s.r = 20;
    s.cx = (x0 + x1) / 2 - 2;
    s.cy = top + (bot - top) / 2 - 2;
    s.x0 = x0 - 2; s.x1 = x1 - 2;
    s.vy = bot - 2 + 1;
    return s;
}
static void cheek(const KDraw *g, const Pal *c, int x, int w)
{
    int k;
    g->fill(g, x, 0, w, g->h, c->wood);
    for (k = 1; k < 4; k++) g->line(g, x + k * w / 4, 1, x + k * w / 4 + (k & 1), g->h - 2, c->grain);
    g->fill(g, x, 0, 1, g->h, c->edge);
    g->fill(g, x + w - 1, 0, 1, g->h, c->grain);
}
/* the rockers of a switch: KTRK's two (1/3, 2/3: on together, all of it), MOD's (the pitch's, the filter's), its value's name under */
static void rockers(const KDraw *g, const Pal *c, const Spot *s, int k, int v)
{
    static const char *const KT[4] = {"Off", "1/3", "2/3", "Full"}, *const MD[7] = {"3>Pit", "3>Flt", "3>Both", "N>Flt", "N>Both", "M>Flt", "M>Both"};
    const int on[2] = {k == 15 ? (v & 1) : v == 0 || v == 2 || v == 4 || v == 6, k == 15 ? (v >> 1) & 1 : v != 0};
    const int w = s->r > 7 ? s->r - 1 : 7, h = w + w / 2, y = s->cy - h / 2;
    const char *t = k == 15 ? KT[v & 3] : MD[v < 0 ? 0 : v > 6 ? 6 : v];
    int q;
    for (q = 0; q < 2; q++) {
        const int x = q ? s->cx + 2 : s->cx - 2 - w;
        g->fill(g, x - 1, y - 1, w + 2, h + 2, c->seam);
        g->fill(g, x, y, w, h, c->rock);
        g->fill(g, x + 1, on[q] ? y + 1 : y + h / 2, w - 2, h / 2 - 1, on[q] ? c->rockon : c->seam);
    }
    if (g->text_w(g, t) < s->x1 - s->x0) g->text(g, s->cx - g->text_w(g, t) / 2, s->vy, t, c->ink);
}
static void mini_draw(const KDraw *g, int id, const int16_t *val, int n)
{
    static const KLayout *const OF[6] = {&LAYOUTS[0], &LAYOUTS[1], &LAYOUTS[2], &LAYOUTS[3], &LAYOUTS[4], &LAYOUTS[5]};
    const Pal c = palette(g);
    const KLayout *L;
    const int edit = id & 1, large = id >= 4;
    int cw, k, r;
    if (id < 0 || id > 5 || g->w < 40 || g->h < 30 || n < 20) return;
    L = OF[id];
    cw = (g->w + 4) / L->cols;
    g->fill(g, 0, 0, g->w, g->h, c.panel);
    if (large) {   /* the sections' names over their knobs, a seam between, the panel's lower lip */
        const Sect *S = LG_SECT[edit];
        for (k = 0; S[k].name; k++) {
            const int x = (g->w + 4) * S[k].x0 / L->cols - 2;
            if (k) g->fill(g, x - 3, 3, 1, g->h - 8, c.seam);
            if (S[k].name[0]) g->text(g, x + 4, ((g->h + 4) / L->rows - g->text_h) / 2, S[k].name, c.ink);
        }
        g->fill(g, cw / 2, g->h - 3, g->w - cw, 3, c.edge);
        cheek(g, &c, 0, cw / 2);
        cheek(g, &c, g->w - cw / 2, cw / 2);
    } else {       /* the rows' seams; the cheeks */
        for (r = 1; r < L->rows; r++) g->fill(g, cw, (g->h + 4) * r / L->rows - 2, g->w - 2 * cw, 1, c.seam);
        cheek(g, &c, 0, cw - 3);
        cheek(g, &c, g->w - cw + 3, cw - 3);
        if (id == 1) {   /* EDIT's third row has no knobs: the nameplate there */
            const int y = (g->h + 4) * 2 / 3, h = (g->h + 4) / 3 - 4, tw = g->text_w(g, "Mini");
            g->fill(g, g->w / 2 - tw / 2 - 10, y + h / 2 - g->text_h / 2 - 4, tw + 20, g->text_h + 8, c.seam);
            g->fill(g, g->w / 2 - tw / 2 - 9, y + h / 2 - g->text_h / 2 - 3, tw + 18, g->text_h + 6, c.panel);
            g->text(g, g->w / 2 - tw / 2, y + h / 2 - g->text_h / 2, "Mini", c.ink);
        }
    }
    for (k = 1; k < L->n; k++) {   /* the switches: rockers (the knobs are the host's) */
        const KWidget *w = &L->w[k];
        const Spot s = spot(g, L, w);
        if (w->param == 15 || w->param == 18) rockers(g, &c, &s, w->param, val[w->param]);
    }
}

/* ------------------------------------------------------------------ presets --- */
#include "mini_presets.h"
#if K_PLUGIN_EXT_VERSION >= 2
static const KPreset MINI_PRESETS[] = {MINI_PRESET_LIST};
#endif

static const KPluginExt MINI_EXT = {
    .size = sizeof(KPluginExt), .version = K_PLUGIN_EXT_VERSION, .attach = mini_attach,
    .layouts = LAYOUTS, .nlayouts = (uint8_t)(sizeof LAYOUTS / sizeof LAYOUTS[0]), .draw = mini_draw,
#if K_PLUGIN_EXT_VERSION >= 2
    .presets = MINI_PRESETS, .npresets = (uint8_t)(sizeof MINI_PRESETS / sizeof MINI_PRESETS[0]),
#endif
};
static const KPlugin MINI = {sizeof(KPlugin), K_PLUGIN_ABI, 3, K_PLUG_SYNTH, &MINI_DESC, &MINI_DSP,
                             FS, 0, MINI_COST, 0, 0, &MINI_EXT};
/* ram: its state is in the parts' memory (engine_t.mem).
 * cost (MINI_COST, mini_int.h / below): one voice sounds whatever is held (mono), the rest are silent; the track's time is one
 * voice's, declared spread over the room's six (memos/2026-11-22_mini_plugin.md has the measures). */

KREISEL_PLUGIN_EXPORT const KPlugin *kreisel_plugin(void) { return &MINI; }
