# Army of Two — visual bug investigation (living document)

Status: **in progress.** This is the working tracker for the game's rendering problems. There
are **two distinct symptoms** that may have **different root causes** — do not assume one fix
covers both.

It renders correctly in Xenia, so this is a config/parity gap, not a fundamental failure:
- Xenia compat: <https://github.com/xenia-project/game-compatibility/issues/167>
- Xenia Canary compat: <https://github.com/xenia-canary/game-compatibility/issues/543>
  (labeled `gpu-readback` + `tech-engine-unreal`; reported symptom "overbright")

---

## Symptoms

1. **Over-bright / washed-out** — the whole frame is blown out, with a blurred/ghosted
   "double image" overlaid (looks like bloom on top of an over-exposed scene).
2. **See-through objects / geometry** — foreground characters and the first-person weapon
   render translucent / you can see through them.

(Observed across the tutorial-area screenshots, on the D3D12 backend.)

---

## Bug 1 — over-bright / washed-out

**Confidence: HIGH.** Root cause identified.

Army of Two is Unreal Engine 3 and uses **HDR eye-adaptation**: it renders to an HDR target,
resolves a downsampled luminance, and **reads it back to the CPU** to drive auto-exposure +
bloom. That requires GPU→CPU *resolve readback*.

ReXGlue (like stock Xenia) ships readback-resolve **disabled by default**:
`readback_resolve = "none"` and `d3d12_readback_resolve = false` →
`ReadbackResolveMode::kDisabled`. With no luminance readback, exposure stays pinned at a
blown-out value → over-exposure → the bloohttps://github.com/xenia-project/xenia.gitm pass blurs the blown-out frame and composites it
back → washed-out + ghosted look.

