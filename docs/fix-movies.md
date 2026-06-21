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

1. **Hook the per-frame Bink call** `sub_8308E368` (the `UCodecMovieBink` service
   call; `HBINK` in r3). It's an internal recompiled function, so it's overridden
   via the **weak-alias** trick: define a strong `sub_8308E368` that calls the real
   body `__imp__sub_8308E368`, then reads the decoded planes. (Patching
   `PPCFuncMappings` does *not* work for internal functions — that only intercepts
   indirect/import-thunk calls, which is why the `NtCreateFile`/`NtOpenFile` open
   trace can use it but the Bink calls cannot.)
2. **Read the planes** from `HBINK → BINKFRAMEBUFFERS` (layout below), convert
   BT.601 limited-range YUV420 → RGBA on the movie thread, hand off to the UI
   thread under a mutex.
3. **Draw** the latest frame fullscreen via an `ImGuiDialog` overlay
   (`ImGui::GetBackgroundDrawList()->AddImage`), created in
   `AotApp::OnCreateDialogs`. Only fullscreen movies (≥1024×576) are presented; the
   small 100×100 `LoadingCoin*` UI loops are skipped.

## Reverse-engineering reference (verified against the running game)

- Engine/integration: UE3 `UCodecMovieBink` + RAD `BinkTextures` (plane-texture
  model — Bink decodes into Y/cR/cB/A plane buffers; a shader does YUV→RGB).
- Bink library is linked at ~`0x8308xxxx`; async decode workers at
  ~`0x82422000–0x82599000`. `.bik` files open on a worker thread.
- `sub_8308C518` = BinkOpen core (returns `HBINK` in r3).
- `sub_8308E368` = per-frame service call (the host hook point).
- `UCodecMovieBink` service method = `sub_824B5EA8` (in `aot_recomp.20.cpp`).

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

## Known issues

- **Green vertical band** on fullscreen movies: at the read point one async-decode
  worker strip's *chroma* isn't finished (luma is fine, U/V≈0 → green). Reading the
  current or previous frame buffer both still catch it.
- Cutscene **subtitles are covered** by the fullscreen overlay (the game draws them
  into the black guest frame). Logos have no subtitles.
- No letterbox (stretches to window); the overlay texture is recreated each frame.
- `LoadingCoin*` UI loops stay black (not host-presented).

## Next steps

1. **Green band — read a complete frame.** Find the true "decode complete" sync
   point and read there. Candidates around the service loop in `sub_824B5EA8`:
   the post-loop `sub_8308DE38`, or `sub_8308E968`/`sub_8308D628` — instrument to
   see which returns only after all worker strips finish. Fallbacks:
   - *Detect-and-skip:* if the chroma has a contiguous ~0 column band, the frame is
     incomplete — keep the previous published frame (worst case repeats a frame).
   - *Column-merge:* fill ~0-chroma columns from the other (complete) buffer.
2. **Aspect ratio:** letterbox 16:9 instead of stretching.
3. **Perf:** reuse one `ImmediateTexture` (update in place); only run the overlay
   while a fullscreen movie is active.
4. **Optional:** present the `yuva420p` UI loops too (needs their on-screen rect).

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
