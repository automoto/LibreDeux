# Development

## Codegen Loop

1. Change `config/aot_rexglue_overrides.toml` or app source.
2. Run `scripts/build-aot.ps1 -Regenerate`.
3. Launch with `.\rungame.ps1 -StopExisting`.
4. Inspect the newest log under the build directory.

Use the NMake build path for current gameplay milestones. Do not use the partial
Ninja build tree unless it is deliberately revived later.

## Missing Function Loop

If the log reports `invalid or unregistered function`, add only that observed guest address to `config/aot_rexglue_overrides.toml`, regenerate, rebuild, and retest.

Do not batch new function hints by nearby address alone. Batch only addresses
observed in the same log/run, or exact targets proven by table/callsite
analysis.

## Capture Loop

Use the bounded capture script for menu proof runs:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-aot-crash-repro.ps1 -Seconds 30
```

Use the input capture script for menu navigation experiments:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-aot-input-capture.ps1 -Seconds 120 -StopExisting
```

The input plan is still generic. Manual controller testing may be needed when
keyboard input does not reproduce Start/A menu paths.

## Local Co-Op

Connect two XInput controllers before launch, then run:

```powershell
.\rungame.ps1 -XInput -LocalCoop -StopExisting
```

Use `-CoopTrace` only when capturing XAM user/input diagnostics. It expands the
log with per-call local co-op traces and is not needed for normal play.

To isolate early audio-adjacent crashes without changing the normal launch path:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-aot-input-capture.ps1 -Seconds 30 -StopExisting -NoInput -ExtraArgs "--aot_nop_audio --log_verbose --log_noisy"
```

## Crash Loop

Enable per-user Windows Error Reporting dumps for `aot.exe`:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\enable-aot-local-dumps.ps1
```

Run the current bounded repro:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-aot-crash-repro.ps1
```

Analyze the newest dump when Windows Debugging Tools are installed:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\analyze-aot-dump.ps1
```

For a symbol-friendly build:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-aot-symbols.ps1 -Configure
```
