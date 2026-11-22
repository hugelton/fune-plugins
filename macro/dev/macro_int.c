/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * A dev tool (not part of the plugin): Macro's integer models and its whole voice, callable from the comparison harness
 * (plaits_compare.cc) and from tools that write WAVs. Built with macro.c included, so it sees its static functions. */
#include "../macro.c"

/* one model alone (no gate, no gain): `n` samples as floats (Plaits' OUT scale, 1.0 = 32768); the controls 0 .. 127, the
 * pitch as a frequency (Hz) at 44.1 kHz. The part's block hook runs once a block, as the host does. */
void mh_model(int model, int harm, int timb, int morf, double hz, float *out, int n)
{
    static track_t t;
    static voice_t v;
    vmod_t m;
    int32_t buf[CTL];
    int i, k;
    Ctl c;
    for (i = 0; i < (int)(sizeof t.p / sizeof t.p[0]); i++) t.p[i] = 0;
    for (i = 0; i < 8; i++) v.s[i] = 0;
    v.ph[0] = v.ph[1] = v.ph[2] = 0;
    for (k = 0; k < 24; k++) mc_fallback.amp[k] = 0;   /* (no host: the fallback part) */
    mc_fallback.wpos[0] = mc_fallback.wpos[1] = mc_fallback.wpos[2] = 0;
    t.p[P_E0 + E_MODL] = (int16_t)model; t.p[P_E0 + E_HARM] = (int16_t)harm;
    t.p[P_E0 + E_TIMB] = (int16_t)timb; t.p[P_E0 + E_MORF] = (int16_t)morf;
    m.inc = (uint32_t)(hz / 44100.0 * 4294967296.0 + 0.5);
    m.pitch16 = (int32_t)((69.0 + 12.0 * __builtin_log2(hz / 440.0)) * 16.0 + 0.5);
    m.fine = 0; m.cutoff = 0; m.shape = 0; m.amp0 = m.amp1 = 32767; m.envq15 = 32767;
    c.h = p15(harm); c.tm = p15(timb); c.mo = p15(morf);
    c.inc = m.inc; c.pitch16 = m.pitch16; c.fine = 0;
    v.s[3] = (int32_t)F_SYNC;
    for (i = 0; i < n; i += CTL) {
        const int len = n - i < CTL ? n - i : CTL;
        mc_block(&t);
        c.part = mc_part(&t);
        mc_model(model, &v, buf, (uint32_t)len, &c, &m);
        for (k = 0; k < len; k++) out[i + k] = (float)buf[k] / (model == M_GRAIN ? 65536.0f : 32768.0f);   /* (Grain: x 2 in m_grain, Fune's level) */
    }
}

/* the whole voice as the host plays it (the gate, the gains, voice_amp at a full amplitude), `n` samples of a note held
 * `held` samples: the values p[0 .. 7] (edit[]), the note (MIDI), the velocity. */
void mh_voice(const int16_t *val, int note, int vel, int held, float *out, int n)
{
    static track_t t;
    static voice_t v;
    vmod_t m;
    int32_t acc[CTL];
    int i, k;
    for (i = 0; i < (int)(sizeof t.p / sizeof t.p[0]); i++) t.p[i] = 0;
    for (i = 0; i < 8; i++) { t.p[P_E0 + i] = val[i]; v.s[i] = 0; }
    v.ph[0] = v.ph[1] = v.ph[2] = 0;
    v.vel = (uint8_t)vel; v.note = (uint8_t)note;
    m.pitch16 = note * 16;
    m.inc = pitch_inc((uint32_t)m.pitch16);
    m.fine = 0; m.cutoff = 0; m.shape = 0; m.amp0 = m.amp1 = 32767; m.envq15 = 32767;
    mc_note_on(&t, &v);
    for (i = 0; i < n; i += CTL) {
        const int len = n - i < CTL ? n - i : CTL;
        const int32_t a = i < held ? 32767 : 0;
        m.amp1 = a;
        mc_block(&t);
        for (k = 0; k < CTL; k++) acc[k] = 0;
        mc_render(&t, &v, acc, (uint32_t)len, &m);
        m.amp0 = a;
        for (k = 0; k < len; k++) out[i + k] = (float)acc[k] / 32768.0f;
    }
}
