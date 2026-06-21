# Fixing Movie Playback

Engineering record for the Bink movie fix. Background/architecture is in
[movie-play.md](movie-play.md).

## Summary

Army of Two's Bink movies (`game/AO2Game/Movies/*.bik`) played with correct audio,
subtitles, and timing but **black video**. The cause is **not** the decoder — the
recompiled Bink decoder works fine and writes correct YUV frames into its plane
buffers. The problem is ReXGlue's **GPU render path: it rejects the movie's draw**
every frame, so the decoded frame is never drawn.

**Fix (shipped):** bypass the broken guest render. Each frame we read the game's
already-decoded Y/cR/cB planes, convert YUV→RGBA on the host, and draw the frame
as a fullscreen ImGui overlay. Audio, subtitles, timing, and skip stay on the
game's own working path. Implemented in `src/aot_bink_shim.cpp`; toggle with cvar
`aot_host_movies` (default on). **No FFmpeg and no ReXGlue changes needed.**

## How the fix works

Internal recompiled functions are overridden via the **weak-alias** trick: define
a strong `sub_XXXX` that calls the real body `__imp__sub_XXXX`. (Patching
`PPCFuncMappings` does *not* work for internal functions — it only intercepts
indirect/import-thunk calls, which is why the `NtCreateFile`/`NtOpenFile` open
trace can use it but the Bink calls cannot.)

1. **Capture HBINK** in a `sub_8308E368` override (the per-frame Bink call; `HBINK`
   in r3). This call runs *inside* the async decode/wait loop, so it is used only
   to record the handle, not to read planes.
2. **Publish after the decode completes** in a `sub_824B5EA8` override (the outer
   `UCodecMovieBink` service): call the original (its loop finishes the frame), then
   read the planes. (Reading inside `sub_8308E368` raced the decode workers.)
3. **Read + repair + convert** in `PublishMovieFrame`: read `HBINK →
   BINKFRAMEBUFFERS` (layout below); the broken decode strip leaves chroma at ~0
   (renders green), so repair it — fill dead chroma pixels horizontally from the
   nearest good pixel, then fill mostly-dead rows vertically from the nearest good
   row; a final output guard turns any residual green pixel grey. Convert BT.601
   YUV420 → RGBA on the movie thread; hand off to the UI thread under a mutex.
4. **Draw** the latest frame via an `ImGuiDialog` overlay
   (`ImGui::GetBackgroundDrawList()->AddImage`), created in
   `AotApp::OnCreateDialogs`, aspect-correct (letterboxed/centered). Only fullscreen
   movies (≥1024×576) are presented; the 100×100 `LoadingCoin*` UI loops are skipped.

## Reverse-engineering reference (verified against the running game)

- Engine/integration: UE3 `UCodecMovieBink` + RAD `BinkTextures` (plane-texture
  model — Bink decodes into Y/cR/cB/A plane buffers; a shader does YUV→RGB).
- Bink library is linked at ~`0x8308xxxx`; async decode workers at
  ~`0x82422000–0x82599000`. `.bik` files open on a worker thread.
- `sub_8308C518` = BinkOpen core (returns `HBINK` in r3).
- `sub_8308E368` = per-frame Bink call inside the decode loop (used to capture HBINK).
- `sub_824B5EA8` = `UCodecMovieBink` service (in `aot_recomp.20.cpp`); its loop
  returns once decode is complete — the host frame is published here.

`HBINK` (big-endian): `+0x00` Width, `+0x04` Height, `+0x08` Frames,
`+0x0C` FrameNum, `+0x10` LastFrameNum, `+0x14`/`+0x18` frame-rate num/den,
**`+0xB8` `BINKFRAMEBUFFERS*`**.

