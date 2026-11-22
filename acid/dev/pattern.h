/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Leo Kuroshita (@kurogedelic), Hugelton Instruments */
/* The 16-step line the dev harness and the WAVs play (plugins/acid/README.md): a note (-1 a rest), an accent, a slide into the
 * next step (the note held over it: the next one comes while it sounds). 125 BPM, sixteenths; a note without a slide lets go
 * 0.08 of a step before the next (as Fune's sequencer). */
#ifndef ACID_PATTERN_H
#define ACID_PATTERN_H
typedef struct { int note, acc, slide; } AcidStep;
static const AcidStep ACID_LINE[16] = {
    {36, 1, 0}, {36, 0, 0}, {48, 0, 1}, {46, 0, 0}, {36, 1, 0}, {-1, 0, 0}, {39, 0, 1}, {41, 0, 0},
    {36, 0, 0}, {48, 1, 0}, {36, 0, 1}, {43, 0, 0}, {36, 1, 0}, {34, 0, 0}, {46, 0, 1}, {48, 1, 0},
};
#define ACID_STEP_SAMPLES 5292   /* 125 BPM: 0.12 s at 44.1 kHz */
#define ACID_GATE_SAMPLES 4868   /* 0.92 of a step */
/* the events of `bars` times the line, at sample times (rounded to the host's 32-sample blocks) */
typedef struct { int at, note, vel, on; } AcidEvent;
static int acid_line_events(AcidEvent *ev, int max, int bars, int acc_vel, int vel)
{
    int b, i, n = 0, held = -1;
    for (b = 0; b < bars; b++)
        for (i = 0; i < 16; i++) {
            const AcidStep *s = &ACID_LINE[i];
            const int at = ((b * 16 + i) * ACID_STEP_SAMPLES) & ~31;
            if (s->note >= 0 && n < max) ev[n++] = (AcidEvent){at, s->note, s->acc ? acc_vel : vel, 1};
            if (held >= 0 && held != s->note && n < max) ev[n++] = (AcidEvent){at, held, 0, 0};   /* the slide's last note: off after */
            held = -1;
            if (s->note < 0) continue;
            if (s->slide) held = s->note;
            else if (n < max) ev[n++] = (AcidEvent){(((b * 16 + i) * ACID_STEP_SAMPLES) + ACID_GATE_SAMPLES) & ~31, s->note, 0, 0};
        }
    if (held >= 0 && n < max) ev[n++] = (AcidEvent){(bars * 16 * ACID_STEP_SAMPLES) & ~31, held, 0, 0};
    return n;
}
#endif
