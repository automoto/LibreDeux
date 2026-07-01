# Building

Build from an **"x64 Native Tools Command Prompt for VS 2022"** with
`C:\Program Files\LLVM\bin` on `PATH`. The build uses CMake + Ninja. See the
[README](../README.md) for the full step-by-step; this is a quick reference.

## Layout

```text
tools/rexglue-sdk/                              official ReXGlue SDK (git submodule)
tools/rexglue-sdk/out/install/win-amd64/...     built + installed SDK
game/default.xex                                your legal extraction (git-ignored)
```

`game/`, `iso/`, `generated/`, `build/`, and other tool outputs are git-ignored.

## 1. Extract the game

```
python scripts\extract_iso.py            # auto-detects iso\**\*.iso -> game\
python scripts\extract_iso.py path\to\game.iso
```

## 2. Build the ReXGlue SDK (once, Ninja)

```
python scripts\repair_sdk_symlinks.py
cmake --preset win-amd64 -S tools\rexglue-sdk -DCMAKE_C_FLAGS=-mssse3 -DCMAKE_CXX_FLAGS=-mssse3 -DREXGLUE_ENABLE_TRACY=OFF
cmake --build tools\rexglue-sdk\out\build\win-amd64 --config Release --target install
```

Installs to `tools\rexglue-sdk\out\install\win-amd64` — the path the project preset uses.
`-mssse3` matches Army of Two's baseline; `-DREXGLUE_ENABLE_TRACY=OFF` skips the Tracy
profiler (not needed, and it does not link under current LLVM/lld).

## 3. Generate code + build this project

```
python scripts\codegen.py                                  # ReXGlue codegen + AoT workarounds
cmake --preset release
cmake --build build\aot-ninja-release --target aot
```

To point at a ReXGlue SDK install elsewhere, override on configure:
`cmake --preset release -DCMAKE_PREFIX_PATH=C:\path\to\rexglue\install`.

## 4. Launch

```
build\aot-ninja-release\aot.exe --game_data_root=game
```
