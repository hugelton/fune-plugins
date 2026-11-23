#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""Build the plugin catalog (catalog/): what Fune's web app shows on its Library page and installs from.

    python3 tools/catalog.py --fune /path/to/Fune [--key KEY.pem] [--out catalog] [--only acid,mini]
                             [--check /path/to/fune-plugin-check] [--kpl-from DIR] [--dev-key-file]

Every folder with a plugin.json is one plugin (README.md: the fields). For each, at the version its plugin.json says:
  kpl/<folder>-<version>.kpl          the FM-1 image, made by Fune's tools/kpl_build.py (the JieLi toolchain, Docker unless on
                                      Linux x86-64); --kpl-from DIR takes DIR/<folder>.kpl instead (made before)
  thumbs/<folder>-<version>.png       its web page (640 x 360) beside its FM-1 screen (240 x 240), from fune-plugin-check --shots --web
  previews/<folder>-<version>.opus    the 4.5 seconds fune-plugin-check plays (its defaults, notes 36 .. 96), Opus (ffmpeg)
and index.json lists them: each file with its size and SHA-256, the FM-1's numbers (flash, RAM, cost) from the image and the
record, and the plugin.json's words. A WASM module for the web is not there yet (P2): the field is left out.

Versions are never replaced: an id and version already in catalog/index.json is kept as it is, its files untouched; if the
plugin's source changed since (the "source" hash: its folder's files as git has them), the build stops: raise the version.
Old versions stay listed (the newest is what the web app shows; the others are for whoever pinned one).

Signing: with --key (or FUNE_CATALOG_KEY=path) the index is signed, ECDSA P-256 over SHA-256 of index.json's bytes, written
base64 (DER) to index.json.sig, as Rokko's bundle library does. The key never lives in this repository. Without a key there is
no .sig and Fune's web app refuses the catalog. --dev-key-file also writes dev-public-key.txt (the key's public half, base64
SPKI): Fune's web app trusts it only when served from this machine (localhost), for testing with a key of one's own.
"""
import argparse
import base64
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent.parent
FORMAT = 1
KINDS = ("synth", "drum", "effect")
DEVICES = ("fm1", "desktop", "zero", "web")
ID_RE = re.compile(r"^[ -~]{4}$")
SPDX = {"MPL-2.0", "MIT", "GPL-3.0-or-later", "GPL-2.0-or-later", "Apache-2.0", "BSD-2-Clause", "BSD-3-Clause", "ISC", "CC0-1.0"}


def fail(msg):
    sys.exit(f"catalog: {msg}")


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def file_entry(out, rel):
    b = (out / rel).read_bytes()
    return {"file": rel, "size": len(b), "sha256": sha256(b)}


# ---- plugin.json
def read_meta(folder):
    """the plugin.json of a folder, checked; returns the dict"""
    p = folder / "plugin.json"
    try:
        m = json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        fail(f"{p}: {e}")
    need = {"id": str, "name": str, "long_name": str, "version": int, "kind": str, "license": str, "author": str,
            "summary": dict, "source": str, "devices": list, "tags": list}
    for k, t in need.items():
        if not isinstance(m.get(k), t):
            fail(f"{p}: '{k}' missing or not a {t.__name__}")
    if not ID_RE.match(m["id"]):
        fail(f"{p}: id '{m['id']}' is not four printable characters (the record's uid)")
    if not 1 <= m["version"] <= 255:
        fail(f"{p}: version {m['version']} is not 1 .. 255 (the record's version)")
    if m["kind"] not in KINDS:
        fail(f"{p}: kind '{m['kind']}' is not one of {', '.join(KINDS)}")
    if m["license"] not in SPDX:
        fail(f"{p}: license '{m['license']}' is not an SPDX id this catalog knows ({', '.join(sorted(SPDX))})")
    for lang in ("en", "ja"):
        if not isinstance(m["summary"].get(lang), str) or not m["summary"][lang]:
            fail(f"{p}: summary.{lang} missing")
    if "fm1_note" in m and not all(isinstance(m["fm1_note"].get(x), str) for x in ("en", "ja")):
        fail(f"{p}: fm1_note needs en and ja")
    bad = [d for d in m["devices"] if d not in DEVICES]
    if bad or not m["devices"]:
        fail(f"{p}: devices {bad or '[]'}: each one of {', '.join(DEVICES)}")
    if not all(isinstance(t, str) and re.match(r"^[a-z0-9-]+$", t) for t in m["tags"]):
        fail(f"{p}: tags are lower-case words")
    if not (folder / m["source"]).is_file():
        fail(f"{p}: source {m['source']} is not in the folder")
    if not (folder / "LICENSE").is_file():
        fail(f"{folder}: no LICENSE in the folder")
    return m


def source_hash(folder):
    """a hash of the plugin's folder as git has it (the tracked files and their bytes as they are now); without git, every file"""
    try:
        r = subprocess.run(["git", "-C", str(folder), "ls-files", "-z", "--", "."], capture_output=True, check=True)
        files = sorted(f for f in r.stdout.decode().split("\0") if f)
    except (OSError, subprocess.CalledProcessError):
        files = []
    if not files:
        files = sorted(str(f.relative_to(folder)) for f in folder.rglob("*") if f.is_file())
    h = hashlib.sha256()
    for f in files:
        if f == "plugin.json":   # (its words may change without a new version: the summary, the tags)
            continue
        h.update(f.encode() + b"\0" + hashlib.sha256((folder / f).read_bytes()).digest())
    return h.hexdigest()