**Xenia's documented fix for this exact title is `d3d12_readback_resolve = true`**
(emuline working config; Xenia readback-resolve wiki: it fixes games that are "overly bright or
dark" due to HDR eye-adaptation). It "works in Xenia" because users flip that knob; we ship it
off.

Code references (official SDK under `tools/rexglue-sdk/`, not modified):
- `GetReadbackResolveMode()` — `src/graphics/command_processor.cpp:246`
  (`d3d12_readback_resolve=true` → `kFast`; `readback_resolve="full"` → `kFull`).
- Mode read per-resolve at `src/graphics/d3d12/command_processor.cpp:2924`, `:3116`.
  Both cvars are `kHotReload`.

Note: an earlier "memexport fast-path readback bug" lead was a **false alarm** —
`IssueDraw_MemexportReadbackFullPath` (`src/graphics/d3d12/command_processor.cpp:2757-2799`)
does its own `AwaitAllQueueOperationsCompletion()` + memcpy to guest memory. No SDK fix needed.

**Fix (AOT-layer only):** a single AOT knob **`aot_hdr_readback`** (`auto`/`fast`/`full`/`off`,
default `auto`), read once in `AotApp::OnPreSetup`, that sets the underlying readback cvars:
- `auto` (default) → kFast (Xenia parity), applied only if the user hasn't already set the
  underlying `d3d12_readback_resolve`/`readback_resolve` cvars.
- `fast` → `d3d12_readback_resolve=true` (kFast).
- `full` → `readback_resolve="full"` (kFull) — documented **backup** if kFast is insufficient.
- `off` → readback disabled (for A/B testing and perf).

**Why a dedicated knob instead of guarding on `HasNonDefaultValue` alone:** see
[cli-limitation.md](cli-limitation.md). The cvar system has **no source tracking**, so
`HasNonDefaultValue` cannot distinguish "user explicitly set `d3d12_readback_resolve=false`"
from "unset" (both equal the default `false`). A naive guard would silently re-enable readback
and make it impossible to turn OFF (breaking the `-NoReadback` A/B test). Because
`aot_hdr_readback`'s `off`/`full`/`fast` are all *non-default* enum values there is no blind
spot, and the knob is overridable from CLI or `aot.toml` regardless of the config>CLI precedence
quirk (we read the resolved value in `OnPreSetup`, which runs after all sources are applied).

The `cli-limitation.md` precedence inversion does **not** block setting our default:
`OnPreSetup` (rex_app.cpp:273) runs after `LoadConfig` (rex_app.cpp:123), so we are the last
writer.

**Status:** fix implemented in code; **awaiting in-game verification.**

---

## Bug 2 — see-through geometry (occlusion / depth correctness)

**Confidence: MEDIUM (hypothesis: depth-buffer correctness).** Persists after Bug 1 fixed.

Refined understanding (from user, post-exposure-fix):
- The "horizontal black bar" is **an actual game object** rendering wrong (opaque-black, clipping
  through other geometry) — **NOT** a framebuffer/EDRAM artifact. EDRAM tile-row theory dropped.
- Affected: **characters (skinned meshes) AND most/all solid (opaque) objects** render
  see-through. Transparent surfaces / alpha-tested foliage are *not* singled out.

→ Broad see-through of **opaque** geometry = objects failing to occlude what's behind them =
an **occlusion / depth-buffer correctness** problem, *not* an alpha-blend material issue.
Blend-state translation already verified faithful to Xenia (`pipeline_cache.cpp:1487-1574`) — ruled out.

Leading hypotheses:
- **float24 (D24FS8) depth handling.** Xbox 360 depth is float24; ReXGlue defaults
  `depth_float24_convert_in_pixel_shader=false`, `depth_float24_round=false`. Wrong float24
  depth → failed depth tests → see-through. **Prime suspect.**
- **Render path:** D3D12 on NVIDIA/AMD uses RTV (`kHostRenderTargets`); the **ROV** path does
  accurate per-pixel depth/blend. Re-test ROV now that exposure no longer masks the scene.
- Skinned-character meshes also affected → also re-test vertex-fetch (`gpu_allow_invalid_fetch_constants`).

Toggle bisect (priority order; user runs one per launch, records result):
1. `--render_target_path_d3d12=rov`
2. `--depth_float24_convert_in_pixel_shader=true`
3. `--depth_float24_round=true`
4. `-ReadbackFull` (`--readback_resolve=full`)
5. `--depth_transfer_not_equal_test=false`
6. `--gpu_allow_invalid_fetch_constants=true`
Combo if a single partially helps: `--render_target_path_d3d12=rov --depth_float24_convert_in_pixel_shader=true`.

Note: items 1 & 6 were tried *before* the Bug 1 exposure fix (washout masked the result) → re-testing.

**Toggle bisect result (2026-06-21): ALL NEGATIVE.** `render_target_path_d3d12=rov` (no change),
`depth_float24_convert_in_pixel_shader=true` (**worse** — added a giant black bar),
`depth_float24_round=true` (no change), `readback_resolve=full` (no change),
`depth_transfer_not_equal_test=false` (no change), `gpu_allow_invalid_fetch_constants=true` (no
change). → **Not config-tunable; not depth-precision; not the render path.** Depth-correctness
hypothesis is dropped.

**Refined symptom (user):** it's **specific walls / structures** that go transparent and
**clip through** other geometry (e.g. a distant dark structure clips wrong; a dark horizontal
streak on a wall). Foreground characters look mostly fine now. This is *selective* per-surface,
not a global occlusion failure.

**Re-characterized by user (2026-06-21, corrected + screenshot):** specific "black structures"
**clip into walls as flat dark shapes / thin horizontal black slivers**, and other objects
appear transparent (structures behind become visible).

**Leading theory: corrupted / degenerate VERTEX data for specific meshes.** A mesh with wrong
vertex positions collapses into a flat sliver → renders as a thin black shape clipping through
geometry, AND stops covering what's behind it → reads as "the object went transparent, revealing
the structure." This unifies both symptoms and explains why NO render-state cvar
(depth/blend/path/readback) affected it — corrupted geometry isn't a toggle. Classic
"works-in-Xenia-not-here" divergence: wrong vertex data fed to the otherwise-faithful Xenia GPU
backend.

Confirm/reject with RenderDoc **Mesh Viewer** on a bad-structure draw:
- **VS Input** positions garbage / out-of-range → **vertex fetch / vertex-format / endian-swap**
  bug (top suspect; Xbox 360 is big-endian — a mishandled vertex format or byte-swap corrupts
  positions for specific formats).
- VS Input fine but **VS Output collapsed** → **vertex-shader (DXBC) translation** bug for that
  shader.
- Geometry fine but black → fall back to pixel-shader / material (secondary).

Secondary hypotheses (only if Mesh Viewer shows geometry is fine): pixel-shader alpha output,
alpha-to-coverage/alpha-test, blend-enable derivation, texture-format decode (Xenia #1098).

**CONFIRMED via RenderDoc (2026-06-21).** A Python scan of 153 draws flagged exactly **1
suspicious draw — EID 319** (`DrawIndexedInstanced`, 1716 indices): post-VS clip-space output
X[-521550, 528733] Y[-162067, 310626] **W[-366877, 363423]** (Z sane [5.4, 34.7]). Huge garbage
X/Y/W = the mesh transforms to absurd positions → renders as the flat black sliver. Single
localized draw, not global. Z being sane while X/Y/W explode hints at a **position W / component
or stride decode** problem (e.g. position read as float4 picking up garbage in W) rather than a
totally random buffer.

**EID 319 detail (RenderDoc):** no D3D12 input layout (Xenia-derived backend pulls vertices in
the translated VS from a raw buffer — expected). The bound vertex stream is **VB res 2790,
stride = 8 bytes, offset 535344** → a **compressed position format** (8 bytes ⇒ likely
`SHORT4`/`SHORT4N` int16×4 or `HALF4` float16×4). VS Output SV_Position is garbage (huge X/Y/W),
so the shader's fetch/decode of that 8-byte format is wrong.

**Raw VB read (EID 319, VB res 2790, stride 8):** decodes cleanly as **little-endian** values in
[-1,1] (float32×2 ≈ ±1, or float16×4 ≈ ±2) — i.e. **this stream is byte-swapped correctly**, and
its values look like **texcoords/normal, not position**. So (a) the generic endian swap works,
and (b) RenderDoc surfaced a non-position shader-pulled stream; the position stream wasn't
captured. The garbage SV_Position magnitude (~±500k ≈ 32767×~15) points to either a
**normalized-int position read without normalization** or a **garbage transform matrix**.

**Constants read (EID 319, cb2):** large but **structured, not garbage** (clean rotation blocks,
world-translation values ~±50k; no NaN/inf). Constant-upload theory dropped (would be noise + hit
many draws). **SV_Position W spans −366877..+363423 (crosses zero)** → vertices land on both
sides of the camera plane → mesh stretches into the black sliver. So the vertex *transform/fetch*
is producing inconsistent garbage positions for this mesh.

The translator's per-format normalization table (`dxbc_translator_fetch.cpp:300-427`) looks
**faithful to upstream Xenia** (e.g. `k_16_16_16_16` signed-norm ÷(2^15−1) is present), so it's
not a naive missing-normalize. The bug is subtler — exact vfetch format / attribute parsing /
fetch-address path for this mesh's vertex format. This is an **SDK-level GPU bug** in the vendored
official ReXGlue; fix requires comparing ReXGlue's vertex-fetch translation to upstream Xenia.

**Shader analysis (EID 319 VS hash 6a632572, read from RenderDoc export `eid319_vs.txt`):**
position is **FMT_32_32_32_FLOAT, stride 48**, fetched as 3 raw dwords at `index*48 + base`
(`CB1[47].z`), endian-swapped, `w=1`, then matrix-multiplied by `CB2[5..8]` with the standard
Xenia "0×x=0" mul/mad semantics, then NDC-scaled. **The generated shader is faithful to upstream
Xenia — no decode/normalization bug.** (RenderDoc shows float immediates at low precision, so the
"l(0.000000)" endian masks are really small non-zero patterns like 0x00FF00FF, not zeros.)

**Conclusion: the corruption is in the DATA fed to a correct shader**, i.e. one of: the position
float bits in shared memory, the fetch base address (`CB1[47].z`), or the transform constants
(`CB2[5..8]`). All are produced by ReXGlue's **CPU side** (statically-recompiled game code +
command processor), not the GPU backend. This is a deep emulator-internals / recompiler-accuracy
divergence ("works in Xenia's interpreted CPU, not in a static recompile"), **not** an
AOT-layer or simple shader fix.

⚠️ **Unverified assumption:** we inferred EID 319 == the on-screen black sliver from the
position-bbox heuristic; we have NOT visually confirmed EID 319 is the artifact. Worth confirming
(RenderDoc "highlight drawcall" overlay) before deeper work — the visible "transparent" walls
might be different draws the position heuristic didn't flag.

**EID 319 was a PHANTOM (2026-06-21).** Saved the bound render target before (EID 318) and after
(EID 319) via `SaveTexture` → **identical images**, so EID 319 makes no visible change. Its huge
clip coords just mean it's transformed off-screen/clipped; it is NOT the on-screen artifact. The
position-bbox heuristic was misleading. (Saving the final backbuffer res 310 at the last event
returned blank white — last event is post-present; would need to target the present/last-draw
event to capture the rendered frame.)

**Conclusion / recommendation:** Bug 2 is a subtle, *selective* rendering divergence. We have
ruled out: every relevant cvar (depth/path/readback/blend/msaa), the blend-state translation, and
— for the one geometry lead we found — the vertex-shader translation (faithful to Xenia). The
remaining causes live in ReXGlue's CPU/data path (recompiler/command-processor feeding wrong
data) and a fix would be in the vendored official SDK (upstream), not the AOT layer. Combined with
the heuristic's only lead being a phantom, GPU-side RenderDoc archaeology has low ROI for a fix
*here*. **Recommended: treat Bug 1 (over-bright) as the shipped win; file Bug 2 upstream to
ReXGlue with this evidence, or park it documented.** The game is substantially improved and
playable (correct exposure).

