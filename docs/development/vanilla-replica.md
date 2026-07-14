# The Vanilla Replica System

A reimplementation of Skyrim SE's shadow-map rendering (`DrawWorld::RenderShadowmaps`,
SE 1.5.97 `0x1412E3480`) with byte-exact validation layers at every level, plus the
optimized instanced submission that is its first performance payoff. This document is
the system boundary: what belongs to the replica, how it is validated, and the recipe
for extending it to the next engine pass (z-prepass).

## Module map

| Piece | File(s) | Role |
| --- | --- | --- |
| Replica core | `src/UtilityPassReplica.cpp/.h` | 1:1 reimplementations of the engine's per-pass pipeline: `BeginPassReplica` (BSBatchRenderer::BeginPass `0x141308030`), `ReplicaRenderPassImmediately`, the three setup reimpls (`FlushSetupTechniqueReplica`, `FlushSetupGeometryReplica`, alpha setup), `FlushDirtyStatesReplica` (SetDirtyStates `0x140D705B0` against a caller-supplied state block), and the per-object draw dispatch (`DrawTriShapeReplica`, sub-index, skinned). |
| Instanced submission | `UtilityPassReplica::RenderShadowInstanced` | The optimized path: groups claimed passes by (mesh `rendererData`, technique, fade), fills one FP16 instance stream per map with a single `Map(DISCARD)` (F16C `_mm_cvtps_ph` pack), hoists technique/geometry setup to once per (tech,fade) run, and issues one `DrawIndexedInstanced` per group via `StartInstanceLocation`. |
| Mode driver + hooks | `src/ShadowThreaded.cpp/.h` | `CS_SHADOW_MT` modes (see below), the `RenderShadowmaps`/`RenderShadowmap`/`BeginPass` detours, the per-map claim bracket (`CaptureHook`), parallel-cull machinery, and the state-leak validator. |
| Draw-state fingerprints | `src/Vanilla/DrawState.cpp/.h` | Per-draw GPU state fingerprint + compare (mode 7 verification). |
| Shader reflection | `src/Vanilla/ShaderReflect.cpp/.h` | Used-byte / used-slot masks so state compares only check bytes the bound shaders actually consume. |
| Shader side | `package/Shaders/Utility.hlsl` (`INSTANCED`) | The instanced VS permutation: per-instance World from TEXCOORD4-7 (slot-1 stream), substituted upstream of the parabolic/clamped code so every shadow variant stays correct. **Any edit here requires a shader deploy — the game compiles from its own `Data/Shaders`, not the repo.** |

## Modes (`CS_SHADOW_MT` / devbench `communityshaders.shadowmt`)

| Mode | Name | Purpose |
| --- | --- | --- |
| 0 | off | Vanilla. With `CS_SHADOW_TIMING`, wall/CPU-times the untouched walk. |
| 1 | capture | Observe + log the per-map pass partition (no behavior change). |
| 3 | worker-serial | Replay every map's claimed passes through one private-block worker. The correctness reference for the replica render path. |
| 4 | concurrent | N worker threads, one deferred context each (validated crash-free; perf still bounded by submission). |
| 5 / 6 | BeginPass ownership / + verify | Full BeginPass replacement; mode 6 runs the engine-vs-replica command compare (validated 1.63M commands / 0 divergence). |
| 7 | draw-state verify | Per-pass engine-vs-worker GPU draw-state diff (needs `CS_RE_REFLECT`). |
| 8 | parallel cull | Fans the per-map cull walks (`Func42`) out to a worker pool; submits serially. ⚠ The walk can itself DRAW (skinned/dismember casters) — see "known hazards". |
| 9 | **instanced** | The production path: parallel-cull off, instanced submission on. −69 % shadow draws, −30 % utility submission CPU, +17 % FPS in shadow-bound scenes; pixel parity at noise floor. |
| 10 | instanced+cull (experimental) | Mode 8 + mode 9 combined. **Crashes**: cull-walk draws on workers race the immediate context (`crash-2026-07-13-23-49-01`). Kept env-gated for future work; requires walk-time draw suppression or accumulator classification first. |

## Validation layers

1. **Command compare (mode 6)** — every context call the replica issues is recorded through
   the out-of-module stub and compared 1:1 against the engine's recording of the same pass.
