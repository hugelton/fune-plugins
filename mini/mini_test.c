/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Part of MINI (plugins/mini), based on CA-72 by Idle Foundry (GPL-3.0-or-later). */
/* MINI's test: it registers in the FM-1's room (integers, no FPU) and a desktop's, sounds at its defaults, never overflows (every
 * level, EMPHASIS and the contour at full, at low and high cutoffs and notes), whistles at EMPHASIS 10 and the whistle follows the
 * keys, is mono with last-note priority; its factory presets (mini_presets.h) each sound and stay in range.
 *   kreisel_mini_test [DIR]   with a folder: each preset as DIR/<n>_<name>.wav (a phrase, then a held note under a filter sweep) */
#include "kreisel.h"
#include "kreisel_audio.h"
#include "kreisel_engine_api.h"
#include "kreisel_plugin_load.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct { const char *name, *set; } Preset;
#include "mini_presets.h"
static const Preset PRESETS[] = {MINI_PRESET_LIST};
#define NPRESET ((int)(sizeof PRESETS / sizeof PRESETS[0]))

static int mini;
static KAudio au;

static int pidx(const char *label)
{
    const KEngine *e = k_engine_at(mini);
    int i;
    for (i = 0; i < e->nparams; i++) if (!strcmp(e->edit[i].label, label)) return i;
    return -1;
}
static void set(KApp *g, const char *label, int v) { const int i = pidx(label); assert(i >= 0); g->params[0][K_PAGE_SOUND][4 + i] = (KVal)v; }
static void take(KApp *g)   /* track 1 plays MINI at its defaults */
{
    const KEngine *e;
    int i;
    k_init(g);
    g->sound[0] = mini;
    e = k_engine_at(mini);
    for (i = 0; i < e->nparams; i++) g->params[0][K_PAGE_SOUND][4 + i] = e->edit[i].def;
}
/* a preset's words over the defaults (as interface 1.5's KPreset): LABEL=V, V a number or a name; A= D= S= R= ENV 1 */
static void apply(KApp *g, const char *words)
{
    const KEngine *e = k_engine_at(mini);
    char buf[512], *w, *save = 0;
    snprintf(buf, sizeof buf, "%s", words);
    for (w = strtok_r(buf, " ", &save); w; w = strtok_r(0, " ", &save)) {
        char *eq = strchr(w, '=');
        int i, v, k;
        assert(eq);
        *eq = 0;
        if (!w[1] && strchr("ADSR", w[0])) { g->params[0][K_PAGE_ENV1][strchr("ADSR", w[0]) - "ADSR"] = (KVal)atoi(eq + 1); continue; }
        i = pidx(w);
        assert(i >= 0);
        if (e->edit[i].fmt == KF_ENUM) {
            for (k = 0, v = -1; e->edit[i].names[k]; k++) if (!strcmp(e->edit[i].names[k], eq + 1)) v = k;
            if (v < 0) { fprintf(stderr, "no name %s for %s\n", eq + 1, w); assert(0); }
        } else v = atoi(eq + 1);
        assert(v >= e->edit[i].min && v <= e->edit[i].max);
        g->params[0][K_PAGE_SOUND][4 + i] = (KVal)v;
    }
}

typedef struct { double peak, rms; int clip, bad, up; size_t n; } Meas;
static void meas(const float *b, size_t n, Meas *m)
{
    size_t i;
    double s = 0;
    memset(m, 0, sizeof *m);
    m->n = n;
    for (i = 0; i < n; i++) {
        const float x = b[2 * i];
        if (!isfinite(x)) m->bad++;
        if (fabsf(x) > m->peak) m->peak = fabsf(x);
        if (fabsf(x) >= 0.999f) m->clip++;
        s += (double)x * x;
        if (i && b[2 * i - 2] < 0.f && x >= 0.f) m->up++;
    }
    m->rms = sqrt(s / (double)n);
}
static float buf[2 * 44100 * 8];
static void render(size_t frames, float *dst) { k_audio_render(&au, dst, frames); }

