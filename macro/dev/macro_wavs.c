/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * A dev tool (not part of the plugin): Macro played through Fune's own audio path (k_audio_*, 44.1 kHz, the track's envelope,
 * the mixer and the master), each model as WAVs to listen to:
 *   <model>_sweep.wav   a held C3, the gate open: HARM 0 .. 127, then TIMB, then MORF (1.6 s each; the others at 64)
 *   <model>_phrase.wav  a two-bar phrase at 120 BPM, the gate pinged (DCAY 70), the velocities varied
 *   preset_<n>.wav      the factory presets, the phrase
 * Usage: macro_wavs OUT_DIR. The plugin is linked in (macro.c built with kreisel_plugin renamed macro_plugin). */
#include "kreisel.h"
#include "kreisel_audio.h"
#include "kreisel_engine.h"
#include "kreisel_plugin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const KPlugin *macro_plugin(void);
#define SR 44100
#define FRAME 735
#define MAXLEN (SR * 6)

static float L[MAXLEN], R[MAXLEN];

static void wav(const char *path, int n)
{
    FILE *f = fopen(path, "wb");
    uint32_t v;
    uint16_t s;
    int i;
    if (!f) { perror(path); exit(1); }
    fwrite("RIFF", 1, 4, f); v = 36 + (uint32_t)n * 4; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f); s = 1; fwrite(&s, 2, 1, f); s = 2; fwrite(&s, 2, 1, f);   /* 16-bit PCM, stereo */
    v = SR; fwrite(&v, 4, 1, f); v = SR * 4; fwrite(&v, 4, 1, f); s = 4; fwrite(&s, 2, 1, f); s = 16; fwrite(&s, 2, 1, f);
    fwrite("data", 1, 4, f); v = (uint32_t)n * 4; fwrite(&v, 4, 1, f);
    for (i = 0; i < n; i++) {
        const float a[2] = {L[i], R[i]};
        int k;
        for (k = 0; k < 2; k++) {
            float x = a[k] * 32767.0f;
            int16_t q = (int16_t)(x > 32767 ? 32767 : x < -32768 ? -32768 : x);
            fwrite(&q, 2, 1, f);
        }
    }
    fclose(f);
}

typedef struct { int at, len, note, vel; } Note;   /* in sixteenths at 120 BPM (5512.5 samples) */
static const Note PHRASE[] = {
    {0, 2, 48, 110}, {2, 1, 55, 70}, {3, 1, 60, 90}, {4, 2, 58, 120}, {6, 1, 55, 60}, {7, 1, 53, 80},
    {8, 3, 51, 115}, {11, 1, 58, 70}, {12, 2, 60, 100}, {14, 2, 63, 75},
    {16, 2, 48, 110}, {18, 1, 55, 70}, {19, 1, 60, 90}, {20, 2, 67, 127}, {22, 2, 65, 80},
    {24, 4, 63, 100}, {28, 4, 60, 60},
};

static int ENGINE;
static KApp g;
static KAudio a;

static void set(int k, int v) { g.params[0][K_PAGE_SOUND][4 + k] = (KVal)v; }

static void defaults(void)
{
    int k;
    const KEngine *e = k_engine_at(ENGINE);
    k_init(&g);
    k_track_engine(&g, 0, ENGINE);
    for (k = 0; k < e->nparams; k++) set(k, e->edit[k].def);
}

static int render_phrase(int n)
{
    int done = 0, i, k, sixteenth = SR / 8;
    float buf[2 * FRAME];
    k_audio_init(&a, SR);
    k_audio_sync(&a, &g);
    while (done < n) {
        const int len = n - done < FRAME ? n - done : FRAME;
        for (k = 0; k < (int)(sizeof PHRASE / sizeof PHRASE[0]); k++) {
            const int on = PHRASE[k].at * sixteenth, off = (PHRASE[k].at + PHRASE[k].len) * sixteenth - sixteenth / 4;
            if (on >= done && on < done + len) k_audio_note(&a, 0, PHRASE[k].note, PHRASE[k].vel, 1);
            if (off >= done && off < done + len) k_audio_note(&a, 0, PHRASE[k].note, 0, 0);
        }
        k_audio_render(&a, buf, (size_t)len);
        for (i = 0; i < len; i++) { L[done + i] = buf[2 * i]; R[done + i] = buf[2 * i + 1]; }
        done += len;
        k_audio_sync(&a, &g);
    }
    return n;
}

static int render_sweep(void)
{
    const int seg = SR * 16 / 10, n = seg * 3 + SR / 2;
    int done = 0, i;
    float buf[2 * FRAME];
    set(4, 127);   /* the gate open: the host's envelope (a gate) holds it */
    k_audio_init(&a, SR);
    k_audio_sync(&a, &g);
    k_audio_note(&a, 0, 48, 100, 1);
    while (done < n) {
        const int len = n - done < FRAME ? n - done : FRAME, s = done / seg, x = (done % seg) * 127 / seg;
        set(1, s == 0 ? x : 64); set(2, s == 1 ? x : 64); set(3, s == 2 ? x : 64);
        if (s >= 3) k_audio_note(&a, 0, 48, 0, 0);
        k_audio_sync(&a, &g);
        k_audio_render(&a, buf, (size_t)len);
        for (i = 0; i < len; i++) { L[done + i] = buf[2 * i]; R[done + i] = buf[2 * i + 1]; }
        done += len;
    }
    return n;
}