2. **Draw-state fingerprints (mode 7)** — full GPU state readback per draw, masked by
   `ShaderReflect`'s used-slot info, engine vs replica.
3. **Instanced command validation (always on, `shadowmt action=instval`)** —
   pass-conservation invariants per map (`claimed == grouped + drops`,
   `grouped == instanced + fallback`, `drawn == groups`), a ring of the most recent
   `DrawIndexedInstanced` commands, and a throttled byte-compare of the F16C instance pack
   against the engine's scalar reference path.
4. **Post-RenderShadowmaps state-leak validator (`shadowmt action=stateval`, or
   `CS_SHADOW_STATE_VALIDATE=1`)** — snapshots ~20 context-state fields at every
   `RenderShadowmaps` **entry**. Vanilla frames teach a per-field stability baseline;
   replica frames verify against it. Entry state reflects the *previous* frame's full
   pipeline, so a divergence means the replica leaked state the engine did not recover
   from — exactly the class that once corrupted main-scene textures. A self-test hook
   (`CS_STATEVAL_SELFTEST=1`) deliberately desyncs the GPU rasterizer from the engine's
   CPU model each mode-9 frame; the validator must fire, proving the detector sees.
5. **Visual sweep** — frozen mode-0 vs mode-9 screenshot pixel-diff (threshold: mean < 1.0/765,
   >30-diff pixels < 0.05 %) across ≥50 teleport locations (`F:\claudetmp\rtprof\sweep50.ps1`),
   plus temporal burst checks (consecutive-frame diffs; flashing shows as spikes).

## Known hazards (hard-won)

- **Stale shader deploys**: the game compiles HLSL from `Data/Shaders`. A DLL-only deploy after
  editing `package/Shaders` silently tests old shaders — the `INSTANCED` define compiles to a
  no-op plain VS and every instance renders at the group representative's position.
  Verify with a grep of the deployed file before trusting any shader-behavior test.
- **Per-object state in batches**: anything SetupGeometry varies per object (the stencil-dither
  fade token, alpha-test refs) must be part of the group key or hoisted per run — it cannot vary
  inside one `DrawIndexedInstanced`.
- **`VA_INSTANCEDATA` IL encoding**: the IL key must carry ONLY bit 63 (slot-1 presence).
  Bit 53 or the offset nibble pins the instance elements to slot 0 and corrupts the layout.
- **The cull walk can draw** (mode 8/10 hazard): `Func42` renders skinned pre-passes for some
  accumulators; those walks must never run off the render thread while the immediate context
  is in use.
- **Engine CPU-model coherence**: the engine skips re-binding state its block believes is
  current. Any GPU state changed behind the block's back must be either restored or the block
  force-dirtied — otherwise the leak surfaces frames later in unrelated passes.

## Extending to the z-prepass (next)

The z-prepass renders the same Utility-shader depth-only passes as shadow maps
(`RENDER_DEPTH` techniques instead of `RENDER_SHADOWMAP`), in the main view:

1. **Hook point**: the depth-prepass accumulator submit inside the main `DrawWorld` render
   (the `Depth pass N (DS:...)` regions visible in annotated captures). Bracket it exactly
   like `RenderShadowmapDetour` brackets a shadow map: open a map-work entry, arm the claim
   hook, run the original, render the claimed subset instanced.
2. **Reuse as-is**: `CaptureHook`'s claim gate (whole-TRISHAPE, non-alpha-test),
   `RenderShadowInstanced` (grouping, one-Map fill, per-run setup, `StartInstanceLocation`
   draws), the instanced VS permutation (`RENDER_DEPTH | INSTANCED` compiles from the same
   `Utility.hlsl` edit), and every validation layer above (instval invariants apply verbatim;
   the state validator's entry-compare covers any new leak automatically).
3. **New work**: the per-pass state the z-prepass varies that shadows do not — notably
   `RENDER_DEPTH` uses alpha-test more broadly and feeds early-Z; audit `SetupGeometry`'s
   `RENDER_DEPTH` branches for per-object state to fold into the group key (the fade-token
   lesson). Validate with mode-6-style command compare on the prepass bracket before
   measuring performance.
4. **Order of proof (non-negotiable, learned the hard way)**: per-pass replica parity first
   (a mode-3 analogue), then instancing, then perf. Visual parity gates every step; commit at
   every validated milestone.
