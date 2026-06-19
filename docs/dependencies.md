# Dependencies

## ReXGlue SDK

Use the official ReXGlue SDK repository unless Army of Two proves it needs project-specific runtime fixes.

Default local layout:

```text
LibreArmyOfTwo/
  tools/rexglue-sdk/
```

Clone:

```powershell
git clone --recursive https://github.com/rexglue/rexglue-sdk.git tools\rexglue-sdk
```

Build:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-rexglue-official.ps1
```

On Windows, the build helper materializes libmspack symlinks in the ignored SDK checkout when Git checked them out as text placeholders. It also defaults Clang builds to `-mssse3`, which current ReXGlue sources need for byte-swap intrinsics.

The project build script defaults to:

```powershell
tools\rexglue-sdk\out\install\win-amd64-nmake
```

## Extraction Tools

`scripts\extract-game.ps1` defaults to the local 360tools checkout used by the 3U workspace:

```powershell
..\3Unchallenged\tools\360tools
```

Pass `-ToolsRoot` to use another checkout.

## Game Data

This project does not provide game files. Put your own legal extraction in `game/`.
