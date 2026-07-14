# Shadow walk parallelization — native +7.5–12% (the max shadow lever)

Status: **RE-complete; the pre-walk direct-read design is INVALIDATED by in-engine testing (see
"CRITICAL FINDING" below); a corrected approach is needed.** Every offset here is verified against
the legit unpacked SE-1.5.97 db (`G:\IDA Projects\skyrimcutter`) and CommonLibSSE-NG headers,
cross-checked with `D:\GitHub\offsets-1-5-97-0.csv` (addr→AddrLib-ID). No guessed IDs.

## CRITICAL FINDING (2026-07-14, mode-10 `kInstanceOwnVerify` harness)

The `kInstanceOwnVerify` mode direct-reads the accumulator at **Func43 entry** and compares its
instanceable count to CaptureHook's. Result: **directReadInst=0 every map** (diverged ~100%). The
one-shot diagnostic pinned the cause precisely:

```
[DirectRead] br=<valid> cap=16 occBuckets=10 rawWalked=0 inst=0 | ggNonNull=16 ggViaNext=0 ggViaPGN=0
```

- `occBuckets=10`: the `renderPassMap` technique registry IS populated (structure/offsets correct).
- `rawWalked=0`: the `renderPass` hash-array pass chains are **empty**.
- `ggNonNull=16 / ggViaNext=0 / ggViaPGN=0`: the `geometryGroups[16]` pass-lists are **empty too**.

**The batchRenderer holds NO materialized passes at Func43 entry.** `RenderBatches` (0x1412CCE40)
materializes each group's passes from the culled geometry, walks them, and clears them — transiently,
per group — so the passes exist ONLY during the walk (exactly where CaptureHook taps them). There is
no clean pre-walk point to enumerate them.

**Consequence:** the "skip Func43 walk + self-contained direct-read on a worker" design below does not
work as written. Parallelizing the walk must instead be one of:
1. **Capture during the walk** (CaptureHook, serial — the current mode-9) and parallelize only the
   downstream build — but that build (0.26ms across 7 uneven maps) was already measured net-negative.
2. **MT the walk itself** — run RenderBatches/BeginPass on workers with the shared render-state
   globals (S-block `0x143027EB0`, technique caches) localized per worker (the kConcurrent machinery,
   which works but was deferred-context-bound → fails ExecuteCommandList on native).
3. **Read the culled geometry pre-walk and replicate geometry→pass** (technique selection etc.) — the
   most work, effectively re-implementing RenderBatches' materialization.

Combined with the Amdahl ceiling (below), the shadow lever is both **bounded (~+7.5–12% max over dev)
AND hard to realize** — there is no cheap correct direct-read. The mode-10 harness (retained) is the
proof; `DirectReadInstanceableCount` is kept for a future mid-walk hook point.

## Why this, and the hard ceiling (measured, not estimated)

Dragonsreach native, 7 maps / 6896 instanced passes: `RenderShadowmaps` = **1.63 ms = 31.9 %**
of the 5.10 ms frame (live profiler, n=128). Amdahl caps the whole-frame gain:

| | frame | fps | vs dev 223.7 |
|---|---|---|---|
| branch now (mode 9) | 5.10 | 196 | −12% |
| shadow walk+build parallelized | 4.16 | 240 | **+7.5%** |
| + branch non-shadow regression removed | 3.53 | 283 | **+26.6%** |
| shadow→0 (impossible) | 3.47 | 288 | +28.8% |
| **target +30%** | 3.44 | 291 | — |

**+30% over dev is beyond the ceiling of shadow work** (shadow is 32% of frame; even shadow→0 =
+28.8%). User acknowledged this and chose "max out shadows anyway" → this doc's ~+7.5–12%.

## Where the 1.63 ms goes (walk-split, this session)