# ---- the things made
def build_so(fune, folder, meta, work):
    so = work / (folder.name + ".so")
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-shared", "-fPIC", "-O2", "-std=gnu11", "-fvisibility=hidden", "-I", str(fune / "sdk"), "-I", str(fune / "src"),
           "-I", str(folder), str(folder / meta["source"]), "-o", str(so), "-lm"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        fail(f"{folder.name}: the desktop build failed:\n{r.stderr}")
    return so


def run_check(check, so, work, name):
    js, wav, shots = work / f"{name}.json", work / f"{name}.wav", work / f"shots_{name}"
    r = subprocess.run([str(check), str(so), "--quiet", "--web", "--json", str(js), "--wav", str(wav), "--shots", str(shots)],
                       capture_output=True, text=True)
    if r.returncode:
        fail(f"{name}: fune-plugin-check refused it:\n{r.stdout}{r.stderr}")
    return json.loads(js.read_text()), wav, shots


def build_kpl(fune, folder, meta, work, kpl_from):
    out = work / (folder.name + ".kpl")
    if kpl_from:
        src = Path(kpl_from) / (folder.name + ".kpl")
        if not src.is_file():
            fail(f"{folder.name}: no {src}")
        shutil.copyfile(src, out)
        return out
    r = subprocess.run([sys.executable, str(fune / "tools/kpl_build.py"), str(folder / meta["source"]), "-o", str(out)],
                       capture_output=True, text=True)
    if r.returncode:
        fail(f"{folder.name}: kpl_build.py failed:\n{r.stdout}{r.stderr}")
    return out


def ffmpeg(*args):
    r = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", *map(str, args)], capture_output=True, text=True)
    if r.returncode:
        fail(f"ffmpeg {' '.join(map(str, args))}:\n{r.stderr}")


def make_thumb(shots, uid, dst):
    """the web page's shot (640 x 360) and the FM-1's screen (240 x 240) side by side on the app's black, 920 x 360"""
    web, fm1 = shots / f"{uid}_web_sound.png", shots / f"{uid}_fm1_sound.png"
    for f in (web, fm1):
        if not f.is_file():
            fail(f"no {f.name} from fune-plugin-check")
    ffmpeg("-i", web, "-i", fm1, "-filter_complex",
           "color=c=0x0b0b0c:s=920x360[bg];[bg][0:v]overlay=0:0[a];[a][1:v]overlay=660:60,format=rgb24", "-frames:v", 1,
           "-compression_level", 9, dst)


def make_preview(wav, dst):
    """the check's 4.5 s, faded out at the end, Opus 64 kb/s in Ogg"""
    ffmpeg("-i", wav, "-af", "afade=t=out:st=3.9:d=0.6", "-c:a", "libopus", "-b:a", "64k", "-ar", "48000",
           "-map_metadata", "-1", dst)


def kpl_header(fune, data):
    sys.path.insert(0, str(fune / "tools"))
    import kpl_build   # (Fune's: the same checks)
    try:
        h = kpl_build.parse(data)
    except ValueError as e:
        fail(f"not a .kpl as built: {e}")
    return h, kpl_build.plugin_name(data, h), kpl_build.uid_str(h["uid"])


