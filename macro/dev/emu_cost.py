#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""A dev tool: Macro's cost on the FM-1, counted by fm1-emu (docs/AUDIO.md "Cost on the FM-1": instructions, not cycles).

    emu_cost.py FM1_EMU kreisel.bin WORKDIR flash_0.bin .. flash_7.bin [--retro]

kreisel.bin: the FM-1 app built with KREISEL_FM1_BOOTCHECK=0 (for the emulator only). flash_<m>.bin: emu_flash.py's flash with a
build of Macro whose defaults are model m and DCAY 127 (the gate open: the voices sound the whole time, the worst case): no knob
is turned in the emulator (a page key pressed twice opens its tools). Each run: SCL (SYNTH), ALGORITHM to the last engine
(Macro), six keys held from HOLD0 to HOLD1 ms (five voices sound: fm1-emu 0.4.0 plays the sixth key not, a fifth and a sixth key held
cost the same, 2026-11-23); a silent run is the same without the keys. --retro: the first engine (Retro, as
it starts), the same keys, for a reference. The figure: the ISR instructions per 256-frame block the keys add, and a
voice's share (of SOUNDING); WORKDIR/<model>.bmp the screen while it plays."""
import re
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

MODELS = ["Pair", "Fold", "FM", "Grain", "Harm", "Wave", "Chord", "Noise"]
RUN, HOLD0, HOLD1 = 5200, 2600, 4800
BLOCKS_A_S = 44100 / 256
SOUNDING = 5   # (the voices the six keys sound)


def keys(play, engine_steps=16):
    k = ["300:SCL"] + [f"{500 + i * 100}:ALGORITHM:+1" for i in range(engine_steps)]
    if play:
        k += [f"{HOLD0}-{HOLD1}:k{n}" for n in (12, 14, 16, 17, 19, 21)]
    return ",".join(k)


def run(emu, binf, flash, work, play, steps=16):
    d = Path(tempfile.mkdtemp(dir=work))
    (d / "work").mkdir()
    snap = (HOLD0 + HOLD1) // 2
    cmd = [emu, "--bin", binf, "--entry", "0x02000120", "--flash", flash, "--ms", str(RUN), "--steps", "4000000000",
           "--keys", keys(play, steps), "--snap", str(snap)]
    out = subprocess.run(cmd, cwd=d, capture_output=True, text=True).stdout
    m = re.search(r"\(([\d.]+) M ISR instructions per emulated s\)", out)
    if not m:
        sys.exit("no ISR figure:\n" + out[-2000:])
    return float(m.group(1)), d / "work" / f"lcd_{snap}.bmp"


def per_block(silent, six):
    return (six - silent) * 1e6 * (RUN / 1000) / ((HOLD1 - HOLD0) / 1000 * BLOCKS_A_S)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) != 11:
        sys.exit(__doc__)
    emu, binf, work = (str(Path(a).resolve()) for a in args[:3])
    flashes = [str(Path(a).resolve()) for a in args[3:]]
    jobs = [("silent", flashes[0], False, 16)] + [(MODELS[i], flashes[i], True, 16) for i in range(8)]
    if "--retro" in sys.argv:
        jobs += [("Retro silent", flashes[0], False, 0), ("Retro", flashes[0], True, 0)]
    with ThreadPoolExecutor(max_workers=4) as ex:
        res = dict(zip([j[0] for j in jobs], ex.map(lambda j: run(emu, binf, j[1], work, j[2], j[3]), jobs)))
    silent = res["silent"][0]
    print(f"{'':14s} M ISR/s   six keys k instr / block   a voice k / block")
    print(f"{'(silent)':14s} {silent:7.1f}")
    for name in MODELS + (["Retro"] if "--retro" in sys.argv else []):
        base = res["Retro silent"][0] if name == "Retro" else silent
        v, bmp = res[name]
        pb = per_block(base, v)
        print(f"{name:14s} {v:7.1f}   {pb / 1000:10.0f}                {pb / SOUNDING / 1000:8.1f}")
        if bmp.exists():
            (Path(work) / f"{name.lower()}.bmp").write_bytes(bmp.read_bytes())


if __name__ == "__main__":
    main()
