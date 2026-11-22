#!/bin/sh
# SPDX-License-Identifier: MIT
# Builds the dev harness (compare.cpp) beside Open303's own sources, which stay in their clone (OPEN303, outside the repository).
#   OPEN303=/path/to/Open303 sh plugins/acid/dev/build.sh   ->   plugins/acid/dev/build/acid_compare
set -e
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../../.." && pwd)
o3=${OPEN303:-/Volumes/SSD/MacData/open303}/Source/DSPCode
out=$here/build
mkdir -p "$out"
cc -O2 -std=c11 -c "$here/acid_devhost.c" -I"$repo/sdk" -I"$repo/src" -I"$here/.." -o "$out/acid_devhost.o"
srcs=""
for f in GlobalFunctions rosic_AnalogEnvelope rosic_BiquadFilter rosic_BlendOscillator rosic_Complex rosic_DecayEnvelope \
         rosic_EllipticQuarterBandFilter rosic_FourierTransformerRadix2 rosic_FunctionTemplates rosic_LeakyIntegrator \
         rosic_MidiNoteEvent rosic_MipMappedWaveTable rosic_NumberManipulations rosic_OnePoleFilter rosic_Open303 \
         rosic_RealFunctions rosic_TeeBeeFilter rosic_AcidPattern rosic_AcidSequencer; do
  srcs="$srcs $o3/$f.cpp"
done
c++ -O2 -std=c++14 -w -I"$o3" -I"$here" $srcs "$here/compare.cpp" "$out/acid_devhost.o" -o "$out/acid_compare"
echo "$out/acid_compare"
