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

## The pivotal RE question — ANSWERED: YES, M3 is the SMALL variant

There **is** a callable reset: **`BSBatchRenderer::sub_141306DB0` (0x141306DB0)**, signature
`void(BSBatchRenderer*)`. Confirmed by decompilation: it iterates the batch renderer's hash
container (`br+0x20`) and zeroes every live group's 0x30-byte slot-head block (`passArrayBase +
0x30*groupIdx`), then drains the group-key list (`br+0x60`) back to the global 16B node pool
(`unk_143490BC0`) via `FUN_141308FE0`, and clears `br+0x58`. It runs under a TLS-local alloc-tag
guard (`TEB[…]+1896` save/set/restore), so it is safe to call on the render thread. It is the same
primitive the batch-renderer ctor (0x141306340) and teardown (0x141306C30) use. **No private
accumulators needed** — we reuse the per-light accumulator the engine already has, render from it,
then reset it ourselves.

**Corruption mechanism (exact):** the freeze was a `passGroupNext` **self-cycle**. Shadow-caster
passes are cached on the shader property (`prop->vtable[0x158]` returns the cached list). If a
slot head is not cleared, next frame's sorted insert (`Func1`, 0x141306FD0) walks the slot chain,
breaks immediately at the still-present head `P` (equal depth), and sets `*slotHead = P;
P->passGroupNext = P` → self-cycle. Our bounded walk rejects it; handing it to the engine hangs
its walk. Clearing the slot heads (the reset, or the engine's incremental `BeginPass` zeroing)
makes the next insert start from an empty slot → no cycle. That is why the free is load-bearing.

## Architecture (built + validated)

Hook: the existing `kEnumInstance` (mode 13) Func42 detour (0x1412CAC20), scoped to the shadow
phase (`g_inEnumShadowPhase`) + a 600-frame in-world settle gate. Sub-behavior via env:
`CS_SHADOW_M3=0a|0b`. `EnumInstanceM3` reads `renderMode = accum+0x150` and returns a "handled"
bool; the thunk runs the engine Func42 when it returns false.

- **M3.0a** (`CS_SHADOW_M3=0a`, ✅ commit 08a59bf3): skip the engine render, call
  `sub_141306DB0(br)` only. Gate: game stable, `[EnumM3]` inst count CONSTANT (744, not growing
  745→1292), persistent lists empty. Proved the reset heals the pool → SMALL variant confirmed.
- **M3.0b** (`CS_SHADOW_M3=0b`, ✅ commits 2b72500f, 69ffbb30): `DirectReadEnumerate` →
  `RenderShadowInstanced` (instanceable subset) + `ReplicaRenderPassImmediately` (remainder;
  DirectReadEnumerate now emits remainder tech+alpha) → `sub_141306DB0` reset. At Func42 entry the
  map's RT/DSV/viewport are still bound, so we render onto the live state under a block
  save/force-dirty/restore. Shadow depth is order-independent, so the hash-bucket enumeration order
  is fine. Gate: same-boot frozen-animation toggle mode 0↔13, scene pixel-diff == the
  vanilla-vs-vanilla drift floor (interior 0.210 vs 0.205; exterior 5.22 vs 5.20) → pixel-perfect.
- **renderMode gate** (✅ commit 69ffbb30): shadow maps are a MIX of 13 (spot) / 14 (dir-cascade) /
  15 (parabolic) in both interiors and exteriors. `EnumInstanceM3` owns ONLY mode 13 and returns
  false (→ engine) for the rest. Directional cascades reference passes across MULTIPLE Func42 calls,
  so our per-call reset frees a pass a later call renders → engine `BeginPass` AV (0x141308707) on
  the exterior. Gating to 13 makes M3.0b crash-free + pixel-correct everywhere.

**Perf:** M3.0b is NEUTRAL vs vanilla on this co-bound RTX-4080 rig (render thread, ~295fps
interior) — the same co-bound ceiling as all shadow-MT work here; the instanced draw-count cut is
hidden behind the GPU. The correctness foundation (own render + own free, pixel-perfect) is the
milestone.

## Mode-14 directional cascades (opt-in `CS_SHADOW_M3_OWN14=1`, INCOMPLETE)

Investigated via a 4-lens RE workflow + live crash bisect. Findings:
- Mode 14 shares ONE accumulator (`0x9f6bc280`) across N cascade Func42 calls, unlike mode 13's
  private per-light accumulator. The per-Func42 `sub_141306DB0` reset drains its nodes to the global
  pool, so a later cascade renders a recycled pass → engine `BeginPass` null-deref (RBX=0,
  `SkyrimSE+0x1308707`). Skipping the reset instead LEAKS the chain into a self-cycle (grows to 2^17).
- **Fix that works (main groups): frame-end reset.** Collect mode-14 batch renderers during the light
  loop (dedup'd), reset each ONCE after the whole loop (`callOriginal`). Validated: no crash, no
  main-group leak, pixel-diff vs vanilla = drift floor on the sparse exterior Save344. Commit 9ac82f52.
- **Two remaining blockers on dense/realistic scenes:**
  1. **Persistent-list leak.** `br+0x78/0xB8/0xE8` (blood/lowaniso/decal/VL lists) grow every frame
     (observed B8 38980→43180, E8 91604→93404) — `sub_141306DB0` does NOT drain them and we skip the
     engine's `RenderPersistentPassList` (0x141306240). Needs a persistent-list drain at reset.
  2. **Dense-caster crash.** `coc WhiterunExterior01` (grass+trees) → garbage-vtable call
     (jumped to `0x7EEECB80`): a directional-unique `BSInstanceGroup`/grass caster whose per-instance
     world is NOT a single matrix at `geom+0x7C`, so `RenderShadowInstanced` reads a bad pointer. Needs
     caster-type gating (reject BSInstanceGroup/BSSegmentedTriShape) + let the engine render those.
- Full mode-14 ownership therefore needs: **snapshot-by-value** at enumerate time (own the pass fields,
  don't deref live `BSRenderPass*` across a cascade), **group-layout-flag-aware walk** (only flat-walk
  groups with `groupHead+0x26 & 1`), **caster-type gating**, and **persistent-list draining**. Given the
  perf is co-bound-neutral, this is a large effort for correctness completeness, not FPS.
2. **M3.2 workers.** Move `DirectReadEnumerate` + (mesh,tech) batching onto worker threads; keep
   the instanced + remainder draws + reset on the render thread. Note the ceiling is small: the
   enumerate is cheap (~microseconds over ~1400 passes) and the DX11 draws are inherently
   render-thread-serial (shared immediate context), so the CPU moved off-thread is modest.
3. **CPU-bound measurement.** Measure M3.0b (+ mode 9) with SSGI off / on a weaker GPU regime,
   where the instanced draw-count cut can actually surface as FPS.
