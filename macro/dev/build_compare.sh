#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
# A dev tool: builds plaits_compare (Plaits' float engines beside Macro's integer models) into OUT (default build/macro_dev).
#   plugins/macro/dev/build_compare.sh /path/to/eurorack [OUT]
# Plaits is built at 44.1 kHz: its plaits/dsp/dsp.h is copied into OUT with the corrected rate set to 44100 (the original is
# left as it is), and put first on the include path.
set -e
ER=${1:?the eurorack clone (https://github.com/pichenettes/eurorack, with its stmlib submodule)}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
OUT=${2:-$ROOT/build/macro_dev}
mkdir -p "$OUT/shadow/plaits/dsp"
sed 's/kCorrectedSampleRate = 47872.34f/kCorrectedSampleRate = 44100.0f/' "$ER/plaits/dsp/dsp.h" > "$OUT/shadow/plaits/dsp/dsp.h"
cc -std=c99 -O2 -c -I"$ROOT/sdk" -I"$ROOT/src" "$HERE/macro_int.c" -o "$OUT/macro_int.o"
P="$ER/plaits"
c++ -std=c++14 -O2 -w -I"$OUT/shadow" -I"$ER" -DTEST \
  "$HERE/plaits_compare.cc" "$P/resources.cc" "$P/dsp/chords/chord_bank.cc" \
  "$P/dsp/engine/virtual_analog_engine.cc" "$P/dsp/engine/waveshaping_engine.cc" "$P/dsp/engine/fm_engine.cc" \
  "$P/dsp/engine/grain_engine.cc" "$P/dsp/engine/additive_engine.cc" "$P/dsp/engine/wavetable_engine.cc" \
  "$P/dsp/engine/chord_engine.cc" "$P/dsp/engine/noise_engine.cc" \
  "$ER/stmlib/dsp/units.cc" "$ER/stmlib/utils/random.cc" "$OUT/macro_int.o" -o "$OUT/plaits_compare"
echo "$OUT/plaits_compare"
# the WAV writer: Fune's audio path (build/libkreisel_core.a, from `cmake --build build`) with the plugin linked in
cc -std=c99 -O2 -c -I"$ROOT/sdk" -I"$ROOT/src" -Dkreisel_plugin=macro_plugin "$ROOT/plugins/macro/macro.c" -o "$OUT/macro_linked.o"
cc -std=c99 -O2 -I"$ROOT/src" -I"$ROOT/build/gen" "$HERE/macro_wavs.c" "$OUT/macro_linked.o" "$ROOT/build/libkreisel_core.a" "$ROOT/build/libkreisel_sample.a" "$ROOT/build/libkreisel_core.a" -lm -o "$OUT/macro_wavs"
echo "$OUT/macro_wavs"
# the screenshots: the app's UI code (build/libkreisel_ui.a) with the plugin linked in
cc -std=c99 -O2 -I"$ROOT/src" -I"$ROOT/src/ui" -I"$ROOT/build/gen" "$HERE/macro_shots.c" "$OUT/macro_linked.o" \
  "$ROOT/build/libkreisel_ui.a" "$ROOT/build/libkreisel_core.a" "$ROOT/build/libkreisel_sample.a" "$ROOT/build/libkreisel_core.a" -lm -o "$OUT/macro_shots"
echo "$OUT/macro_shots"
