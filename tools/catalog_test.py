#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""tools/catalog.py on a small plugin (Fune's BEEP example, its .kpl from Fune's web fixtures: no FM-1 toolchain needed):

    python3 tools/catalog_test.py --fune /path/to/Fune [--check /path/to/fune-plugin-check]

- the index: its fields, each file's size and SHA-256 as the file is;
- the signature: verifies with the key's public half (openssl), fails for a changed index and for another key;
- a second run keeps the entry and its files, the serial rises; a serial that does not rise is refused;
- the source changed, the version not: refused (versions are never replaced); a version the record does not say: refused;
- no key: no .sig; a plugin.json missing a field: refused."""
import argparse
import base64
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import catalog  # noqa: E402

fails = 0


def check(ok, what):
    global fails
    print(("ok   " if ok else "FAIL ") + what)
    if not ok:
        fails += 1


def run(args, expect_ok=True):
    r = subprocess.run([sys.executable, str(HERE / "catalog.py"), *args], capture_output=True, text=True)
    return r


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fune", default=os.environ.get("FUNE_DIR"))
    ap.add_argument("--check")
    a = ap.parse_args()
    if not a.fune:
        sys.exit("catalog_test: --fune /path/to/Fune (or FUNE_DIR)")
    fune = Path(a.fune).resolve()
    chk = Path(a.check) if a.check else fune / "build/fune-plugin-check"
    if not chk.is_file():
        print(f"catalog_test: SKIPPED (no fune-plugin-check at {chk})")
        return 0
    with tempfile.TemporaryDirectory(prefix="fune-catalog-test-") as t:
        t = Path(t)
        plugs, out, kpls = t / "plugins", t / "catalog", t / "kpl"
        (plugs / "beep").mkdir(parents=True)
        kpls.mkdir()
        shutil.copyfile(fune / "plugins/beep/beep.c", plugs / "beep/beep.c")
        shutil.copyfile(fune / "LICENSE", plugs / "beep/LICENSE")
        shutil.copyfile(fune / "platforms/web/fixtures/beep.kpl", kpls / "beep.kpl")
        meta = {"id": "beep", "name": "Beep", "long_name": "Beep example", "version": 1, "kind": "synth", "license": "MPL-2.0",
                "author": "Test", "summary": {"en": "A test", "ja": "テスト"}, "source": "beep.c", "devices": ["fm1", "desktop"],
                "tags": ["test"]}
        (plugs / "beep/plugin.json").write_text(json.dumps(meta, ensure_ascii=False))
        key, key2 = t / "k.pem", t / "k2.pem"
        for k in (key, key2):
            subprocess.run(["openssl", "genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out", str(k)], check=True,
                           capture_output=True)
        base = ["--fune", str(fune), "--check", str(chk), "--plugins", str(plugs), "--out", str(out), "--kpl-from", str(kpls)]

        r = run(base + ["--key", str(key), "--serial", "100"])
        check(r.returncode == 0, "builds a catalog of one plugin" + ("" if r.returncode == 0 else ": " + r.stderr + r.stdout))
        if r.returncode:
            return 1
        data = (out / "index.json").read_bytes()
        ix = json.loads(data)
        check(ix["format"] == 1 and ix["serial"] == 100 and len(ix["plugins"]) == 1, "index: format 1, the serial, one entry")
        e = ix["plugins"][0]
        check(all(e[k] == meta[k] for k in ("id", "version", "name", "long_name", "kind", "license", "author", "summary", "devices",
                                               "tags")), "the entry carries plugin.json's words")
        check(e["fm1"]["file"] == "kpl/beep-1.kpl" and e["thumb"]["file"] == "thumbs/beep-1.png" and e["preview"]["file"] == "previews/beep-1.opus",
              "files named <folder>-<version>")
        check("web" not in e, "no web (WASM) field before P2")
        for part in ("fm1", "thumb", "preview"):
            b = (out / e[part]["file"]).read_bytes()
            check(len(b) == e[part]["size"] and hashlib.sha256(b).hexdigest() == e[part]["sha256"], f"{part}: size and SHA-256 as the file is")
        check((out / e["fm1"]["file"]).read_bytes() == (kpls / "beep.kpl").read_bytes(), "the .kpl is the one built, byte for byte")
        check(e["fm1"]["flash"] == 4096 and e["fm1"]["ram"] == 0 and e["fm1"]["interface"], "FM-1 numbers from the image: one sector, no RAM")
        check((out / e["thumb"]["file"]).read_bytes()[:8] == b"\x89PNG\r\n\x1a\n", "the thumbnail is a PNG")
        check(b"OpusHead" in (out / e["preview"]["file"]).read_bytes()[:200], "the preview is Ogg Opus")

        sig = (out / "index.json.sig").read_bytes()
        pub, pub2 = catalog.public_b64(key), catalog.public_b64(key2)
        check(catalog.verify(pub, data, sig), "the signature verifies with the key's public half")
        check(not catalog.verify(pub, data.replace(b'"serial": 100', b'"serial": 101'), sig), "a changed index does not")
        check(not catalog.verify(pub2, data, sig), "another key does not")
        check(len(base64.b64decode(pub)) == 91, "the public key is SPKI DER (91 bytes, P-256)")
        check(not (out / "dev-public-key.txt").exists(), "no dev-public-key.txt unless asked")

        before = {p: p.read_bytes() for p in out.rglob("*") if p.is_file() and p.suffix != ".json" and p.suffix != ".sig"}
        r = run(base + ["--key", str(key), "--serial", "101"])
        check(r.returncode == 0 and "kept" in r.stdout, "a second run keeps the entry")
        check(all(p.read_bytes() == b for p, b in before.items()), "and its files")
        check(json.loads((out / "index.json").read_text())["serial"] == 101, "the serial rises")
        r = run(base + ["--key", str(key), "--serial", "101"])
        check(r.returncode != 0 and "not above" in r.stderr, "a serial that does not rise is refused")

        (plugs / "beep/plugin.json").write_text(json.dumps({**meta, "tags": ["test", "beep"]}))
        r = run(base + ["--key", str(key), "--serial", "102"])
        check(r.returncode == 0 and json.loads((out / "index.json").read_text())["plugins"][0]["tags"] == ["test", "beep"],
              "plugin.json's words change without a new version")

        (plugs / "beep/beep.c").write_text((plugs / "beep/beep.c").read_text() + "\n/* changed */\n")
        r = run(base + ["--key", str(key), "--serial", "103"])
        check(r.returncode != 0 and "raise the version" in r.stderr, "source changed, same version: refused")

        (plugs / "beep/plugin.json").write_text(json.dumps({**meta, "version": 2}))
        r = run(base + ["--key", str(key), "--serial", "104"])
        check(r.returncode != 0 and "v2" in r.stderr, "a version the record does not say: refused")

        (plugs / "beep/plugin.json").write_text(json.dumps({k: v for k, v in meta.items() if k != "license"}))
        r = run(base + ["--serial", "105"])
        check(r.returncode != 0 and "license" in r.stderr, "plugin.json without a licence: refused")

        (plugs / "beep/plugin.json").write_text(json.dumps(meta))
        shutil.copyfile(fune / "plugins/beep/beep.c", plugs / "beep/beep.c")
        r = run(base + ["--serial", "106"])
        check(r.returncode == 0 and "NOT signed" in r.stdout and not (out / "index.json.sig").exists(), "no key: no .sig")
        r = run(base + ["--key", str(key), "--dev-key-file", "--serial", "107"])
        check((out / "dev-public-key.txt").read_text().strip() == pub, "--dev-key-file writes the public half")

        (out / e["fm1"]["file"]).write_bytes(b"x" + (out / e["fm1"]["file"]).read_bytes()[1:])
        r = run(base + ["--key", str(key), "--serial", "108"])
        check(r.returncode != 0 and "never replaced" in r.stderr, "a published file changed on disk: refused")
    print("catalog_test: " + ("all ok" if not fails else f"{fails} FAILED"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
