/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * A dev tool (not part of the plugin): Macro's pages drawn by the app's own UI code, a note sounding (so the waveform's frame
 * shows the voice: the host's scope), each model's SOUND page and the EDIT page, on the FM-1 (240 x 240) and the web's screen
 * (640 x 360), as PPM files: OUT_DIR/<profile>_<page>_<model>.ppm. Usage: macro_shots OUT_DIR */
#include "draw.h"
#include "kreisel.h"
#include "kreisel_audio.h"
#include "kreisel_engine.h"
#include "kreisel_plugin.h"
#include <stdio.h>
#include <stdlib.h>

const KPlugin *macro_plugin(void);

static void ppm(const char *path, const GCanvas *cv)
{
    FILE *f = fopen(path, "wb");
    int i;
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", cv->pw, cv->ph);
    for (i = 0; i < cv->pw * cv->ph; i++) {
        const uint32_t p = GPIX_RGB(cv->px[i]);
        fputc((int)(p >> 16 & 255), f); fputc((int)(p >> 8 & 255), f); fputc((int)(p & 255), f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    static const char *const PROF[2] = {"fm1", "web"};
    static const char *const NAMES[8] = {"pair", "fold", "fm", "grain", "harm", "wave", "chord", "noise"};
    static KApp a;
    static KAudio au;
    float buf[2 * 735];
    char path[600];
    int e, pi, m, k, f;
    if (argc != 2) { fprintf(stderr, "usage: macro_shots OUT_DIR\n"); return 2; }
    if (k_plugin_register(macro_plugin()) < 0) return 1;
    e = k_engine_find(K_UID('m', 'a', 'c', 'r'));
    for (pi = 0; pi < 2; pi++) {
        const KProfile *p = k_profile_find(PROF[pi]);
        if (!p || !ui_draw_init(p, 0)) { fprintf(stderr, "no profile %s\n", PROF[pi]); return 1; }
        for (m = 0; m < 9; m++) {   /* the eight models' SOUND, then EDIT (Pair) */
            const KEngine *d = k_engine_at(e);
            const int model = m < 8 ? m : 0;
            KCursorVisual c = {0};
            k_init(&a); k_set_shown(&a, p->shown); k_set_layout(&a, k_profile_layout(p));
            k_track_engine(&a, 0, e);
            for (k = 0; k < d->nparams; k++) a.params[0][K_PAGE_SOUND][4 + k] = d->edit[k].def;
            a.params[0][K_PAGE_SOUND][4 + 0] = (KVal)model;
            a.params[0][K_PAGE_SOUND][4 + 4] = 127;   /* the gate open: the waveform holds while the frame is drawn */
            a.track = 0;
            k_audio_init(&au, 44100);
            k_audio_sync(&au, &a);
            k_audio_note(&au, 0, 48, 100, 1);
            for (f = 0; f < 20; f++) { k_audio_render(&au, buf, 735); k_audio_sync(&au, &a); }
            if (m == 8) a.params[0][K_PAGE_SOUND][4 + 4] = 70;
            k_open_page(&a, m < 8 ? K_PAGE_SOUND : K_PAGE_EDIT);
            ui_draw(&a, &c, 5000); ui_draw(&a, &c, 5016);
            snprintf(path, sizeof path, "%s/%s_%s.ppm", argv[1], PROF[pi], m < 8 ? NAMES[m] : "edit");
            ppm(path, ui_draw_canvas());
        }
        ui_draw_shutdown();
    }
    return 0;
}
