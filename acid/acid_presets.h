/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Leo Kuroshita (@kurogedelic), Hugelton Instruments */
/* ACID's factory presets (acid.c; the dev tools read them too), in the host's factory words (src/presets_factory.c): LABEL=V over
 * the engine's defaults, A= D= S= R= ENV 1 (0 .. 100): a gate, its release long enough for an accent's. Names of our own. */
#ifndef ACID_PRESETS_H
#define ACID_PRESETS_H
static const KPreset ACID_PRESETS[] = {   /* (KPreset: KPluginExt version 2, interface 1.5) */
    {"Squelch", "WAVE=Saw CUT=36 RES=100 ENV=70 DCY=80 ACC=90 A=0 D=50 S=100 R=20"},
    {"Round Bass", "WAVE=Saw CUT=30 RES=40 ENV=25 DCY=78 ACC=50 A=0 D=50 S=100 R=20"},
    {"Square Pluck", "WAVE=Sqr CUT=44 RES=80 ENV=55 DCY=74 ACC=80 A=0 D=50 S=100 R=20"},
    {"Long Sweep", "WAVE=Saw CUT=20 RES=110 ENV=110 DCY=100 ACC=100 A=0 D=50 S=100 R=20"},
    {"Rubber", "WAVE=Sqr CUT=26 RES=118 ENV=80 DCY=86 ACC=110 SLID=66 A=0 D=50 S=100 R=20"},
    {"Hot Line", "WAVE=Saw CUT=56 RES=96 ENV=64 DCY=82 ACC=96 DRV=70 A=0 D=50 S=100 R=20"},
    {"Dark Sub", "WAVE=Sqr CUT=8 RES=30 ENV=16 DCY=90 ACC=40 A=0 D=50 S=100 R=20"},
    {"Screamer", "WAVE=Saw CUT=70 RES=127 ENV=90 DCY=76 ACC=127 DRV=100 A=0 D=50 S=100 R=20"},
};

#endif