NOTE: the auto-scan flagged only EID 319 (extreme garbage geometry). If "transparent objects"
persist after fixing 319, that may be a separate issue (the position-bbox scan wouldn't catch
alpha/material problems) — revisit then.

---

## TODO

### Bug 1 — over-bright/washed-out
- [x] Add `aot_hdr_readback` enum cvar (`auto`/`fast`/`full`/`off`) in `src/aot_app.cpp`.
- [x] Apply it in `AotApp::OnPreSetup` (`src/aot_app.h`) — overridable, no `HasNonDefaultValue`
      blind spot for the off-case.
- [x] Add `config/aot.example.toml` documenting `aot_hdr_readback`.
- [x] Add `rungame.ps1` switches `-ReadbackFull` (→ `--aot_hdr_readback=full`) and
      `-NoReadback` (→ `--aot_hdr_readback=off`).
- [x] Build + verify: washout gone by default (user-confirmed); `-NoReadback` reproduces.

### Bug 2 — see-through geometry (occlusion / depth)
- [x] Recheck after Bug 1: still see-through (characters + most solid objects). Black "bar" is a
      misrendered object, not a framebuffer artifact.
- [ ] Run depth-focused toggle bisect (rov, depth_float24_convert_in_pixel_shader,
      depth_float24_round, readback=full, depth_transfer_not_equal_test, invalid_fetch). Record.
