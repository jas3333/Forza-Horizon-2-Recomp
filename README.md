# Forza Horizon 2 Recompiled

An experimental static recompilation of the Xbox 360 version of Forza Horizon 2, using [ReXGlue](https://github.com/rexglue/rexglue-sdk) (vendored in this repository under `rexglue-sdk/`, so a single `git clone` is enough to build -- no separate SDK checkout or submodule step).

This repository contains project source, recompilation metadata, build configuration, and the SDK it builds against. It does not contain game data, Xbox system files, saves, or compiled executables. You must provide files from your own copy of the game.

The vendored SDK is licensed `BSD-3-Clause` (see `rexglue-sdk/LICENSE`); portions are derived from the [Xenia project](https://xenia.jp). This license applies only to the SDK and this project's own source -- it does not grant rights to game files or anything generated locally from them.

## Project status

This is an **early, unresolved work in progress** -- not a working build. Being upfront about that on purpose: on the tested Windows AMD64 system, the current tree:

- boots and reaches the loading screen; but
- **the main guest thread permanently stalls waiting on a kernel semaphore during boot, and the game never progresses past the loading screen.** The root cause has not been found yet -- confirmed (via a structured comparison against a real Xenia execution trace) to be a genuine behavioral divergence from correct execution, not a build/config issue, and confirmed independent of the underlying SDK's own code (identical before and after a large upstream SDK merge). See `rexglue-sdk`'s diagnostic tooling (`[WAITDIAG]`, `[CALLTRACE]`, checkpoint probes) if you want to pick up that investigation.

Two other real, reproducible crashes were found and fixed during this same investigation (both are workarounds around confirmed root causes, not upstream fixes):

- a null vtable-slot indirect call that crashed early in boot (`src/forzahorizon2_hook_sub_824a6170.cpp`);
- a heap free/coalesce routine dereferencing an uninitialized or corrupted list pointer, which could escalate into heap corruption and a fatal crash in a background thread (`src/forzahorizon2_hook_sub_823f4ee8.cpp`, `src/forzahorizon2_hook_sub_823f8718.cpp`).

Neither of those two is the reason the game doesn't reach gameplay -- that's still the open semaphore stall above.

## Known issues and tested boundary

- Main thread boot stall (see above) -- this is the actual blocker right now.
- The corruption behind the heap-free crash (garbage Flink/Blink pointers) has a guard against it now, but its root cause is still unknown; the guard could theoretically be masking a real logic bug elsewhere instead of a truly-corrupt-but-harmless value.
- Nothing past the loading screen has been tested. Gameplay, races, career progression, and content installation are all unverified.

## Requirements

- Windows AMD64
- Git, CMake 3.25 or newer, Ninja, and Clang
- a legally obtained, extracted Forza Horizon 2 (Xbox 360) game tree

The project expects these game executables from your own extracted copy:

```text
default.xex
XMediaFacade_default.xex
SpeechFacade_default.xex
```

## Build

```powershell
git clone https://github.com/fabioap-cpu/Forza-Horizon-2-Recomp.git
cd Forza-Horizon-2-Recomp

# Point the codegen/runtime at your own extracted game copy.
# Edit config_default.toml: set game_data_root to that path.

cmake --build --preset win-amd64-release --target forzahorizon2_codegen
cmake --build --preset win-amd64-release --target forzahorizon2
```

The first command runs the ReXGlue codegen (recompiles the guest XEX into C++ under `generated/`, not committed -- it's regenerated locally and can be large). The second builds the actual `forzahorizon2.exe`.

Given the current known stall, launching the result will boot into a loading screen and stop there -- see **Project status** above.

## Repository policy

Do not commit extracted game files, generated source (`generated/` -- gitignored), user data, saves, diagnostic captures/logs, or compiled binaries. Inspect every staged change before publishing it.

This project is not affiliated with or endorsed by Microsoft, Xbox, Playground Games, Turn 10 Studios, or the Forza franchise.
