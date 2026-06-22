# Bug: host-presented movies render offset (horizontally wrapped)

## Status: FIXED

## Symptom

Army of Two's intro movies (EA logo, Army-of-Two logo) played **offset**: the
picture was shifted right so the right edge wrapped around to the left of the
frame. Confirmed visually in-game and verified against an ffmpeg reference decode
of the same `.bik` files (which are correctly centered at the source).

## Root cause

Under ReXGlue the decoded picture comes out of the Bink plane buffers **cyclically
rotated to the right within each row** — each luma row is shifted right by 256 px
(chroma by 128 px, since chroma is half-res), wrapping the right edge to the left.

`PublishMovieFrame` in [src/aot_bink_shim.cpp](../src/aot_bink_shim.cpp) read each
plane row straight (`yr[x]`), so it reproduced the wrap verbatim in the output
texture. The conversion is an identity copy in x, so the shift originates entirely
in the source plane layout, not in our loop.

### How it was diagnosed

1. Dumped the raw RGBA we produce (decoupled from ImGui/GPU) → the texture itself
   was offset, so the bug is in `PublishMovieFrame`, not the draw/upload.
2. Decoded the same `.bik` frame with ffmpeg as ground truth → source is centered.
3. Cross-correlated our frame vs reference → uniform **+256 px** horizontal shift
   (not a shear, so the plane pitch is correct).
4. Dumped both Bink frame buffers' raw Y planes → both identically shifted (not a
   wrong-buffer issue); faint left-edge content matched the picture's right edge →
   a **cyclic wrap**, not a lossy shift.
5. Cyclically rotating each row left by 256 px re-centered the picture exactly,
   with no seam → confirmed the model.

The offset ties to the chroma-plane pitch padding: `cr_pitch - cw = 768 - 640 =
128` (chroma), and `2 × 128 = 256` (luma). All of the game's fullscreen movies are
1280×720, so this is `x_wrap = 256` in practice.

## Fix

In the YUV→RGBA conversion ([src/aot_bink_shim.cpp](../src/aot_bink_shim.cpp)),
sample each source column at `(x + x_wrap) mod w` instead of `x`, with
`x_wrap = 2 * (cr_pitch - cw)`. Using the un-wrapped source x for both luma (`yr[sx]`)
and the chroma index (`sx >> 1`) keeps luma and chroma aligned (no color fringing).

The change is a ~10-line diff in the conversion loop; nothing else in the pipeline
changes. (Diagnostic scaffolding used during investigation — raw frame/Y-plane
dumps, a geometry trace, and a speculative display-dims tweak — was removed; only
the verified fix remains.)

## What was NOT the cause

- Not the SDK texture upload — D3D12 `CreateTexture` copies row-by-row using the
  aligned footprint pitch, so it does not shear/wrap a `w*4` source.
- Not the draw/letterbox — the raw texture was already offset before upload.
- Not a padded buffer width — all intro clips are 1280×720 with `pitch == width`.

## TODO

- [x] Investigate root cause (shim conversion vs SDK upload vs draw)
- [x] Reproduce & measure offset (raw dump + ffmpeg reference + cross-correlation)
- [x] Confirm cyclic-wrap model (both Y buffers; rotate-to-center test)
- [x] Implement `x_wrap` correction in the converter
- [x] Verify corrected raw frames are centered + correct color (EA + Army of Two)
- [x] Remove diagnostic dump code; keep the geometry trace
- [x] Final in-game confirmation (maximized window): logos centered
- [x] Update [fix-movies.md](fix-movies.md)
- [ ] (Separate, pre-existing) thin green bar at the very top edge — cosmetic follow-up

## Verification

1. `powershell -ExecutionPolicy Bypass -File scripts\build-aot.ps1`
2. `.\rungame.ps1 -StopExisting` — EA and Army-of-Two logos render centered.
3. Optional ground-truth check: decode a reference frame and compare —
   `& <ffmpeg> -ss 3 -i game\AO2Game\Movies\Ao2Logo.bik -frames:v 1 ref.png`
4. `--aot_host_movies=false` still falls back to stock black movies (unchanged).