- [ ] If a toggle fixes it → ship as AOT-layer per-game default; else escalate to GPU trace.

---

## Gemini research report review (2026-06-21)

Report (`docs/gemini-research.md`) thesis: artifact = **inverted occlusion** — UE3 uses GPU
occlusion queries; if the emulator fakes them ("always visible"), UE3 stops culling and draws
background structures through walls. Argues ROV-no-fix rules out depth precision (matches us) and
that Xenia fixed Army of Two via real ZPD queries (`occlusion_queries="fast"`).

Assessment:
- **Credible & untested:** the occlusion-query *direction*. Fits ROV-no-help, shader-faithful,
  works-in-Xenia, CPU-side. Would also make EID 319 (phantom) irrelevant.
- **Already disproven:** its other fixes — `depth_transfer_not_equal_test=true` (already default)
  and `depth_float24_convert_in_pixel_shader=true` (we tested: made it WORSE).
- **Confabulated specifics:** cvar names (`occlusion_queries`, `query_occlusion_sample_*_threshold`)
  and "commit fbd620c" don't exist in ReXGlue. Real cvars: `occlusion_query_enable` (bool, default
  true), `query_occlusion_fake_sample_count` (int, default 1000).

Code reality: ReXGlue implements **real** `EVENT_WRITE_ZPD` queries
(`d3d12/command_processor.cpp:175` + `BeginGuestOcclusionQuery`/`InitializeOcclusionQueryResources`
:4865), faking only if the query heap fails (logs WARN). **0 occlusion warnings across 216 logs**
→ real queries active. So if occlusion IS the cause, the real-query *results are inaccurate*
(false-positive visible) — a different but real SDK bug. Runtime `DisableHostOcclusionQueries()`
(:190/220/231) can silently switch to fake mid-frame (no log on those paths) — possible.

