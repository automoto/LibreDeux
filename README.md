# Libre Army of Two

Libre Army of Two is a ReXGlue-based native PC recompilation workspace for Army of Two on Xbox 360.

Compatibility note: you must provide your own legally obtained disc/ISO extraction. This repository does not include game assets, retail binaries, generated code from a retail binary, keys, patches, or downloads.

## Quick Start

Clone or place the official ReXGlue SDK under ignored local tools:

```powershell
git clone --recursive https://github.com/rexglue/rexglue-sdk.git tools\rexglue-sdk
```

Build and install the official SDK:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-rexglue-official.ps1
```

Extract your legally obtained ISO into `game\`:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\extract-game.ps1
```

Build this project:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-aot.ps1 -Regenerate
```

Run the game manually:

```powershell
powershell -ExecutionPolicy Bypass -File .\rungame.ps1 -StopExisting
```

Run local split-screen co-op with two XInput controllers:

```powershell
powershell -ExecutionPolicy Bypass -File .\rungame.ps1 -XInput -LocalCoop -StopExisting
```

## Current Status

Scaffold, legal local extraction, official ReXGlue code generation, and host
build are working locally. The current build boots into the visible main menu,
reaches single-player gameplay, and supports local split-screen co-op with two
XInput controllers; see `plan.md` for the gameplay plan and `local-coop.md` for
the co-op notes.

## Repo Map

- `config/aot_manifest.toml` - ReXGlue project manifest.
- `rungame.ps1` - manual game launcher.
- `scripts/` - extraction, build, analysis, capture, and audit helpers.
- `src/` - small ReXGlue app customization.
- `config/` - tracked ReXGlue function hints.
- `docs/` - setup, legal policy, development notes, and status.

## Rexglue

This repo defaults to the official `rexglue/rexglue-sdk` repository at `tools\rexglue-sdk`. Do not use the 3U compatibility fork unless Army of Two proves it needs the same runtime fixes.

## Legal / Repo Hygiene

Keep this repo free of game assets and generated game-derived code. See `docs/legal.md`.
