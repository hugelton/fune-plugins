/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
 * Part of MINI (plugins/mini), based on CA-72 by Idle Foundry (GPL-3.0-or-later). */
/* MINI's factory presets (its own, not CA-72's: they set MINI's twenty values), as interface 1.5's KPreset takes them: LABEL=V
 * a parameter by its label (V a number or one of its names); A= D= S= R= the loudness contour, the track's ENV 1 (0 .. 100 = the
 * panel's 0 .. 10; R above 0 is the DECAY switch on). The knobs are the panel's x 12.7 (CUT 64 = 5, DCY 38 = 3). Included by the
 * plugin and by its test (mini_test.c), which plays each on both paths.
 * Times (the knob's law, CA-72's): DECAY 2 ~0.15 s, 3 ~0.22 s, 4 ~0.32 s, 5 ~0.44 s, 6 ~0.52 s, 7 ~2.4 s. CUTOFF 2 ~110 Hz,
 * 3 ~210 Hz, 4 ~480 Hz, 5 ~1.1 kHz, 6 ~2.3 kHz, 7 ~5.3 kHz (before the contour and the keys). */
#define MINI_PRESET_LIST \
    {"Round Bass", "RNG1=16' WAV1=Saw RNG2=16' WAV2=Sqr FRQ2=8 RNG3=32' WAV3=Tri FRQ3=0 OSC2=110 OSC3=80 NOIS=0 GLID=0 " \
                   "CUT=44 EMPH=51 AMT=76 KTRK=1/3 DCY=32 SUS=25 MOD=3>Pit MODW=0 A=0 D=40 S=80 R=0"}, \
    {"Glide Lead", "RNG1=8' WAV1=Saw RNG2=8' WAV2=Saw FRQ2=10 RNG3=4' WAV3=Sqr FRQ3=-8 OSC2=105 OSC3=40 NOIS=0 GLID=58 " \
                   "CUT=70 EMPH=64 AMT=51 KTRK=2/3 DCY=38 SUS=76 MOD=3>Pit MODW=0 A=0 D=50 S=90 R=30"}, \
    {"Brass", "RNG1=8' WAV1=Saw RNG2=8' WAV2=Saw FRQ2=9 RNG3=8' WAV3=Saw FRQ3=-9 OSC2=115 OSC3=100 NOIS=0 GLID=0 " \
              "CUT=38 EMPH=25 AMT=89 KTRK=2/3 DCY=51 SUS=76 MOD=3>Pit MODW=0 A=15 D=40 S=85 R=30"}, \
    {"Fat Stack", "RNG1=16' WAV1=Saw RNG2=16' WAV2=Saw FRQ2=11 RNG3=32' WAV3=Saw FRQ3=-11 OSC2=127 OSC3=120 NOIS=0 GLID=0 " \
                  "CUT=64 EMPH=38 AMT=51 KTRK=1/3 DCY=51 SUS=89 MOD=3>Pit MODW=0 A=0 D=50 S=100 R=30"}, \
    {"Pluck", "RNG1=8' WAV1=Sqr RNG2=4' WAV2=Nrrw FRQ2=4 RNG3=8' OSC2=70 OSC3=0 NOIS=0 GLID=0 " \
              "CUT=25 EMPH=76 AMT=102 KTRK=Full DCY=25 SUS=0 MOD=3>Pit MODW=0 A=0 D=25 S=0 R=25"}, \
    {"Wind", "WAV1=Off OSC2=0 OSC3=0 NOIS=120 RNG3=LFO WAV3=Tri FRQ3=-60 GLID=0 " \
             "CUT=51 EMPH=108 AMT=0 KTRK=Off DCY=64 SUS=100 MOD=3>Flt MODW=90 A=30 D=60 S=100 R=60"}, \
    {"Whistle", "WAV1=Off OSC2=0 OSC3=0 NOIS=0 CUT=16 EMPH=127 AMT=0 KTRK=Full DCY=64 SUS=127 GLID=45 MOD=3>Pit MODW=0 " \
                "A=20 D=60 S=100 R=40"}, \
    {"Wheel Lead", "RNG1=8' WAV1=Saw RNG2=8' WAV2=Sqr FRQ2=-6 RNG3=LFO WAV3=Tri FRQ3=52 OSC2=95 OSC3=0 NOIS=0 GLID=30 " \
                   "CUT=76 EMPH=51 AMT=38 KTRK=2/3 DCY=51 SUS=89 MOD=3>Pit MODW=22 A=5 D=50 S=95 R=30"},
