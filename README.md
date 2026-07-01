# Libre Army of Two

A [ReXGlue](https://github.com/rexglue)-based native PC static recompilation of
**Army of Two** (Xbox 360).

You must provide your own legally obtained disc image. This repository contains **no**
game assets, retail binaries, code generated from a retail binary, keys, or downloads.

## Dependencies

- **CMake** 3.25+ and **Ninja**
- **LLVM/Clang** at `C:\Program Files\LLVM` (the presets reference it there)
- **Visual Studio 2022 Build Tools** with the Desktop C++ workload (MSVC headers,
  Windows SDK, `vcvarsall.bat`)
- **Python 3** (game extraction + codegen; standard library only)

## Build & Run

Run the CMake commands from an **"x64 Native Tools Command Prompt for VS 2022"** with
`C:\Program Files\LLVM\bin` on `PATH`, so Clang finds the MSVC/Windows SDK environment.
Each step must succeed before the next.

**1. Clone with submodules** (the ReXGlue SDK is a submodule under `tools/`):

```
git clone --recurse-submodules <repo_url>
cd LibreArmyOfTwo
```

Already cloned without `--recurse-submodules`? Run
`git submodule update --init --recursive`.

**2. Extract your game.** Put your own legal Army of Two disc image in `iso\`, then:

```
python scripts\extract_iso.py
```

This writes the game files to `game\` (with `game\default.xex`).

**3. Build the ReXGlue SDK** (once). On Windows, first materialize the SDK's symlinked
sources; Tracy (a profiler) is disabled to keep the link clean:

```
python scripts\repair_sdk_symlinks.py
cmake --preset win-amd64 -S tools\rexglue-sdk -DCMAKE_C_FLAGS=-mssse3 -DCMAKE_CXX_FLAGS=-mssse3 -DREXGLUE_ENABLE_TRACY=OFF
cmake --build tools\rexglue-sdk\out\build\win-amd64 --config Release --target install
```

Installs to `tools\rexglue-sdk\out\install\win-amd64`, where the project preset looks.

**4. Generate the recompiled code** from your game copy (runs ReXGlue codegen, then the
Army of Two source workarounds):

```
python scripts\codegen.py
```

**5. Configure and build the host:**

```
cmake --preset release
cmake --build build\aot-ninja-release --target aot
```

**6. Run:**

```
build\aot-ninja-release\aot.exe --game_data_root=game
```

Local split-screen co-op with two XInput controllers:

```
build\aot-ninja-release\aot.exe --game_data_root=game --input_backend=xinput --aot_coop_local
```

After editing `config\aot_rexglue_overrides.toml` or app source, re-run step 4 (codegen)
then step 5 (build).

## Current Status

Boots into the visible main menu, reaches single-player gameplay, and supports local
split-screen co-op with two XInput controllers. In-engine Bink movies/cutscenes play
through a host overlay, and HDR exposure is corrected.

### Known Issues

- **See-through geometry:** some background structures can render through foreground
  geometry (a GPU occlusion-query parity gap). Not fixed in this build; see
  [docs/fix-occlusion-queries.md](docs/fix-occlusion-queries.md).

## ReXGlue SDK

The `tools/rexglue-sdk` submodule is the **official** `rexglue/rexglue-sdk`, pinned to
commit `e8ce24f` (SDK `0.8.1.4`). Army of Two builds against vanilla ReXGlue; the
occlusion fix above is not part of it.

## Legal / Repo Hygiene

Keep this repo free of game assets and generated game-derived code. See
[docs/legal.md](docs/legal.md).
