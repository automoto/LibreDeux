# Known limitation: `aot.toml` silently overrides command-line flags

## TL;DR

If a command-line flag (cvar) "doesn't take effect," **check for an `aot.toml` next to the
`aot.exe` first.**

cvar source precedence in ReXGlue is currently **config file > environment variable >
command line** — the *inverse* of the usual expectation. A value set in `<exe_dir>/aot.toml`
will silently win over the same `--flag` passed on the command line, with no warning.

This is **dormant today** because no `aot.toml` ships next to the built exe, so command-line
flags work as expected. The trap only springs once someone drops an `aot.toml` beside the exe.

## Why this happens

cvars are applied "last writer wins" — there is no per-flag source priority. The three sources
are applied in this order:

1. **Command line** — `cvar::Init(argc, argv)` parses argv at the very top of `wWinMain`
   (`tools/rexglue-sdk/src/ui/windowed_app_main_win.cpp:65`).
2. **Environment** — `cvar::ApplyEnvironment()` immediately after
   (`windowed_app_main_win.cpp:66`).
3. **Config file** — `cvar::LoadConfig(<exe_dir>/aot.toml)`, applied *much later* inside
   `ReXApp::SetupEnvironment()` (`tools/rexglue-sdk/src/ui/rex_app.cpp:122-123`).

Because the config file is written to cvar storage last, it overwrites whatever the command
line and environment set. The registry (`tools/rexglue-sdk/src/core/cvar.cpp:466`, `Init`) does
not track which source set a value, so there is nothing to protect the command-line value.

This ordering is **official ReXGlue SDK behavior** (`tools/rexglue-sdk/` is the upstream
`rexglue/rexglue-sdk` repo) — it was not introduced by this project (LibreArmyOfTwo).

## Config file location

The config path is `<exe_dir>/aot.toml` — i.e. `aot.toml` next to `aot.exe` (the app name
`"aot"` + `.toml`, resolved in `ReXApp::SetupEnvironment`). `LoadConfig` early-returns if the
file does not exist, which is why the limitation is dormant when no such file is present.

## Workarounds (for now)

- If you rely on command-line flags for experiments, **don't keep an `aot.toml` next to the
  exe.**
- Or, put your experiment toggles **in `aot.toml`** instead of on the command line — the file
  always wins, so configure there and it will stick.
- Or, remove/unset the specific key in `aot.toml` that is shadowing the flag you want to pass.

## Related gotcha: typo'd flags are silently ignored

`cvar::Init` uses CLI11 with `allow_extras()` and only prints parse errors to **stderr**
(`tools/rexglue-sdk/src/core/cvar.cpp:483-489`), which a windowed app discards. A mistyped
`--flag` (or one whose module isn't registered) is dropped with no log feedback — another
reason a flag can appear "not registered." There is also no boot log of which cvars actually
took effect.

## If we ever decide to fix it

Re-assert environment + command-line overrides *after* `LoadConfig`, so precedence becomes
config < env < CLI. The clean, SDK-pristine spot is `AotApp::OnPostInitLogging()`
(`src/aot_app.h`), which runs after `LoadConfig` (`rex_app.cpp:123`) but before any graphics
cvar is consumed (the GPU command processor / render-target cache read them during
`runtime_->Setup()`). The same hook is the natural place to also log non-default cvars
(`cvar::ListModifiedFlags()`) and warn on unrecognized `--flags`. Not doing this now — noted
here only as a pointer.
