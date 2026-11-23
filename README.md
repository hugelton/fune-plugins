# Fune plugins

The official engine and effect plugins for [Fune](https://github.com/hugelton/Fune), a small music studio for multiple platforms.
Fune itself stays small; sounds come in as plugins, so nobody needs to fork the firmware to add one.

Each folder is one plugin and carries **its own licence** (`LICENSE` in the folder, SPDX headers in its files). Fune's core is
MPL-2.0; a plugin here may be MPL-2.0, MIT or GPL-3.0-or-later, as its folder says.

| Plugin | Kind | Licence | Devices |
| --- | --- | --- | --- |
| [Ensemble](ensemble/) | synth: divide-down strings with a chorus | MPL-2.0 | all (FM-1 too) |
| [Vector](vector/) | synth: four wavetables mixed by X / Y | MPL-2.0 | all (FM-1 too) |
| [E.Piano](epiano/) | synth: an electric piano (two-operator tines, bark, tremolo) | MPL-2.0 | all (FM-1 too) |
| [Macro](macro/) | synth: eight synthesis models after Mutable Instruments Plaits | MIT (after Plaits, MIT) | all (FM-1 too; Wave and Harm cost about twice a Retro voice) |
| [Mini](mini/) | synth: a three-oscillator mono synth of the early 1970s, drawn as its panel | GPL-3.0-or-later (based on CA-72 by Idle Foundry) | all (FM-1 too) |
| [Acid](acid/) | synth: a mono bass line with accent and slide | MIT (based on Open303 by Robin Schmidt) | all (FM-1 too) |

## Building

The plugins build against Fune's SDK (`sdk/` and `src/` of a Fune checkout).

```bash
cmake -S . -B build -DFUNE_DIR=/path/to/Fune && cmake --build build      # desktop shared libraries in build/plugins/
python3 /path/to/Fune/tools/kpl_build.py ensemble/ensemble.c              # an FM-1 image (.kpl)
```

Inside a Fune checkout this repository sits at `extern/fune-plugins` (a submodule) and Fune's build finds it there.

## Adding one

One folder, one C file (plus its tables), the SDK's rules (`docs/PLUGIN_SDK.md` in Fune): integers only so it runs on the FM-1,
no allocations, the host draws the screens. Put the licence in the folder. Names: your own; no product names or logos of others.

## The catalog

Fune's web app has a Library page that lists these plugins and installs them onto a connected FM-1, from a signed catalog built
here: `catalog/` (`index.json`, `index.json.sig`, `kpl/`, `thumbs/`, `previews/`), the same shape as Rokko's bundle library.

To add a plugin to it, give its folder a `plugin.json`:

```json
{
  "id": "acid", "name": "Acid", "long_name": "Acid bass line", "version": 1, "kind": "synth", "license": "MIT",
  "author": "...", "credits": "Based on Open303 by Robin Schmidt (MIT)",
  "summary": { "en": "A mono bass line with accent and slide", "ja": "..." },
  "source": "acid.c", "devices": ["fm1", "desktop", "zero"], "tags": ["mono", "bass"]
}
```

`id` is the record's uid (four characters), `version` the record's version (1 .. 255: raise it with every change of the code
or the sound), `kind` synth, drum or effect, `license` SPDX (as the folder's `LICENSE`), `credits` optional, `source` the C file
`kpl_build.py` builds, `devices` of fm1, desktop, zero, web; optional `fm1_note` {en, ja}: a cost worth saying.

Then build the catalog:

```bash
python3 tools/catalog.py --fune /path/to/Fune --key /path/outside/any/repo/catalog-key.pem
```

It builds each plugin for the desktop, runs Fune's `fune-plugin-check` on it (it must pass; its screenshots make the picture,
its 4.5 s of sound the Opus preview), builds the FM-1 image with Fune's `tools/kpl_build.py` (the FM-1 toolchain in Docker;
`--kpl-from DIR` takes images made before), checks that plugin.json, the record and the image agree, and writes each file with
its size and SHA-256 into `index.json`, signed (ECDSA P-256, base64 DER in `index.json.sig`). Needs ffmpeg and openssl.

- **Versions are never replaced.** An id and version already in the index keep their files; if its source changed, the tool
  stops: raise the version. Older versions stay listed.
- **The key never comes here.** Pass it by path (`--key` or `FUNE_CATALOG_KEY`); Fune's web app holds only its public half.
  For testing, a key of your own and `--dev-key-file` (Fune's web app trusts that file only when served from localhost).
- `catalog/` is not committed until the catalog is first published (`.gitignore`).
- Tests: `python3 tools/catalog_test.py --fune /path/to/Fune`.
