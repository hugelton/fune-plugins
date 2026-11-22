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
| (coming: Macro, Mini, Acid) | | | |

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