static void wav(const char *path, const float *b, size_t frames)
{
    FILE *f = fopen(path, "wb");
    uint32_t v;
    size_t i;
    if (!f) { perror(path); return; }
#define W32(x) (v = (uint32_t)(x), fwrite(&v, 4, 1, f))
#define W16(x) do { uint16_t h_ = (uint16_t)(x); fwrite(&h_, 2, 1, f); } while (0)
    fwrite("RIFF", 1, 4, f); W32(36 + frames * 4); fwrite("WAVEfmt ", 1, 8, f); W32(16); W16(1); W16(2); W32(44100); W32(44100 * 4);
    W16(4); W16(16); fwrite("data", 1, 4, f); W32(frames * 4);
    for (i = 0; i < frames * 2; i++) { const float x = b[i] > 1.f ? 1.f : b[i] < -1.f ? -1.f : b[i]; W16((int16_t)lrintf(x * 32767.f)); }
    fclose(f);
}

/* a preset: a phrase (some notes legato, to glide), then a held note while CUT sweeps down and up; frames written into buf */
static size_t play_preset(KApp *g, int base)
{
    static const int8_t PH[8] = {0, 0, 12, 7, 3, 5, 7, 0};
    static const uint8_t LEG[8] = {0, 0, 0, 1, 0, 1, 1, 0};   /* a note held over the next's start */
    const int cut = g->params[0][K_PAGE_SOUND][4 + pidx("CUT")];
    size_t at = 0, k;
    int i;
    k_audio_init(&au, 44100);
    k_audio_sync(&au, g);
    for (i = 0; i < 8; i++) {
        const size_t len = 44100 * 22 / 100;
        k_audio_note(&au, 0, base + PH[i], 100, 1);
        if (i && LEG[i - 1]) k_audio_note(&au, 0, base + PH[i - 1], 0, 0);   /* (the last, held over, let go now) */
        render(len - 882, buf + 2 * at); at += len - 882;
        if (!LEG[i]) k_audio_note(&au, 0, base + PH[i], 0, 0);
        render(882, buf + 2 * at); at += 882;
    }
    render(44100 / 3, buf + 2 * at); at += 44100 / 3;
    k_audio_note(&au, 0, base, 100, 1);
    for (k = 0; k < 150; k++) {   /* 2.5 s: the cutoff from where the preset has it down to 0, up to 127, back */
        const int ph = (int)k, c = ph < 50 ? cut - cut * ph / 50 : ph < 100 ? 127 * (ph - 50) / 50 : 127 - (127 - cut) * (ph - 100) / 50;
        g->params[0][K_PAGE_SOUND][4 + pidx("CUT")] = (KVal)c;
        k_audio_sync(&au, g);
        render(735, buf + 2 * at); at += 735;
    }
    g->params[0][K_PAGE_SOUND][4 + pidx("CUT")] = (KVal)cut;
    k_audio_note(&au, 0, base, 0, 0);
    render(44100, buf + 2 * at); at += 44100;
    return at;
}

static double block_us;

