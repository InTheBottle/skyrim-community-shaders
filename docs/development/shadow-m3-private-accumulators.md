# Shadow M3 — Private per-light accumulators

Status: **design in progress** (2026-07-15). Chosen by the user after the enumeration-rebuild
skip-Func42 path was proven non-viable over the shared accumulator. This doc captures the proven
facts; the concrete staged architecture is filled in from the RE plan (see "Architecture" below).

## Why we are here — the verdict that forced this route

`kEnumInstance` (mode 13) tried to render Skyrim's per-light shadow casters from our own
enumeration and **skip the engine's fused Func42 render**. Two findings closed that door:

1. **The enumeration itself is correct and rock-stable.** `DirectReadEnumerate` reads the
   accumulator's caster chains and returns `inst=745 remainder=124` every frame in Dragonsreach
   (Save345) at 235 fps in the safe observational mode (`[EnumObserve]`, engine renders+frees).
   Reading the chains post-cull is safe.

2. **Skipping Func42 freezes the game within ~1 frame.** Func42's walk frees the pass nodes
   incrementally (`BSBatchRenderer::sub_141307E80` @ 0x141307E80 → `NiMemFree(node,16)`, gated on
   `m_AutoClearPasses` at `br+0x6C`). There is **no per-frame bulk pool reset** to recover: a
   "skip corrupt frame, engine-passthrough next frame" latch still froze (PresentMon FPS=0, log
   stops, process alive). Once a chain is left un-freed it cycles (`passGroupNext` self-links);
   our bounded walk (2^17/chain, 2^19/accum) rejects it, and handing it to the engine Func42
   hangs the engine's own walk. This is the **same shared-pass-node-pool wall that blocked
   kWalkMT** (mode 12 concurrent), reached from the enumeration angle.

Measured contrast (Save345 Dragonsreach):

| Path | enumeration | FPS | Result |
|------|------------|-----|--------|
| Observe (engine frees) | `inst=745` stable every frame | 235 | ✓ correct, no freeze |
| Skip (we don't free) | `inst=745→1292` growing | 0 | ✗ freezes in ~1 frame |

**Conclusion:** the engine's incremental free is load-bearing and cannot simply be skipped. The
instancing draw-collapse win is already shipped via `kInstance` (mode 9), which keeps the engine
free intact. The only way to skip the engine walk is to **own the pass-node lifecycle** — private
per-light accumulators with their own node pool and their own free.

Commit: `2bab55a0` (bounded walk + safe observational default + `CS_SHADOW_ENUM_SKIP=1` harness).

## Proven layout facts (reuse these; all confirmed this session)

- ShadowMapData (RenderShadowmap `desc`, a2): camera=`desc+0x40`, accumulator=`desc+0x48`,
  target=`desc+0x54`, slice=`desc+0x58`. `renderMode = accum+0x150` (0 main / 13 spot /
  14 dir-cascade / 15 parabolic).
- BSBatchRenderer = `accum+0x130` (= BSShaderAccumulator `_pad_8[296]`). Chain walk:
  `passArrayBase=*(br+8)`, `hashtable=*(br+0x48)`, `cap=*(br+0x2C)` (pow2), `sentinel=*(br+0x38)`;
  16B hash entry `{key@0, groupIdx@4, next@8}`; head `= *(passArrayBase + 8*(slot + 6*groupIdx))`
  for slot 0..4; walk `pass->passGroupNext` (BSRenderPass `+0x30`). Free-list head at `br+0x60`,
  `br+0x58` scratch, `m_AutoClearPasses` at `br+0x6C`.
- Instanceable subset = non-alpha-test (slot ∉ {1,3,4}), whole-TRISHAPE (geom`[0x150]==3`),
  non-skinned (geom`+0x130`==0). ~85% of casters in dense scenes.
- Func42 (0x1412CAC20) = `SetRenderMode(accum+0x150)` then `table[renderMode](accum,flags)`
  (table 0x1431D1C40, runtime-populated → static reads 0). Func43 (0x1412CAC90) = shadow teardown.
- Cull: CalculateAndDrawShadowCasterLights (0x1412E2660) → ListAccumulationJob (0x1412E2DE0,
  BSJobs-parallel) → RegisterObject (BSShaderAccumulator::sub_1412CA110, 0x1412CA110) dispatches
  `unk_1431D1B40[renderMode]` per caster. Per-frame reset ResetCalculatedShadowCasterLights
  (0x1412E2E10) → ShadowSceneNode::ClearShadowCasterArray (0x1412BC7F0) nulls the
  `shadowCasterLights` array only (NOT the pass nodes).
- Per-light RenderShadowmap = 0x141305610; NiCamera::Render = 0x1412C15C0 → sub_1412C1600
  (SetCameraData + Func37 + Func42) then Func43.

## Reusable building blocks (already byte-exact / shipped)

- `UtilityPassReplica::DirectReadEnumerate` — bounded, cycle-safe chain read → instanceable
  (pass+tech) + remainder. Returns false on a cyclic/oversized chain.
- `UtilityPassReplica::RenderShadowInstanced` — groups claimed passes by (mesh,technique) and
  issues one DrawIndexedInstanced per group (mode 9's win).
- `UtilityPassReplica::BeginPassReplica` / `ReplicaRenderPassImmediately` — byte-exact per-pass /
  per-group DX11 replica (mode 5/6 validated) for the non-instanceable remainder.
- `MakeShadowWorker` / `WorkerBeginScope` / `WorkerSeedMap` — per-worker private block + deferred
  context (mode 3/4).

## The pivotal RE question (sizes the whole build)

Is there a callable **BSBatchRenderer Clear/Reset** that frees all pass chains back to its own
pool (recycles the 16B nodes, resets passArray heads + hash table + free-list + counts)?
- YES → M3 is SMALL: our instanced-draw + remainder, then call reset (keep the pool healthy).
- NO → M3 is LARGE: construct fully private BSShaderAccumulators, redirect the cull into them,
  enumerate on workers, reset them ourselves each frame.

## Architecture

_(To be filled from the RE plan — staged M3.0 single-light → M3.1 all-lights → M3.2 workers,
with exact hook addresses, the free/reset mechanism, and per-stage atlas-hash validation gates.)_