`BINKFRAMEBUFFERS` (at `HBINK+0xB8`): `+0x00` TotalFrames(=2, double-buffered),
`+0x04/0x08` YA W/H, `+0x0C/0x10` cRcB W/H, `+0x14` current buffer index,
`+0x18` `Frames[0]`, `+0x48` `Frames[1]`. Each `Frames[]` is 4 plane descriptors
(**order Y, cR, cB, A**), each = `{Allocate, Buffer(guest ptr), BufferPitch}`.
Planes live in the `0xFFxxxxxx` physical/GPU window. (cR=Cr/V, cB=Cb/U.)

## What we tried (and ruled out)

- **Host-decode the .bik with FFmpeg** (original plan): abandoned — the decoder
  already works, so re-decoding fixes nothing. (libavcodec *can* decode these
  files, confirmed via `scripts/probe-movies.ps1`, but it's unnecessary.)
- **Inject pixels into the planes** (overwrite Bink's buffers): a grey test
  pattern written to the planes never appeared on screen → confirmed the
  planes→screen path is broken, not the plane contents.
- **Fix the guest render (direction A):** the draw is rejected because the movie
  quad's vertex-fetch constant has type `kTexture`(2) in a vertex slot →
  ReXGlue's command processor logs `Vertex fetch constant ... completely invalid`
  and skips the draw (≈30/sec). Patching the SDK to allow that draw removed the
  rejection (449→0) but it was **still black** (another layer: shader-creation
  also fails, `shader backend factory failed rc=0x8000FFFF`). Direction A is a
  deep, multi-layer GPU-translation effort, so we pivoted to the host overlay (B).
  That SDK experiment was **reverted** — the shipped fix needs no SDK changes.

## Status & known issues

Movies now **play with correct video** (the main green band is fixed by the chroma
repair; verified on screen via the EA/Army-of-Two logos). Remaining, minor:

- **Small flashing green bar at the very top edge** (1–2 px). The top chroma rows
  are dead and the repair/guard don't fully catch this thin edge every frame.
  Cosmetic; tracked as a follow-up.
- Possible slight **offset** reported during early iterations — not reproduced in
  the final full-res pipeline dumps (content was centred); re-check on a cutscene.
- Cutscene **subtitles are covered** by the fullscreen overlay (the game draws them
  into the black guest frame). Logos have no subtitles.
- **Perf:** the overlay recreates its `ImmediateTexture` every frame, and the
  overlay/compositing path runs whenever a movie is active.
- `LoadingCoin*` UI loops stay black (not host-presented; not fullscreen).

## Next steps (follow-ups)

1. **Top green bar:** clamp/repair the top 1–2 chroma rows specifically (e.g.
   vertical-fill the first good row into the top rows, or widen the dead test for
   the top edge). Minor.
2. **Perf:** reuse one `ImmediateTexture` (update in place) instead of recreating
   per frame; only engage the overlay while a fullscreen movie is active.
3. **Subtitles:** for cutscenes, find a way to keep the game's subtitle pass
   visible over the overlay (or composite under it).
4. **Offset:** confirm whether any real offset remains on a cutscene.
5. **Optional:** present the `yuva420p` UI loops too (needs their on-screen rect).

## Repos to update — no ReXGlue fork

`tools/rexglue-sdk` and `build/` are git-ignored (local build deps). The fix is
**entirely in `LibreArmyOfTwo`** and runs on stock ReXGlue:
- new: `src/aot_bink_shim.{h,cpp}`, `scripts/probe-movies.ps1`, `docs/*`
- changed: `src/aot_app.h` (install hook + overlay), `CMakeLists.txt`
  (adds `aot_bink_shim.cpp`), `config/aot_rexglue_overrides.toml` (name hints)

## Reproduce / diagnostics

```powershell
# Confirm the .bik files decode on the host (sanity check only; not used at runtime)
powershell -ExecutionPolicy Bypass -File scripts\probe-movies.ps1 -Decode

# Run with movie open-trace logging
.\rungame.ps1 -StopExisting -ExtraArgs "--aot_trace_movies"

# Disable the host overlay (movies fall back to stock black)
.\rungame.ps1 -StopExisting -ExtraArgs "--aot_host_movies=false"
```
