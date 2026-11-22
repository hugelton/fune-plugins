/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Part of MINI (plugins/mini), based on CA-72 by Idle Foundry (GPL-3.0-or-later). */
/* MINI's panel as both its sound paths take it: the instrument's knobs and switches in integers (knobs 0 .. 127 for the panel's
 * 0 .. 10), filled by mini.c from the track's values. The float path (mini_light, CA-72's light voice) turns them into its
 * Panel; the integer path (mini_int.h, the FM-1) reads them through its tables. */
#ifndef MINI_KNOBS_H
#define MINI_KNOBS_H
#include <stdint.h>

typedef struct {
    uint8_t range[3];       /* 0 Lo, 1 32', 2 16', 3 8', 4 4', 5 2' */
    uint8_t wave[3];        /* the CA-72's order: 0 triangle, 1 shark tooth, 2 reverse sawtooth, 3 sawtooth, 4 square, 5 wide, 6 narrow */
    uint8_t on[3];          /* the mixer's switches */
    uint8_t volume[3];      /* the mixer's VOLUMEs */
    int8_t freq[2];         /* oscillators 2 and 3's FREQUENCY, -64 .. 63: semitones x |x| x 7.5 / 4096 from the centre */
    uint8_t osc3_control;   /* OSC. 3 CONTROL (off: oscillator 3 free of the keys, a low-frequency source at Lo) */
    uint8_t noise_on, noise_volume;
    uint8_t cutoff, emphasis, amount;
    uint8_t kbd1, kbd2;     /* KEYBOARD CONTROL 1 and 2 */
    uint8_t fadsr[3];       /* the filter contour: ATTACK DECAY SUSTAIN */
    uint8_t ladsr[3];       /* the loudness contour */
    uint8_t decay;          /* the DECAY switch */
    uint8_t glide;          /* GLIDE (0: off) */
    uint8_t mod_mix;        /* MODULATION MIX 0 .. 127: oscillator 3 .. noise */
    uint8_t osc_mod, filter_mod;
    uint8_t wheel;          /* the MODULATION wheel */
} MiniKnobs;

#endif
