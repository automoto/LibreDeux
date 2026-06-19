# Building

## Expected Layout

```text
game/default.xex
tools/rexglue-sdk/out/install/win-amd64-nmake/bin/rexglue.exe
```

`game/`, `generated/`, `build/`, and local tool directories are ignored by git.

## Build Official ReXGlue

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-rexglue-official.ps1
```

Use a custom official checkout path:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-rexglue-official.ps1 -RexGlueSource C:\path\to\rexglue-sdk
```

## Build This Project

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-aot.ps1 -Regenerate
```

Use a custom ReXGlue SDK install path:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build-aot.ps1 -Regenerate -RexGluePrefix C:\path\to\rexglue\install
```

## Launch

```powershell
powershell -ExecutionPolicy Bypass -File .\rungame.ps1 -StopExisting
```