- **walk (engine RenderGeometryGroup→BeginPass→RenderPassImmediately + CaptureHook collect) = 0.68 ms/frame** — serial, render thread. Dominated by cold-line traversal of `m_PassGroupNext` linked lists (~10174 passes, LIFO-freelist-scrambled arena → cache miss per hop; pass-prefetch mitigates, can't remove). **Parallelizable across maps.**
- **RSI (group+fill+draw) = 0.57 ms/frame**: fill (F16C matrix pack) ≈0.26 parallelizable, group ≈0.18 parallelizable, draw ≈0.13 render-thread-only (immediate ctx).
- Movable to workers: walk 0.68 + group 0.18 + fill 0.26 ≈ **1.12 ms**. Residue: draws + per-map setup + join ≈ 0.5 ms.

The easy lever (parallelize just fill across maps) was already tried → **net-negative** (7 uneven
maps, tiny per-map work, dispatch overhead). The walk is the only lever that moves the needle, and
it needs ownership.

## The engine structures (all verified)

- `BSShaderAccumulator + 0x130` → `BSBatchRenderer*` (CommonLib `batchRenderer`; RE `_pad_8[296]`).
- `BSBatchRenderer + 0x08` → `renderPass` = inline `PassGroup[]`, **stride 0x30** (RE `*(base+8*(mode+6*grp))` == `PassGroup[grp].passes[mode]`, PassGroup=0x30). `passArrayBase = *(batchRenderer+8)`.
- `BSBatchRenderer + 0x20` → `renderPassMap` = `BSTFixedHashMap<uint32 techniqueID, uint32 groupIndex>`.
- `PassGroup.passes[mode]` (mode 0..4) = chain head (`BSRenderPass*`); chain via `BSRenderPass + 0x30 = passGroupNext`.
- `BSRenderPass`: shader@0x00, geometry@0x10; geom type @ geom+0x150 (==3 whole-trishape), skinned @ geom+0x130 (!=0), rendererData @ geom+0x138, triCount @ geom+0x158.
- **technique = the map key** (engine calls `RenderPassImmediately(pass, *a2=key, v11, flags)`). alphaTest v11 = f(mode): true for modes 1/3/4, false for 0/2. renderFlags = Func43 `a2`.
- Instanceable subset (matches CaptureHook): whole-tri (geom+0x150==3), non-skinned (geom+0x130==0), non-alphaTest → **modes 0 and 2 only**.

AddrLib IDs (1.5.97): RenderShadowmap 100820, NiCamera::Render 99789, Func43 99937 (0x12CAC90),
submit sub_1412CB2E0 99939, RenderGeometryGroup 99963, BeginPass 100852 (0x1308030),
RenderPassImmediately/SetupAndDrawPass 100854, key-iter sub_141307DD0 100849.

## The design — mode `kInstanceOwn` (10), then `kInstanceMT`

**Foundation already built + validated:** `UtilityPassReplica::BeginPassReplica` reads these exact
chains (`passArrayBase = *(a1+8); pass = *(passArrayBase + 8*(grp+6*v6)); walk passGroupNext`) and
is proven byte-exact (mode 6 `BeginPassCompare`, DIVERGED=0 / 1.63M). Mode 5 renders the whole walk
from it. So the direct-read is DONE; this reuses it.

### Serial first (kInstanceOwn, mode 10) — prove skip-walk correctness + measure
Hook Func43 (99937; hook struct already exists for kParallelCull). For mode 10, per map:
1. Engine already did setup + cull before Func43 → accumulator's renderPass chains are populated, DSV/viewport live. **Skip the original Func43** (no engine walk → groups not cleared).
2. `DirectReadInstanceable(accum, flags)`: iterate `renderPassMap` → {tech, grp}; for mode∈{0,2}: walk `passArrayBase[grp].passes[mode]` chain; **partition**: instanceable → `(pass,tech,false,flags)` list; everything else (modes 1/3/4, sub-index, skinned, non-trishape, !CanReplicate) → non-instanceable list.
3. Force-dirty S-block (sflags[0]=0xFFFFFFFF), reset technique caches (mirror `RenderMapInstanced`).
4. Render **non-instanceable per-pass** via `ReplicaRenderPassImmediately` (or engine RenderPassImmediately) — depth is order-independent so batching separately is safe. **CRITICAL: skipping these = missing shadows.**
5. Render **instanceable** via `RenderShadowInstanced(passes, techs, count, flags)`.
6. Restore S-block + caches.

Validate: 50-loc coc sweep + null-test A/B vs mode 9 (must be pixel-identical). Measure serial FPS
— is skip-walk+direct-read faster than the engine walk? (Tighter loop, no per-group BeginPass state
toggling, no RenderPassImmediately dispatch for instanceable.) If faster → win + MT foundation. If
not → walk cost is the irreducible cold-line traversal; only MT helps.

### Then kInstanceMT (11) — move traverse+build to workers
Pass nodes persist frame-long (EngineFixes RenderPassCache) → safe to defer. Per map (render thread):
snapshot ~80 group heads + {tech,grp,mode} (cheap) into a per-map job. Dispatch job to a worker
(pool created at **plugin init**, NOT function-static — the function-static BS::thread_pool spawned
threads during coc load and crashed, 2026-07-14). Worker: traverse chains, partition, build F16C
matrices into a **CPU staging buffer** (no D3D). After all maps: join; render thread per map uploads
the staging buffer to the instance VB (one memcpy) + renders non-instanceable per-pass + issues
DrawIndexedInstanced. Refactor `RenderShadowInstanced` to split CPU-build (worker) from
upload+draw (render thread).

Correctness risks to validate: (a) frame-persistence — confirm the cache doesn't recycle a map's
nodes within the frame (heads snapshot + deferred traversal relies on it); if it does, fall back to
snapshotting the full pass-pointer list on the render thread (walk stays serial, only build
parallelizes = smaller win). (b) non-instanceable ordering — depth-only is order-independent, but
verify no stencil/decal pass sneaks into the instanceable set.

## Test protocol (per gate)
Deploy AIO to I:\ SE 1.5.97 (robocopy /E, NEVER /MIR). Launch skse64_loader.exe. Wait Main Menu +
≥85s, hold 30s, then devbench `load "<save>"`. Env: CS_NATIVE_D3D11=1, CS_SHADOW_MT=10/11,
CS_OCCLUSION=0. Correctness: `F:\claudetmp\rtprof\sweep50_null.ps1`. FPS: interleave vs dev (rig
warms ~20fps/session — always interleave). Commit at each validated gate (session branch).
