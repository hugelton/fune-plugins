/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Leo Kuroshita (@kurogedelic), Hugelton Instruments */
/* A small host for the dev harness: the integer ACID alone, one voice, as Fune calls it (32-sample blocks, note_on with the
 * kept state put back on a retrigger, voice_t.legato, the amp hook over a gate, voice_amp), without Fune's mix (its ENV 1, DC
 * blocker, limiter, master). Built with the SDK and acid.c into the harness (build.sh); not part of the plugin. */
#include <stdlib.h>
#include <string.h>
#include "kreisel_sdk.h"
#include "../acid.c"

static track_t T;
static voice_t V;

void acid_dev_init(const int *vals)
{
    int i;
    memset(&T, 0, sizeof T);
    memset(&V, 0, sizeof V);
    memset(acid_x, 0, sizeof acid_x);
    for (i = 0; i < ACID_DESC.nparams; i++) T.p[P_E0 + i] = (int16_t)(vals && vals[i] >= 0 ? vals[i] : ACID_DESC.edit[i].def);
}
void acid_dev_note(int note, int vel, int on)
{
    if (!on) { if (V.active && V.gate && V.note == note) V.gate = 0; return; }
    {
        const int sounding = V.active, held = sounding && V.gate;
        uint32_t ph[3];
        int32_t s[8];
        int k;
        memcpy(ph, V.ph, sizeof ph);
        memcpy(s, V.s, sizeof s);
        V.note = (uint8_t)note; V.vel = (uint8_t)vel; V.legato = (uint8_t)held; V.gate = 1; V.active = 1; V.stage = 1;
        V.pitch16 = V.pitch_cur = note * 16;
        acid_note_on(&T, &V);
        if (sounding) {
            memcpy(V.ph, ph, sizeof ph);
            for (k = 0; k < 8; k++) if (k != 2 && k != 3 && (ACID_DSP.keep >> k & 1)) V.s[k] = s[k];
        }
    }
}
/* n samples (a multiple of 32) into out, as floats where 1.0 is Open303's 1.0 (the voice's 24000: README) */
void acid_dev_render(float *out, int n)
{
    int32_t b[CTL];
    int i, k;
    for (i = 0; i < n; i += CTL) {
        vmod_t m;
        memset(b, 0, sizeof b);
        if (V.active) {
            const int32_t a = acid_amp(&T, &V, 32767);   /* the gate held open: the host's ENV 1 out of the comparison */
            memset(&m, 0, sizeof m);
            m.pitch16 = V.pitch16;
            m.inc = pitch_inc((uint32_t)m.pitch16);
            m.amp0 = m.amp1 = mulq15(a, (int32_t)V.vel * 258);
            m.envq15 = a;
            acid_render(&T, &V, b, CTL, &m);
        }
        for (k = 0; k < CTL && i + k < n; k++) out[i + k] = (float)b[k] / 24000.f;
    }
}

/* preset i (its name; -1 past the last): its engine values over the defaults into vals (A D S R and the rest are the host's) */
const char *acid_dev_preset(int i, int *vals)
{
    const int n = (int)(sizeof ACID_PRESETS / sizeof ACID_PRESETS[0]);
    const KPreset *ps = ACID_PRESETS;
    int k;
    const char *w;
    if (i < 0 || i >= n) return 0;
    for (k = 0; k < ACID_DESC.nparams; k++) vals[k] = ACID_DESC.edit[k].def;
    for (w = ps[i].set; *w;) {
        char lab[8], val[12];
        int a = 0, b = 0;
        while (*w == ' ') w++;
        while (*w && *w != '=' && a < 7) lab[a++] = *w++;
        lab[a] = 0;
        if (*w == '=') w++;
        while (*w && *w != ' ' && b < 11) val[b++] = *w++;
        val[b] = 0;
        for (k = 0; k < ACID_DESC.nparams; k++)
            if (!strcmp(lab, ACID_DESC.edit[k].label)) {
                const KParamDesc *d = &ACID_DESC.edit[k];
                int e;
                vals[k] = atoi(val);
                if (d->fmt == KF_ENUM) for (e = 0; d->names[e]; e++) if (!strcmp(d->names[e], val)) vals[k] = e;
            }
    }
    return ps[i].name;
}