static int path(const char *lib, const KDeviceCaps *caps, const char *dir)
{
    static KApp g;
    Meas m;
    int i;
    k_plugin_reset();
    k_plugin_set_caps(caps);
    mini = k_plugin_load_file(lib);
    assert(mini >= K_ENGINE_COUNT && k_engine_find(K_UID('m', 'i', 'n', 'i')) == mini);
    assert(k_plugin_check(k_plugin_at(mini), 0) == 0 && k_plugin_fits(k_plugin_at(mini), caps, 0) == 0);
    assert(k_engine_at(mini)->nparams == 20 && k_engine_at(mini)->page2 == 12);
    assert(k_plugin_at(mini)->ext && k_plugin_layout(mini, K_LPAGE_SOUND, K_SCR_SQUARE) && k_plugin_layout(mini, K_LPAGE_EDIT, K_SCR_LARGE));
    assert(!(k_plugin_at(mini)->needs & K_NEED_FLOAT));

    /* the defaults: a note sounds, in range; its time a block here */
    take(&g);
    k_audio_init(&au, 44100); k_audio_sync(&au, &g);
    k_audio_note(&au, 0, 57, 100, 1);
    {
        const clock_t c0 = clock();
        render(44100 / 2, buf);
        block_us = (double)(clock() - c0) / CLOCKS_PER_SEC * 1e6 / (44100.0 / 2 / 32);
    }
    meas(buf + 2 * 4410, 44100 / 2 - 4410, &m);
    printf("defaults: rms %.3f peak %.3f; %.1f us a block (the whole host here)\n", m.rms, m.peak, block_us);
    assert(!m.bad && m.rms > 0.01 && m.peak <= 1.0);

    /* everything at full: no overflow (finite, not held at full scale), at the ends of the cutoff and of the keys */
    {
        static const int CUTS[3] = {0, 64, 127}, NOTES[3] = {24, 60, 108};
        int c, nn;
        for (c = 0; c < 3; c++)
            for (nn = 0; nn < 3; nn++) {
                take(&g);
                apply(&g, "OSC2=127 OSC3=127 NOIS=127 EMPH=127 AMT=127 KTRK=Full WAV1=Sqr WAV2=Nrrw MOD=M>Both MODW=127 RNG1=2' RNG2=2' "
                          "A=0 D=100 S=100 R=50");
                set(&g, "CUT", CUTS[c]);
                k_audio_init(&au, 44100); k_audio_sync(&au, &g);
                k_audio_note(&au, 0, NOTES[nn], 127, 1);
                render(44100, buf);
                meas(buf, 44100, &m);
                printf("full: cut %3d note %3d  rms %.3f peak %.3f clip %d\n", CUTS[c], NOTES[nn], m.rms, m.peak, m.clip);
                assert(!m.bad && m.peak <= 1.0 && m.clip < 441 && m.rms < 0.9);
            }
    }

    {   /* (the same with the wheel at rest: the core, without the modulation) */
        take(&g);
        apply(&g, "OSC2=127 OSC3=127 NOIS=127 EMPH=127 AMT=127 KTRK=Full WAV1=Sqr WAV2=Nrrw MOD=M>Both MODW=0 RNG1=2' RNG2=2' "
                  "A=0 D=100 S=100 R=50 CUT=64");
        k_audio_init(&au, 44100); k_audio_sync(&au, &g);
        k_audio_note(&au, 0, 60, 127, 1);
        render(44100, buf);
        meas(buf, 44100, &m);
        printf("full, the wheel at rest: cut 64 note 60  rms %.3f peak %.3f\n", m.rms, m.peak);
        take(&g);
        apply(&g, "OSC2=0 OSC3=0 NOIS=0 MOD=3>Pit MODW=0 A=0 D=100 S=100 R=50 CUT=64 EMPH=64");
        k_audio_init(&au, 44100); k_audio_sync(&au, &g);
        k_audio_note(&au, 0, 60, 127, 1);
        render(44100, buf);
        meas(buf, 44100, &m);
        printf("one sawtooth, EMPHASIS 5: rms %.3f peak %.3f\n", m.rms, m.peak);
        for (int w = 0; w < 3; w++) {
            static const char *const W[3] = {"MOD=3>Pit MODW=60 WAV3=Tri RNG3=LFO FRQ3=40", "MOD=3>Flt MODW=60 WAV3=Tri RNG3=LFO FRQ3=40",
                                             "MOD=N>Flt MODW=60"};
            char words[200];
            take(&g);
            snprintf(words, sizeof words, "OSC2=0 OSC3=0 NOIS=0 A=0 D=100 S=100 R=50 CUT=64 EMPH=64 %s", W[w]);
            apply(&g, words);
            k_audio_init(&au, 44100); k_audio_sync(&au, &g);
            k_audio_note(&au, 0, 60, 127, 1);
            render(44100, buf);
            meas(buf, 44100, &m);
            printf("  %-44s rms %.3f peak %.3f up %d\n", W[w], m.rms, m.peak, m.up);
        }
    }

    /* EMPHASIS at 10 and nothing in the mixer: it whistles, and an octave up the keys about an octave up the whistle */
    {
        int up60, up72;
        take(&g);
        apply(&g, "WAV1=Off OSC2=0 OSC3=0 NOIS=0 CUT=64 EMPH=127 AMT=0 KTRK=Full A=0 D=50 S=100 R=20");
        k_audio_init(&au, 44100); k_audio_sync(&au, &g);
        k_audio_note(&au, 0, 60, 100, 1);
        render(44100, buf);
        meas(buf + 2 * 22050, 22050, &m);
        up60 = m.up;
        printf("whistle 60: rms %.3f, %d crossings in 0.5 s\n", m.rms, m.up);
        assert(m.rms > 0.01);
        k_audio_init(&au, 44100); k_audio_sync(&au, &g);
        k_audio_note(&au, 0, 72, 100, 1);
        render(44100, buf);
        meas(buf + 2 * 22050, 22050, &m);
        up72 = m.up;
        printf("whistle 72: rms %.3f, %d crossings in 0.5 s\n", m.rms, m.up);
        assert(m.rms > 0.01 && up72 > up60 * 17 / 10 && up72 < up60 * 23 / 10);
    }

    /* mono, last-note priority: C4 held, C5 over it sounds alone; C5 let go, C4 again (legato) */
    {
        int a1, a2, a3;
        take(&g);
        apply(&g, "WAV1=Saw OSC2=0 OSC3=0 CUT=127 EMPH=0 AMT=0 GLID=0 A=0 D=50 S=100 R=10");
        k_audio_init(&au, 44100); k_audio_sync(&au, &g);
        k_audio_note(&au, 0, 60, 100, 1);
        render(22050, buf); meas(buf + 2 * 11025, 11025, &m); a1 = m.up;
        k_audio_note(&au, 0, 72, 100, 1);
        render(22050, buf); meas(buf + 2 * 11025, 11025, &m); a2 = m.up;
        k_audio_note(&au, 0, 72, 0, 0);
        render(22050, buf); meas(buf + 2 * 11025, 11025, &m); a3 = m.up;
        printf("mono: C4 %d, C4+C5 %d, C5 let go %d crossings a quarter second (rms %.3f)\n", a1, a2, a3, m.rms);
        assert(a1 >= 63 && a1 <= 68 && a2 >= 128 && a2 <= 134 && a3 >= 63 && a3 <= 68 && m.rms > 0.03);
    }

    /* the presets: each sounds, in range; kept for the comparison; with a folder, each a WAV */
    for (i = 0; i < NPRESET && i < 8; i++) {
        static const uint8_t BASE[8] = {36, 60, 55, 36, 60, 60, 72, 64};
        size_t n;
        take(&g);
        apply(&g, PRESETS[i].set);
        n = play_preset(&g, BASE[i]);
        meas(buf, n, &m);
        printf("preset %d %-11s rms %.3f peak %.3f clip %d (%.1f s)\n", i + 1, PRESETS[i].name, m.rms, m.peak, m.clip, n / 44100.0);
        assert(!m.bad && m.rms > 0.003 && m.peak <= 1.0 && m.clip < 44);
        if (dir) {
            char p[512], nm[32];
            int c;
            snprintf(nm, sizeof nm, "%s", PRESETS[i].name);
            for (c = 0; nm[c]; c++) if (nm[c] == ' ') nm[c] = '_';
            snprintf(p, sizeof p, "%s/%d_%s.wav", dir, i + 1, nm);
            wav(p, buf, n);
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const KDeviceCaps fm1 = {44100u, 16u * 1024u, 120u, 6u, 0u}, desk = {44100u, 1u << 20, 2000u, 6u, 1u};
    k_plugin_set_caps(&desk);
    assert(k_plugin_load_file(KREISEL_MINI) >= K_ENGINE_COUNT);   /* (a desktop's room takes it) */
    path(KREISEL_MINI, &fm1, argc > 1 ? argv[1] : 0);            /* (the FM-1's, and everything played there) */
    printf("kreisel mini test: OK\n");
    return 0;
}
