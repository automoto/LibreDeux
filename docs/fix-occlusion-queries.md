# Fix: Army of Two see-through structures — D3D12 occlusion query rewrite

**Branch:** `fix/aot-occlusion-query-spanning` in `tools/rexglue-sdk/` (the vendored official
ReXGlue SDK, its own git repo).
**Files:** `src/graphics/d3d12/command_processor.cpp`,
`include/rex/graphics/d3d12/command_processor.h`.
**Status:** WORKING — menu launches, world/character render, occluded structures hidden on entry.
Known remaining issue: structures **intermittently pop back in while moving** (see "Known issue").

## The bug

Army of Two (Unreal Engine 3) rendered background walls/structures **through** foreground geometry
(see-through / clipping). Root cause: the game issues **many concurrent/overlapping GPU occlusion
queries** (each with its own `RB_SAMPLE_COUNT_ADDR`). ReXGlue's D3D12 backend tracked exactly
**one** query at a time as begin/end pairs; on the first out-of-order end it called
`DisableHostOcclusionQueries()` which **stickily** disabled all host queries for the rest of the
session, so every query fell back to the fake "always-visible" sample count → occluded geometry
was never culled. (Confirmed via instrumented logs: `DISABLE@end-mismatch` then 1300+ fake-path
events.)

## The correct model (cumulative ZPASS counter)

Per the `xe_gpu_depth_sample_counts` comment (`include/rex/graphics/xenos.h`) and Xenia-canary's
`D3D12ZPDQueryPool`: the Xbox 360 has **one global ZPASS sample counter**. Each `EVENT_WRITE_ZPD`
writes the **current cumulative count** to its struct; the game computes occlusion as
`end_snapshot − begin_snapshot`. Snapshots of a single running counter need no begin/end pairing
or address matching, so this handles arbitrary concurrent queries natively.

## Implementation (D3D12 only; Vulkan unchanged)

A single host D3D12 occlusion query runs continuously, split into ordered **segments**:
- `OpenZpdSegment()` / `CloseZpdSegment()` — begin/end+resolve a host query; closed segments are
  queued (`zpd_segments_`, with their submission number) for readback.
- `BeginSubmission` opens a segment; `EndSubmission` closes it (so each segment stays within one
  command list); `CheckSubmissionFence` completion calls `PumpZpdResolves()`.
- `PumpZpdResolves()` reads completed segments in order, accumulates `zpd_resolved_total_`, and
  flushes pending snapshots (`zpd_snapshots_`) once their segments resolve.
- `ExecutePacketType3_EVENT_WRITE_ZPD` (rewritten): close segment, record a snapshot for the
  guest address, **resolve synchronously** (`EndSubmission` + `CheckSubmissionFence` + pump) so the
  exact value is in guest memory immediately, reopen segment. No begin/end pairing, no sticky
  disable.
- Fallback to the base fake path via `ZpdActive()` when host queries are unavailable **or on the
  ROV render path** (a fixed-function occlusion query can't count pixel-shader-interlock depth).
  `occlusion_query_enable=false` also forces the fake path (escape hatch).

## Two critical fixes found during bring-up

1. **uint32 WRAP, not clamp** (`WriteGuestOcclusionResult`): the guest counter fields are 32-bit
   and the game does modular `end − begin`. The cumulative total exceeds 4.29B within ~1 second, so
   **clamping** to `UINT32_MAX` saturated every snapshot → `end − begin = 0` → *everything* culled
   (black menu, invisible character). Must **truncate to the low 32 bits** (wrap) — matches the
   real hardware counter; a single object's delta is always < 2³².
2. **Synchronous, not deferred, writeback**: writing the value only when segments resolve on GPU
   completion (deferred) left the `0xFFFFFEED` "not ready" sentinel in guest memory when the game
   read it → the menu went black. Resolving synchronously per event keeps the exact value present
   when the guest polls. (Costs a GPU sync per `EVENT_WRITE_ZPD`.)

## Known issue (next to address)

Structures occasionally **pop back into view while the camera moves**. Likely the per-event GPU
sync makes the command-processor thread lag the game under heavy movement, so some occlusion
results aren't ready when read. Candidate next steps: reduce sync frequency (e.g. only on
end-type events), exclude emulation-internal transfer/resolve draws from the count, or a
bounded best-effort writeback. **Do not regress the menu** while addressing this.

## Verify / escape hatch

- Build: `scripts/build-rexglue-official.ps1` (retry past the intermittent generated-header race),
  then `scripts/build-aot.ps1`. Run: `./rungame.ps1 -D3D12 -StopExisting`.
- `--occlusion_query_enable=false` restores the old fake-visible behavior (structures show, but
  everything renders) if the new path misbehaves.