# ---- signing (openssl: no Python packages needed)
def sign(key, data):
    with tempfile.NamedTemporaryFile(delete=False) as t:
        t.write(data)
    try:
        r = subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(key), t.name], capture_output=True)
        if r.returncode:
            fail(f"openssl could not sign with {key}: {r.stderr.decode()}")
        return base64.b64encode(r.stdout)
    finally:
        os.unlink(t.name)


def public_b64(key):
    """the key's public half, base64 SPKI DER (WebCrypto importKey('spki'))"""
    r = subprocess.run(["openssl", "pkey", "-in", str(key), "-pubout", "-outform", "DER"], capture_output=True)
    if r.returncode:
        fail(f"openssl could not read {key}: {r.stderr.decode()}")
    return base64.b64encode(r.stdout).decode()


def verify(pub_b64, data, sig_b64):
    """True if sig_b64 (base64 DER) is a valid ECDSA P-256 / SHA-256 signature of data under pub_b64 (base64 SPKI)"""
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        (d / "pub.der").write_bytes(base64.b64decode(pub_b64))
        (d / "data").write_bytes(data)
        try:
            (d / "sig").write_bytes(base64.b64decode(sig_b64, validate=True))
        except ValueError:
            return False
        r = subprocess.run(["openssl", "dgst", "-sha256", "-verify", str(d / "pub.der"), "-keyform", "DER", "-signature",
                            str(d / "sig"), str(d / "data")], capture_output=True)
        return r.returncode == 0


