# Development

## Codegen Loop

1. Change `config/aot_rexglue_overrides.toml` or app source under `src/`.
2. Regenerate: `python scripts\codegen.py` (ReXGlue codegen + AoT workarounds).
3. Rebuild: `cmake --build build\aot-ninja-release --target aot`.
4. Launch and inspect the newest log under the build directory:
   `build\aot-ninja-release\aot.exe --game_data_root=game`.

## Missing Function Loop

If the log reports `invalid or unregistered function`, add only that observed guest
address to `config/aot_rexglue_overrides.toml`, then regenerate and rebuild. Do not batch
hints by nearby address alone — add only addresses observed in the same run, or exact
targets proven by table/callsite analysis.

Note: a `[functions."0x..."]` entry's `name` sets the emitted C++ symbol name. For a
function overridden in `src/` (e.g. the Bink seam in `aot_bink_shim.cpp`), the name must
match the `sub_<addr>` symbol that override references.

## Local Co-Op

Connect two XInput controllers before launch, then run:

```
build\aot-ninja-release\aot.exe --game_data_root=game --input_backend=xinput --aot_coop_local
```

Add `--aot_trace_xam` only when capturing XAM user/input diagnostics (verbose).

## Debugging cvars

The host forwards arbitrary ReXGlue cvars, so no rebuild is needed to toggle them:

- `--log_verbose --log_noisy` — verbose logging.
- `--aot_nop_audio` — isolate early audio-adjacent crashes.
- `--aot_graphics_backend=d3d12|vulkan` — select the graphics backend.

The scripted-input capture and crash-dump tooling from earlier development is not part of
this public tree.