Decisive test (hot cvars, no rebuild), watch the see-through structures:
- A: `--occlusion_query_enable=false --query_occlusion_fake_sample_count=0` (force all-occluded).
- B: `--occlusion_query_enable=false --query_occlusion_fake_sample_count=100000` (force all-visible).
A removes structures / B worsens → occlusion is the lever → fix occlusion-query impl in
`tools/rexglue-sdk` on a local branch. Neither changes → occlusion not the cause; report wrong.

## OCCLUSION QUERY = confirmed lever (2026-06-21)

A/B test results (pavilion scene):
- Default (real queries) → structures **visible** (bug).
- `occlusion_query_enable=false query_occlusion_fake_sample_count=100000` (force visible) →
  identical to default.
- `occlusion_query_enable=false query_occlusion_fake_sample_count=0` (force occluded) →
  structures **GONE**, scene correct (but over-culls in other spots).

→ The structures ARE occlusion-gated, and the **real query path effectively returns "visible"**
for occluded geometry. Gemini report's *direction* (occlusion) validated; its specifics were not.

Code trace (`d3d12/command_processor.cpp`): real `EVENT_WRITE_ZPD` path waits via
`CheckSubmissionFence` (which DOES block, :3180-3187) for `query_submission = submission_current_-1`,
and `Signal(submission_fence_, submission_current_++)` (:3497) makes that the correct fence value.
So on paper it waits + reads the real result. Two remaining possibilities (opposite fixes):
- **(b) fallback hit**: `EndSubmission`/`BeginSubmission` returns false at runtime → bails to fake
  count (`write_fallback_result`). Easily fixable.
- **(a) real query returns visible**: occlusion proxy not depth-tested vs the occluder
  (depth/EDRAM state) → deep, likely upstream.

Decisive test pending: `--occlusion_query_enable=true --query_occlusion_fake_sample_count=0`.
Structures gone → (b); still there → (a). Then fix in `tools/rexglue-sdk` on a local branch.

**RESULT: case (b) CONFIRMED.** `occlusion_query_enable=true query_occlusion_fake_sample_count=0`
→ structures GONE. So real queries always fall back to the fake count.

**ROOT CAUSE (exact):** occlusion queries don't survive submission boundaries.
1. Guest begin → `BeginGuestOcclusionQuery` sets `active_occlusion_query_.valid=true`.
2. A submission boundary before the guest end → `EndSubmission` (`d3d12/command_processor.cpp:3457-3461`)
   ends the host query and **clears `active_occlusion_query_`** (no accumulation).
3. Guest end → `!active.valid` → `DisableHostOcclusionQueries()` (:231) sets
   `occlusion_query_resources_available_=false` **stickily**.
4. All later queries → `!resources_available_` (:177) → base fake path → 1000 ("visible") for the
   rest of the session. ⇒ default ≡ force-all-visible, uniformly. Matches all observations.

**FIX (on branch in tools/rexglue-sdk):** make queries span submissions — at a submission
boundary end+resolve the partial and mark resume-pending; on the next submission begin a fresh
host query; at the guest end, sum all partial sample counts and write the total (don't
sticky-disable). Files: `d3d12/command_processor.cpp` (EndSubmission, BeginSubmission,
EVENT_WRITE_ZPD end, EndGuestOcclusionQuery, BeginGuestOcclusionQuery, DisableHostOcclusionQueries)
+ `include/rex/graphics/d3d12/command_processor.h` (ActiveOcclusionQuery + partials list). Perf:
adds the per-query end stall that the fake path skipped; gated by `occlusion_query_enable`.

## FIX IMPLEMENTED (2026-06-21) — SDK branch `fix/aot-occlusion-query-spanning`

Cross-submission occlusion-query accumulation in
`tools/rexglue-sdk/src/graphics/d3d12/command_processor.cpp` (+ header struct/field). 7 edits:
1. Header: `ActiveOcclusionQuery.resume_pending` + `occlusion_query_partial_indices_` vector.
2. `EndSubmission`: at a boundary, end+**resolve** the host sub-query, push its index, set
   `resume_pending` (instead of discarding + clearing → which caused the sticky disable).