# ---- the catalog
def entry(fune, check, folder, meta, out, kpl_from, src_hash):
    name, ver = folder.name, meta["version"]
    stem = f"{name}-{ver}"
    with tempfile.TemporaryDirectory(prefix="fune-catalog-") as w:
        work = Path(w)
        so = build_so(fune, folder, meta, work)
        rep, wav, shots = run_check(check, so, work, name)
        pl = rep["plugin"]
        if pl["id"] != meta["id"] or pl["version"] != ver or pl["kind"] != meta["kind"]:
            fail(f"{name}: plugin.json says '{meta['id']}' v{ver} {meta['kind']}, the record '{pl['id']}' v{pl['version']} {pl['kind']}")
        kpl = build_kpl(fune, folder, meta, work, kpl_from)
        data = kpl.read_bytes()
        h, kname, kuid = kpl_header(fune, data)
        if kuid != meta["id"] or h["version"] != ver:
            fail(f"{name}: the .kpl is '{kuid}' v{h['version']}, plugin.json '{meta['id']}' v{ver} (an old --kpl-from?)")
        for sub in ("kpl", "thumbs", "previews"):
            (out / sub).mkdir(parents=True, exist_ok=True)
        rel = {"kpl": f"kpl/{stem}.kpl", "thumb": f"thumbs/{stem}.png", "preview": f"previews/{stem}.opus"}
        shutil.copyfile(kpl, out / rel["kpl"])
        make_thumb(shots, meta["id"], out / rel["thumb"])
        make_preview(wav, out / rel["preview"])
    e = {k: meta[k] for k in ("id", "version", "name", "long_name", "kind", "license", "author")}
    if meta.get("credits"):
        e["credits"] = meta["credits"]
    e["folder"] = name
    e["summary"] = meta["summary"]
    e["devices"] = meta["devices"]
    e["tags"] = meta["tags"]
    e["fm1"] = {**file_entry(out, rel["kpl"]), "flash": (h["size"] + 4095) // 4096 * 4096, "ram": h["data_size"] + h["bss_size"],
                "cost": pl["cost"], "interface": f"{h['api'] >> 8}.{h['api'] & 255}"}
    if meta.get("fm1_note"):
        e["fm1"]["note"] = meta["fm1_note"]
    # "web": {file, size, sha256} of a WASM module comes with P2 (the web app cannot run outside engines yet)
    e["thumb"] = file_entry(out, rel["thumb"])
    e["preview"] = {**file_entry(out, rel["preview"]), "seconds": 4.5}
    e["source"] = src_hash
    return e


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--fune", required=True, help="a Fune checkout (sdk/, src/, tools/kpl_build.py)")
    ap.add_argument("--check", help="fune-plugin-check (default FUNE/build/fune-plugin-check)")
    ap.add_argument("--out", default=str(HERE / "catalog"), help="the catalog folder (default catalog/)")
    ap.add_argument("--plugins", default=str(HERE), help="the folder of plugin folders (default this repository)")
    ap.add_argument("--only", help="these folders only (comma separated); the others keep what the index has")
    ap.add_argument("--kpl-from", help="take DIR/<folder>.kpl instead of building it")
    ap.add_argument("--key", default=os.environ.get("FUNE_CATALOG_KEY"), help="the signing key (PEM, EC P-256); or FUNE_CATALOG_KEY")
    ap.add_argument("--dev-key-file", action="store_true", help="also write dev-public-key.txt (local testing only)")
    ap.add_argument("--serial", type=int, help="the index's serial (default the time; must rise)")
    a = ap.parse_args()

    fune = Path(a.fune).resolve()
    if not (fune / "sdk/kreisel_sdk.h").is_file() or not (fune / "tools/kpl_build.py").is_file():
        fail(f"{fune} is not a Fune checkout")
    check = Path(a.check) if a.check else fune / "build/fune-plugin-check"
    if not check.is_file():
        fail(f"no fune-plugin-check at {check} (cmake --build build --target fune_plugin_check in Fune, or --check)")
    if not shutil.which("ffmpeg"):
        fail("no ffmpeg (the thumbnails and previews)")
    out, plugdir = Path(a.out).resolve(), Path(a.plugins).resolve()
    out.mkdir(parents=True, exist_ok=True)
    old = {}
    old_serial = 0
    if (out / "index.json").is_file():
        prev = json.loads((out / "index.json").read_text(encoding="utf-8"))
        old_serial = prev.get("serial", 0)
        for e in prev.get("plugins", []):
            old[(e["id"], e["version"])] = e

    only = set(a.only.split(",")) if a.only else None
    plugins, seen = [], set()
    for folder in sorted(p for p in plugdir.iterdir() if (p / "plugin.json").is_file()):
        meta = read_meta(folder)
        key = (meta["id"], meta["version"])
        if meta["id"] in seen:
            fail(f"{folder.name}: id '{meta['id']}' is used by another folder")
        seen.add(meta["id"])
        src = source_hash(folder)
        if key in old:
            e = old[key]
            if e["source"] != src:
                fail(f"{folder.name}: '{meta['id']}' v{meta['version']} is in the catalog already and its source changed since: raise "
                     "the version (versions are never replaced)")
            for f in (e["fm1"], e["thumb"], e["preview"]):
                p = out / f["file"]
                if not p.is_file() or sha256(p.read_bytes()) != f["sha256"]:
                    fail(f"{p}: missing or not what the index says (a published file is never replaced)")
            # its words may be corrected without a new version; the files stay
            for k in ("name", "long_name", "license", "author", "summary", "devices", "tags"):
                e[k] = meta[k]
            if meta.get("credits"):
                e["credits"] = meta["credits"]
            print(f"  {folder.name} v{meta['version']}: kept")
            plugins.append(e)
        elif only is not None and folder.name not in only:
            print(f"  {folder.name}: skipped (--only)")
        else:
            print(f"  {folder.name} v{meta['version']}: building", flush=True)
            plugins.append(entry(fune, check, folder, meta, out, a.kpl_from, src))
    # older versions stay listed after the newest
    have = {(e["id"], e["version"]) for e in plugins}
    plugins += [e for k, e in old.items() if k not in have]
    plugins.sort(key=lambda e: (e["folder"], -e["version"]))

    serial = a.serial if a.serial is not None else int(time.time())
    if serial <= old_serial:
        fail(f"serial {serial} is not above the index's {old_serial}")
    index = {"format": FORMAT, "serial": serial, "plugins": plugins}
    data = (json.dumps(index, ensure_ascii=False, indent=2) + "\n").encode("utf-8")
    (out / "index.json").write_bytes(data)
    sig = out / "index.json.sig"
    if a.key:
        sig.write_bytes(sign(a.key, data))
        if not verify(public_b64(a.key), data, sig.read_bytes()):
            fail("the signature just made does not verify")
        dev = out / "dev-public-key.txt"
        if a.dev_key_file:
            dev.write_text(public_b64(a.key) + "\n")
        elif dev.exists():
            dev.unlink()
        print(f"catalog: {out}/index.json, {len(plugins)} entries, serial {serial}, signed")
    else:
        if sig.exists():
            sig.unlink()
        print(f"catalog: {out}/index.json, {len(plugins)} entries, serial {serial}, NOT signed (no --key): the web app refuses it")


if __name__ == "__main__":
    main()
