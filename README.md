# Forza Horizon 2 Recompiled

A static recompilation of the Xbox 360 version of Forza Horizon 2 for Linux, built
on [ReXGlue](https://github.com/rexglue/rexglue-sdk) with a custom Vulkan renderer.
The game boots, saves, and is playable from start to finish of a race at 60 fps and
up to 4x the original resolution.

This repository contains no game data, no generated (recompiled) code and no
binaries. You need your own copy of the game; the recompiled code is generated
locally from it during the build.

## Status

Tested on Arch Linux, AMD Ryzen 9 3950X, Radeon RX 6800 XT (Mesa RADV).

Works:

- Boot, profile and save game creation, menus, free roam, races.
- 60 fps (the console runs at 30), 1x-4x internal resolution, anisotropic
  filtering, optional motion blur, AMD CAS / FSR output filtering.
- In-game settings menu (F1).

Known issues:

- With a full grid at a race start (~5,500 draws per frame) the renderer's draw
  thread is close to its frame budget; expect a few late frames for the first
  seconds of a race, then a steady 60 fps.
- Loading screens update about once a second while the game loads.
- Only the Linux build is maintained. The Windows and macOS presets come from the
  original project and the SDK and have not been tried since the Linux port.

## How it works

1. **Codegen** (ReXGlue): translates the PowerPC code of `default.xex` and its two
   helper modules into C++ (~475,000 functions, in `generated/`). Run once per
   machine, from your own game files.
2. **Runtime** (`rexglue-sdk/`, vendored and modified): implements the Xbox 360
   kernel and system interfaces the game calls -- threads, synchronization, files,
   memory, saves, input, audio -- plus the GPU command processor front end.
   Large parts derive from [Xenia](https://xenia.jp).
3. **Renderer** (`src/gfx/vk`, built as the `librexgpu-fh2.so` plugin): turns the
   game's Xenos GPU command stream into Vulkan. It started as a fork of the SDK's
   Xenia-derived Vulkan backend and was rebuilt for this game: render targets
   without full EDRAM emulation, resolves straight into textures, predicated-tiling
   strips merged into full-frame passes, a parser / draw / submission thread
   pipeline, and FH2-specific graphics options.
4. **Game hooks** (`src/forzahorizon2_*.cpp`): hand-written replacements for
   specific game functions -- save mounting, crash guards, the settings menu,
   native `pow()`, and debugging tools.

## Requirements

- Linux on x86-64 (x86-64-v3 / AVX2 for the renderer plugin)
- A Vulkan 1.3 GPU and driver (developed on Mesa RADV)
- CMake 3.25+, Ninja, Clang, ccache
- The usual development headers for building SDL (X11 and/or Wayland)
- About 3 GB of disk for the build, plus the game
- A legally obtained, extracted Forza Horizon 2 (Xbox 360) game tree

## Game files

The build and `scripts/play.sh` look for the game in a folder named `game` **next
to** this repository (see `forzahorizon2_manifest.toml`):

```text
recomps/
  game/                     <- your extracted game
    default.xex
    XMediaFacade_default.xex
    SpeechFacade_default.xex
    media/ ...
  Forza-Horizon-2-Recomp/   <- this repository
```

`scripts/play.sh` also accepts `FH2_GAME_DIR=/path/to/game`. Codegen reads the
paths in `forzahorizon2_manifest.toml`; edit them if your game lives elsewhere.

## Build

```sh
cmake --preset linux-amd64-release
cmake --build --preset linux-amd64-release --target forzahorizon2_codegen
cmake --build --preset linux-amd64-release --target forzahorizon2 rexgpu-fh2
```

The codegen step writes the recompiled sources to `generated/` (several hundred
MB). The last step builds the game executable, the runtime and the renderer
plugin into `out/build/linux-amd64-release/`. A full build takes a few minutes on
a 16-core machine; after that, changes to the renderer rebuild in under a minute.

## Play

```sh
scripts/play.sh
```

Saves, settings, the shader cache, screenshots and `play.log` go to `play/` in
the repository (override with `FH2_PLAY_DIR`). The first launch builds the shader
cache, so expect hitches the first time each new area or car is seen.

| Key | Action |
| --- | --- |
| F1 | Settings menu |
| F3 | Frame statistics overlay |
| F4 | Raw list of every runtime setting (cvar) |
| F11 | Report a visual glitch: screenshot + renderer log of the next frames |
| F12 | Screenshot (`play/screenshots/*.ppm`) |

Settings changed in the F1 menu are saved to `play/forzahorizon2.toml`. Options
passed on the command line (`scripts/play.sh --flag=value`) win over the file for
that run. `WINDOWED=1 scripts/play.sh` runs in a window.

A controller is recommended. Keyboard and mouse emulation can be enabled in F1 >
Controls (rebind keys in F4 > Input > Keybinds).

## Renderer switches

The renderer takes shortcuts that trade exactness for speed. Each can be switched
from F1 > Troubleshooting while playing (not saved), or set as an environment
variable for a whole run, e.g. `FH2_DIRECT_CUBE=0 scripts/play.sh`.

| Variable | Default | Effect |
| --- | --- | --- |
| `FH2_FULL_FRAME_STRIPS` | 2 | 2: draw repeated tiling-strip draws once over the whole frame; 1: per strip |
| `FH2_DEDUP_DEPTH_MASKS` | 1 | Also dedup depth-only mask draws (needed for correct in-car view with strips=2) |
| `FH2_DIRECT_RESOLVE` | 1 | Resolve render targets straight into the textures that sample them |
| `FH2_RESOLVE_SKIP_MEMORY` | 1 | Skip the guest memory copy for direct resolves |
| `FH2_DIRECT_CUBE` | 1 | Direct resolves for environment cube map faces |
| `FH2_TRANSFER_ALL` | 0 | 1: also copy EDRAM between render targets of different formats |
| `FH2_MOTION_BLUR` | 0 | Camera motion blur (also in F1 > Graphics) |
| `FH2_TEXTURE_SET_REUSE`, `FH2_DEFER_DESCRIPTOR_WRITES`, `FH2_SAMPLER_MEMO` | 1 | Draw-thread optimizations |
| `FH2_HOST_POW` | 1 | Native `pow()` instead of the recompiled one (2: validate both) |

If something renders wrongly, open F1 > Troubleshooting before a race, press
"Turn all off (most exact)" and check whether it goes away; then re-enable the
switches to find which one is responsible, and press F11 when you see it.

## Development

- `scripts/test-run.sh <name> <seconds>` starts a fresh save (the opening drive
  plays by itself) in a window and logs frame statistics to `testrun/<name>.log`:
  `[FPS]`, `[PRESENT]` (late frames per 5 s), `[SPIKE]` (each slow frame and what
  it waited on), `[RTSTAT]` and `[DRAWTHREAD]` (renderer cost per frame and per
  draw).
- `scripts/fh2ctl.sh` talks to a running game through a named pipe: press keys,
  flip renderer switches (`env FH2_X 0`), capture renderer frame logs
  (`capture 2`), read and write guest memory, set the time of day (`tod 19.5`).
- `tools/` holds analysis scripts (`callgraph.py`, `extract_shaders.py`, ...) and
  the original project's Windows PowerShell scripts, which are not maintained.

## Credits

- **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)** by Tom Clay -- the
  static recompiler and runtime this project is built on (BSD-3-Clause, see
  `rexglue-sdk/LICENSE`).
- **[Xenia](https://xenia.jp)** by Ben Vanik and contributors -- the Xbox 360
  emulator that much of the SDK's kernel, GPU and Vulkan backend code, and through
  it this renderer, derive from (BSD).
- **[FábioAP](https://github.com/fabioap-cpu/Forza-Horizon-2-Recomp)** -- created
  the original project: the ReXGlue project setup for FH2, the vendored SDK, and
  the heap and null-call crash guards in `src/forzahorizon2_hook_sub_*.cpp`. That
  version reached the loading screen on Windows.
- **jas3333** (this fork) -- Linux port; the fix for the boot stall (a stub that
  stopped the game's job worker threads) and other threading and save-game fixes;
  the native Vulkan renderer; the settings menu, debugging tools and performance
  work.

## License

The vendored SDK is BSD-3-Clause (`rexglue-sdk/LICENSE`); its license covers the
SDK only. The original project did not include a license for its own files, so no
license is granted here for the project code beyond what applies to the SDK.

This license situation does not grant any rights to Forza Horizon 2 or to anything
generated from it. Do not redistribute game files, the generated code, or binaries
built from them.

This project is not affiliated with or endorsed by Microsoft, Xbox, Playground
Games, Turn 10 Studios, or the Forza franchise.

## Repository policy

Do not commit extracted game files, generated source (`generated/`, except the
`rexglue.cmake` boilerplate), saves, logs, captures or binaries. Check every
staged change before pushing.