3. `BeginSubmission`: if `resume_pending`, begin a fresh host query in the new command list.
4. `BeginGuestOcclusionQuery`: clear partials/resume state for a new guest query.
5. `EndGuestOcclusionQuery`: end final sub-query if open, then **sum** all partials' sample
   counts (wait once for the last submission), write the total.
6. `EVENT_WRITE_ZPD` end guard: accept `resume_pending` as a valid open query.
7. `DisableHostOcclusionQueries`: clear partials.

SDK rebuilt+installed (exit 0) and AOT rebuilt (exit 0), no errors. Expected: occluded structures
correctly culled by accurate queries, WITHOUT the over-cull of `fake_count=0`. Perf: adds the
per-query end stall that the (broken) fake path skipped; gated by `occlusion_query_enable`.

**Status:** awaiting user verification (default launch, pavilion scene): structures gone AND
character/buildings retained; check framerate.

## Diagnostic run (2026-06-21) — spanning fix did NOT fix it; sticky disable from a mismatch

Instrumented build (`AOT-OCC-DBG` logs). Trace: build live ("real occlusion queries available …
spanning fix BUILD"), then at ~14s into the scene **`DisableHostOcclusionQueries()` fires once**,
followed by **1034+ "base/fake ZPD path (resources_available=false)"** — sticky disable → all
queries fake "visible" → structures rendered. Never reached a "REAL query result".

So the submission-spanning fix wasn't the (only) trigger; a different guard disables. The disable
is immediately followed by a d3d12 fallback → it's one of the ZPD address-mismatch guards:
**begin-mismatch (nested/overlapping queries — game has >1 query in flight, ReXGlue supports only
one)** or **end-mismatch (resume-logic bug)**. Added `DISABLE@{sample-null,begin-mismatch,
end-mismatch}` tags + addresses to identify which. Rebuilding to capture it.

## ROOT CAUSE CONFIRMED (2026-06-21): concurrent occlusion queries + sticky disable

Tagged-diagnostic run: `DISABLE@end-mismatch query_open=true valid=true resume=false
active_addr=1FA4F020 end_addr=1FA4F000`. The game **ends a query for one address while a
different query is the active one** (slots 0x20 apart = one `xe_gpu_depth_sample_counts` each) →
**UE3 runs multiple concurrent/overlapping occlusion queries**, ReXGlue tracks only ONE
(`active_occlusion_query_`), and on the first out-of-order end it **sticky-disables** all host
queries (`occlusion_query_resources_available_=false`) → 1300+ base/fake "visible" → structures
render through walls. (Submission-spanning was a real but secondary issue.)

D3D12 can't truly nest occlusion queries, so full concurrency is hard. **Band-aid attempt:** stop
sticky-disabling on a mismatch — fake just the odd query, keep the active query + host machinery
alive so well-behaved (non-overlapping) queries still get real results and occlude. Removed
`DisableHostOcclusionQueries()` from both ZPD mismatch guards; added a 128-partial safety cap for
queries that never get a matching end. If most structures use non-overlapping queries this should
fix them; if not, proper multi-query support (or the single-running-counter model) is needed.

## Cumulative ZPD rewrite (2026-06-21) — regression + fix

Rewrote D3D12 occlusion to the cumulative model (one continuous host query split into ordered
segments; each EVENT_WRITE_ZPD snapshots the running ZPASS total; game subtracts begin from end;
no begin/end pairing → handles concurrent queries). Branch `fix/aot-occlusion-query-spanning`.

**Regression:** with **deferred** writeback (write the exact value once segments resolve on GPU
completion), the menu went **black** (renders, no crash/hang — log shows clean shutdown). Cause:
the game polls the occlusion struct **same-frame** and treats the still-present 0xFFFFFEED "not
ready" sentinel as occluded → culls its own geometry → black. Deferred latency (~1 frame) isn't
tolerated.

**Fix:** make the writeback **synchronous** — at each EVENT_WRITE_ZPD, close the segment,
`EndSubmission` + `CheckSubmissionFence` (wait), `PumpZpdResolves` writes the exact cumulative
value to guest memory immediately, then reopen the segment. Value is present when the guest polls
→ no over-cull. (The previous real-query path also synced per query; this syncs per event.) Cost:
a GPU sync per EVENT_WRITE_ZPD — correctness first; optimize later (e.g. sync only on end events,
or a faster fence path) if perf is poor.

**Status:** built clean; awaiting user test — (1) menu launches, (2) pavilion structures occluded,
(3) perf. Fallback if still bad: `--occlusion_query_enable=false` restores the working fake path.

## Over-cull bug = uint32 clamp (2026-06-22)

Diagnostic confirmed the host occlusion query DOES count (raw segment counts up to 134M;
`resolved_total` grew past 153 billion). The over-cull came from `WriteGuestOcclusionResult`
**clamping** the cumulative to `UINT32_MAX`: the guest `xe_gpu_depth_sample_counts` fields are
32-bit, the cumulative running counter exceeds 4.29B within ~1s, so every snapshot saturated →
`end - begin = 0` → everything culled (character, menu buttons, all). Fixed by **truncating/
wrapping** to low 32 bits (the game's modular `end - begin` is correct for any single object's
delta < 2^32, matching real hardware's wrapping counter). Built; awaiting user test.

## Findings log

- **2026-06-21** — Root-caused Bug 1 to disabled HDR readback-resolve (Xenia parity:
  `d3d12_readback_resolve=true`). Confirmed `GetReadbackResolveMode` mapping. Ruled out the
  memexport-fast-path "bug" (false alarm). Separated the two symptoms; Bug 2 root cause still
  open. Implementing Bug 1 default (kFast) + kFull backup + rungame switches.
- **2026-06-21** — Implemented Bug 1 fix via `aot_hdr_readback` cvar (`src/aot_app.cpp` +
  `OnPreSetup` in `src/aot_app.h`), `config/aot.example.toml`, and `rungame.ps1`
  `-ReadbackFull`/`-NoReadback` switches. **Builds clean.** Runtime-verified: default D3D12
  launch logs "HDR readback-resolve enabled (kFast, auto default)" — the cvar is applied before
  GPU setup.
- **2026-06-21** — **Bug 1 confirmed FIXED by user** (exposure correct in screenshots). Bug 2
  persists. User clarified the black "bar" is an actual object rendering wrong (clipping
  through), and the see-through affects characters + most solid/opaque objects. Reframed Bug 2 as
  an occlusion/depth-buffer correctness problem (not alpha/blend, which is faithful to Xenia).
  Next: user runs the depth-focused toggle bisect (rov / depth_float24_* / readback=full).
- **2026-06-21** — **Toggle bisect ALL NEGATIVE** (6/6; `depth_float24_convert_in_pixel_shader`
  made it worse). Depth/path/readback hypotheses dropped. User refined: it's *specific walls &
  structures* that go transparent + clip through (selective, not global). Bug 2 is not
  config-tunable → needs a single-frame GPU capture (RenderDoc / native trace) to inspect the
  offending draw's render state. Deciding diagnostic path with user.
- **2026-06-21** — User screenshot shows the "black structure" is a **flat dark shape / thin
  horizontal black sliver clipping into a wall**. Reset theory to **corrupted/degenerate vertex
  data for specific meshes** (collapse → black sliver + apparent transparency of what they should
  occlude). Plan: RenderDoc Mesh Viewer on that draw (VS Input vs Output) to confirm and localize
  to vertex fetch/format/endian vs vertex-shader translation.


## new update
# Apply Xenia's Army of Two DepthBias clamp in the AOT build (the real fix)

## Context

Army of Two renders BSP level geometry / static meshes ("receivers" — walls, buildings,
structures) **through** other geometry. Xenia-canary fixes this with a runtime game-patch (the
"Flickering Decals Fix" in `docs/patch-army-of-two.md`) that **clamps the game's DepthBias to
-0.01f** for BSP receivers (guest `0x82894c44`) and static-mesh receivers (guest `0x82894800`).
This is the known-working fix; it works in Xenia-canary.

Why our prior attempts failed: we clamped `polygon_offset` (PA_SU_POLY_OFFSET register units) in the
GPU backend — the **wrong representation/units**. The game's bias is the `f0` value loaded inside
its own render-setup code, which is what actually drives the depth offset. Xenia patches that value
directly. The occlusion rewrite, depth-float24 convert, and ROV detours were all off-target.

We can't apply Xenia's runtime PPC patch in our **static (AOT) recompiler** (no XEX patcher —
`config.h:81-83` TODO, `codegen_context.cpp:50-54`; and the patch's "code cave" branch trick is
awkward for a static recompiler). But the patch's *effect* is trivial to replicate: force `f0 =
-0.01f` at those two recompiled instructions. The build already has a post-codegen generated-C++
patch mechanism for exactly this kind of thing.

## The exact targets (confirmed in `generated/default/aot_recomp.53.cpp`)

Both sites are the instruction `lfs f0,184(rN)` that loads the receiver DepthBias, generated as:
```cpp
    ctx.f0.f64 = double(temp.f32);
```
- **BSP receivers** `0x82894c44`: `lfs f0,184(r28)` (~line 15277), immediately followed by
  `// rlwinm r11,r11,0,7,3` then `// stfs f0,496(r1)` — use that context to make the match unique.
- **Static-mesh receivers** `0x82894800`: `lfs f0,184(r29)` (~line 14648), followed by
  `// rlwinm r11,r11,0,7,3` then `// stfs f0,432(r1)`.

Replacement: `ctx.f0.f64 = -0.01;` (matches Xenia's `0xbc23d70a` = -0.01f; the value is stored back
via `stfs`, truncated to float, so a double literal is fine).

## Plan

1. **Fast test (no regen):** in `generated/default/aot_recomp.53.cpp`, change the BSP-site
   `ctx.f0.f64 = double(temp.f32);` (the one between `REX_LOAD_U32(ctx.r28.u32 + 184)` and
   `stfs f0,496(r1)`) and the static-mesh-site one (between `REX_LOAD_U32(ctx.r29.u32 + 184)` and
   `stfs f0,432(r1)`) to `ctx.f0.f64 = -0.01;`. Build AOT only (`build-aot.ps1`, no `-Regenerate`).
   Run and check the buildings/structures no longer clip through walls.

2. **Make it durable (survives regen):** add one workaround to
   `scripts/apply-aot-generated-workarounds.ps1` (using `Replace-OnceOrThrow`, the existing pattern)
   for each site, with enough surrounding context (the `REX_LOAD_U32(... + 184)` line + the
   following `rlwinm`/`stfs f0,496(r1)` / `stfs f0,432(r1)`) to be unique. Re-run codegen +
   workarounds once to confirm it applies cleanly.

3. **Determine whether occlusion is still needed (cleanup):** with the depth-bias fix in, run
   `--occlusion_query_enable=false`. If the see-through stays fixed with occlusion faked off, the
   depth bias was the real cause all along and our ZPD occlusion rewrite was masking it — then
   retire it (default `occlusion_query_enable=false` for AoT, or revert `9c491aa`/`332a99a`), which
   also removes the flicker. If occlusion is still needed, keep it.

4. **Revert the dead-end backend clamp:** remove the uncommitted `depth_float24_polygon_offset_clamp`
   cvar from the SDK working tree (`flags.cpp`/`flags.h`/`pipeline_cache.cpp`) — wrong mechanism.

## Files
- `generated/default/aot_recomp.53.cpp` — the two `f0` overrides (fast test).
- `scripts/apply-aot-generated-workarounds.ps1` — durable post-codegen workaround (two segments).
- `tools/rexglue-sdk/src/graphics/flags.cpp`, `include/rex/graphics/flags.h`,
  `src/graphics/d3d12/pipeline_cache.cpp` — revert the polygon_offset_clamp cvar.
- (Conditional, step 3) occlusion default in `src/aot_app.*` or revert SDK commits.
- `docs/fix-visual-bug.md` / `docs/fix-occlusion-queries.md` — update root-cause narrative
  (DepthBias, replicated from Xenia's patch).

## Verification
- Build with the two `f0 = -0.01` overrides; at the pavilion/bridge scene the BSP/static-mesh
  geometry no longer renders through walls. Compare against the unmodified build (revert the two
  lines) to confirm this change is what fixes it.
- Run `--occlusion_query_enable=false` to learn whether occlusion is still load-bearing.
- Confirm the bridge pillars (partially-visible) are now occluded correctly and no flicker.
