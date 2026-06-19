# Status

Initial scaffold created. The local ISO has been extracted into ignored `game/`.

- The repo has an official-ReXGlue-oriented manifest, CMake target, launcher, and build scripts.
- The ReXGlue manifest lives under `config/` so codegen does not scan ignored local tool checkouts under the repo root.
- ISO extraction and XEX analysis helpers are wired to the local 360tools layout used by the earlier 3U workspace.
- `game/default.xex` exists locally after extraction.
- The official ReXGlue SDK is cloned under ignored `tools/rexglue-sdk` and built locally from `rexglue/rexglue-sdk` `main`.
- Initial ReXGlue analysis found three missing branch targets, now tracked in `config/aot_rexglue_overrides.toml`.
- ReXGlue codegen now completes and the generated host builds to `build/aot-nmake-release/aot.exe`.
- First boot reaches window creation, XEX loading, import setup, thread startup, GPU shader/pipeline work, and audio submission before exiting with access violation `0xC0000005`.
- The last verbose logs cluster around XAudio render-driver frame submission, with no missing import or unimplemented-function failure immediately preceding the crash.
- `--aot_nop_audio` is available as an opt-in triage cvar to replace SDL audio with ReXGlue's NOP audio backend for crash isolation. The NOP-audio probe still exits with `0xC0000005`, after the game registers the XAudio render-driver client and queries `ShaderDumpxe:\CompareBackEnds`.