static void report(const char *name, int n)
{
    double peak = 0, rms = 0;
    int i;
    for (i = 0; i < n; i++) { const double v = L[i] < 0 ? -L[i] : L[i]; if (v > peak) peak = v; rms += (double)L[i] * L[i]; }
    printf("%-22s peak %.3f  rms %.4f\n", name, peak, __builtin_sqrt(rms / n));
}

/* a factory preset's words (KEY=V; the engine's labels, A D S R) set on track 1 (a dev tool's small reader) */
static void apply_preset(const char *words)
{
    const KEngine *e = k_engine_at(ENGINE);
    char w[64];
    const char *p = words;
    while (*p) {
        int n = 0, k;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && n < 63) w[n++] = *p++;
        w[n] = 0;
        if (!n) break;
        {
            char *eq = strchr(w, '=');
            if (!eq) continue;
            *eq = 0;
            for (k = 0; k < e->nparams; k++)
                if (!strcmp(e->edit[k].label, w)) {
                    int v = atoi(eq + 1);
                    if (e->edit[k].fmt == KF_ENUM) {
                        int j;
                        for (j = 0; e->edit[k].names[j]; j++) if (!strcmp(e->edit[k].names[j], eq + 1)) v = j;
                    }
                    set(k, v);
                }
            if (!strcmp(w, "A")) g.params[0][K_PAGE_ENV1][0] = (KVal)atoi(eq + 1);
            if (!strcmp(w, "D")) g.params[0][K_PAGE_ENV1][1] = (KVal)atoi(eq + 1);
            if (!strcmp(w, "S")) g.params[0][K_PAGE_ENV1][2] = (KVal)atoi(eq + 1);
            if (!strcmp(w, "R")) g.params[0][K_PAGE_ENV1][3] = (KVal)atoi(eq + 1);
        }
    }
}

int main(int argc, char **argv)
{
    static const char *const SETS[6][2] = {
        {"Soft Saw", "MODL=Pair HARM=20 TIMB=40 MORF=50 DCAY=75 COLR=60 A=0 D=60 S=100 R=30"},
        {"Fold Bass", "MODL=Fold HARM=70 TIMB=75 MORF=35 DCAY=55 COLR=25 TENV=24 A=0 D=50 S=100 R=10"},
        {"Bell Keys", "MODL=FM HARM=88 TIMB=62 MORF=70 DCAY=85 COLR=95 TENV=-20 A=0 D=80 S=100 R=50"},
        {"Glass Pad", "MODL=Wave HARM=30 TIMB=50 MORF=80 DCAY=127 COLR=64 A=45 D=60 S=90 R=60"},
        {"Organ Chord", "MODL=Chord HARM=64 TIMB=30 MORF=100 DCAY=127 LVL=90 A=3 D=60 S=100 R=35"},
        {"Hat Noise", "MODL=Noise HARM=105 TIMB=127 MORF=60 DCAY=28 COLR=100 A=0 D=30 S=100 R=15"},
    };
    static const char *const NAMES[8] = {"pair", "fold", "fm", "grain", "harm", "wave", "chord", "noise"};
    char path[600];
    int m, n;
    if (argc != 2) { fprintf(stderr, "usage: macro_wavs OUT_DIR\n"); return 2; }
    if (k_plugin_register(macro_plugin()) < 0) { fprintf(stderr, "the plugin was refused\n"); return 1; }
    ENGINE = k_engine_find(K_UID('m', 'a', 'c', 'r'));
    if (ENGINE < 0) { fprintf(stderr, "no engine\n"); return 1; }
    for (m = 0; m < 8; m++) {
        defaults(); set(0, m);
        n = render_sweep();
        snprintf(path, sizeof path, "%s/%d_%s_sweep.wav", argv[1], m + 1, NAMES[m]);
        wav(path, n); report(path + strlen(argv[1]) + 1, n);
        defaults(); set(0, m);
        n = render_phrase(SR * 9 / 2);
        snprintf(path, sizeof path, "%s/%d_%s_phrase.wav", argv[1], m + 1, NAMES[m]);
        wav(path, n); report(path + strlen(argv[1]) + 1, n);
    }
    for (m = 0; m < 6; m++) {
        int k;
        char nm[40];
        defaults();
        apply_preset(SETS[m][1]);
        n = render_phrase(SR * 9 / 2);
        for (k = 0; SETS[m][0][k] && k < 39; k++) nm[k] = SETS[m][0][k] == ' ' ? '_' : (char)(SETS[m][0][k] | 0x20);
        nm[k] = 0;
        snprintf(path, sizeof path, "%s/preset_%d_%s.wav", argv[1], m + 1, nm);
        wav(path, n); report(path + strlen(argv[1]) + 1, n);
    }
    return 0;
}
