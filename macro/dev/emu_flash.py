#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""A dev tool: a flash file for fm1-emu (--flash) with the FM-1 app and plugin images in place, as an install would leave them.

    emu_flash.py XIP.bin OUT.bin a.kpl [b.kpl ...]

XIP.bin: the package's XIP window as fm1-emu saves it (FM1_FWSC_XIP=XIP.bin fm1-emu --fwsc build/fm1/kreisel.fwsc --ms 100),
put at flash 0x4000 (the app at 0x4120, XIP 0x02000120); each .kpl linked (platforms/fm1hw/fm1_plugin.py's link) one after another from flash
0x097000 on 4 KiB sectors, XIP = flash - 0x4000 + 0x02000000, their RAM one after another from the pool at 0x01C08000. Nothing
here touches a device."""
import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("fm1_plugin", ROOT / "platforms/fm1hw/fm1_plugin.py")
fp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fp)

FLASH, XIP_AT, REGION, REGION_END, POOL = 0x100000, 0x4000, 0x097000, 0x0E0000, 0x01C08000


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    flash = bytearray(b"\xff" * FLASH)
    xip = Path(sys.argv[1]).read_bytes()
    flash[XIP_AT:XIP_AT + len(xip)] = xip
    at, ram = REGION, POOL
    for path in sys.argv[3:]:
        data, h = fp.load(path)
        need = h["data_size"] + h["bss_size"]
        img = fp.link(data, h, at - 0x4000 + 0x02000000, ram if need else 0)
        if at + len(img) > REGION_END:
            sys.exit(f"{path}: past the plugin region")
        flash[at:at + len(img)] = img
        print(f"{path}: flash {at:#08x} ({len(img)} bytes), ram {ram:#010x} ({need} bytes)")
        at += (len(img) + 0xFFF) & ~0xFFF
        ram += (need + 15) & ~15
    Path(sys.argv[2]).write_bytes(flash)
    print(f"{sys.argv[2]}: plugins {REGION:#x} .. {at:#x} of {REGION_END:#x}")


if __name__ == "__main__":
    main()
