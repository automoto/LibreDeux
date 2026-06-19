# Development

## Codegen Loop

1. Change `config/aot_rexglue_overrides.toml` or app source.
2. Run `scripts/build-aot.ps1 -Regenerate`.
3. Launch with `.\rungame.ps1 -StopExisting`.
4. Inspect the newest log under the build directory.

## Missing Function Loop

If the log reports `invalid or unregistered function`, add only that observed guest address to `config/aot_rexglue_overrides.toml`, regenerate, rebuild, and retest.

## Capture Loop

Use the bounded capture script for first-boot proof runs:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-aot-input-capture.ps1 -Seconds 120 -StopExisting
```

The input plan is intentionally generic until the first boot path is known.

To isolate early audio-adjacent crashes without changing the normal launch path:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\run-aot-input-capture.ps1 -Seconds 30 -StopExisting -NoInput -ExtraArgs "--aot_nop_audio --log_verbose --log_noisy"
```
