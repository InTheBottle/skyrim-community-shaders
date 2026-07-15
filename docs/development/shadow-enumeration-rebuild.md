# Shadow-Caster Enumeration Rebuild â€” Implementation Design

## 0. One-paragraph thesis

Func42 is serial because it *renders while it walks* â€” `BeginPass` does technique-setup D3D, it Maps shared per-frame CBs, and it drains a shared pass-node pool. But the **enumeration** those draws come from is already complete and sitting in per-light private accumulators the moment the shadow cull (`CalculateAndDrawShadowCasterLights 0x1412E2660`) returns, before the render loop `RenderAllShadowmaps 0x1412E3480`. We read those chains ourselves on workers (pure pointer walks, zero D3D), classify + group into (mesh, technique, fade) instance batches in parallel, then on the render thread issue ~9Ã— fewer draws via the already-shipping `RenderShadowInstanced`, plus a serial remainder for the non-instanceable tail. The engine's Func42 is skipped per map.

---

## 1. FEASIBILITY VERDICT

**The visible-caster set IS obtainable without Func42. Chosen source: (a) the per-light accumulator BSRenderPass chains, read post-cull / pre-walk.** Sources (b) and (c) are viable mechanisms but strictly more work for the same or worse result; they are demoted to fallbacks.

### Evidence that (a) is real (the mode-10 "empty" result was a hook-placement bug)

- The decompiled call graph proves `Func43 (0x1412CAC90)` runs *after* `Func42 (0x1412CAC20)` â€” Func42 IS the destructive walk, Func43 is `Func38` teardown. The mode-10 `DirectReadInstanceableCount` walker read at Func43 entry, i.e. after Func42 had already popped the pass heads (`BeginPass` + `m_AutoClearPasses` nulls `PassGroupData[g].m_Passes[slot]` as it drains). The `occBuckets=10 / rawWalked=0` signature is exactly a post-consume snapshot: the technique registry (`renderPassMap`) survives, the pass chains don't. **The struct offsets in the walker are correct** (that's why the registry read succeeded).
- Passes are materialized at **cull** time, not walk time: `sub_140D51280` (the cull deposit) â†’ `sub_1412CA110` (RegisterObject) â†’ per-mode register table `unk_1431D1B40[renderMode]` allocates each `BSRenderPass` and links it into the batchRenderer PassGroup chain. So at cull-complete the chains are fully populated.
- Each `ShadowMapData` (240 bytes) owns a **private** `+0x48 BSShaderAccumulator` (and `+0x40` NiCamera). After the cull, every cascade/light's caster set is in its own accumulator's chains **simultaneously** â€” the exact precondition for lock-free parallel worker reads. Pass nodes are frame-persistent (EngineFixes RenderPassCache), so a deferred read is lifetime-safe.

### Why (a) beats (b) and (c)

| Source | Yields real `BSRenderPass*`? | Extra work | Parity risk |
|---|---|---|---|
| **(a) accumulator chains** | **Yes** â€” directly feeds `RenderShadowInstanced` unchanged | none (pointer walk) | **zero** (engine's own culled set) |
| (b) Process1 / `TestShadowCasterHiZ` hook | No â€” collects bound+pointer only; pointer "never deref'd off-thread"; passes not yet linked at Process1 fire | must synthesize `BSRenderPass` or extend `RenderShadowInstanced` with a transform array; must capture mesh+transform at cull time | medium (must reproduce engine's register-time classification) |
| (c) our own frustum cull | No | reimplement the entire cull | **high** (LOD/fade/cascade-assignment divergence) |

`RenderShadowInstanced`'s hard constraint is decisive: it needs **real engine `BSRenderPass`/`BSGeometry` objects** and derives each World from `SG_BuildMatrix(pass->geometry + 0x7C)` â€” there is no external-transform path. Source (a) is the only one that hands those objects over for free. The one unverified gate is `m_AutoClearPasses` (`br+0x6C`) â€” a single-shot read is fine because we *replace* Func42, so we are the only consumer.

**The `fplanes` frustum test and the per-light VP are NOT on the critical path for (a)** â€” the accumulator chains are already frustum-culled by the engine. They stay in the design as: (i) validation cross-check, and (ii) the enabling primitive if we ever move to source (b)/(c) for the ortho directional cascades.

---

## 2. ARCHITECTURE

### Data flow (per frame)

```
[render thread]  CalculateAndDrawShadowCasterLights 0x1412E2660  (engine cull, unchanged)
                     â””â”€ deposits casters into each ShadowMapData+0x48 accumulator chains
                        â”‚
[fork to workers] â”€â”€â”€â”€â”€â”¤  ENUMERATE + CLASSIFY + BATCH  (pure CPU, no D3D)
                        â”‚   for each active ShadowMapData (light/cascade), on its own worker:
                        â”‚     1. verify br+0x6C (once), walk PassGroup chains (DirectRead layout)
                        â”‚     2. per pass: classify instanceable | remainder
                        â”‚     3. instanceable â†’ group key = rd ^ (tech<<1) ^ (fadeKey<<40)
                        â”‚     4. emit MapWork{ instBatches[], remainderPasses[], techs[], renderFlags }
[join]  â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”˜
                        â”‚
[render thread]  RenderAllShadowmaps 0x1412E3480 / RenderShadowmapDetour per map:
                     â”œâ”€ bind THIS map's RT4 / DSV(slice) / viewport / camera CB b12  (CullSetup)
                     â”œâ”€ RenderShadowInstanced(batch.passes, batch.techs, n, renderFlags)   // instanced
                     â”œâ”€ serial remainder: ReplicaRenderPassImmediately per non-instanceable pass
                     â””â”€ SKIP engine Func42 for this map
```

### Worker phase (pure CPU, NO D3D) â€” the new `ShadowCasterBatcher`

Per active `ShadowMapData`, on a worker (`bshoshany-thread-pool`), touching only CPU memory:

1. **Locate accumulator** = `*(ShadowMapData + 0x48)`; `br = *(accum + 0x130)`; `renderMode = *(accum + 0x150)` (13 spot / 14 dir-cascade / 15 parabolic).
2. **Gate `m_AutoClearPasses`** = `*(br + 0x6C)`; assert-log once per session.
3. **Walk chains** using the verified DirectRead layout: `passArrayBase = *(br+8)`, hash table `*(br+0x48)`, `cap=*(br+0x2C)`, sentinel `*(br+0x38)`; per occupied bucket `groupIdx = *(e+4)`; head = `*(passArrayBase + 8*(mode + 6*groupIdx))`; chain via `pass+0x30`.
4. **Per-pass read (CPU only)** â€” from `pass` and `geom = pass->geometry (pass+0x10)`:
   - `rd = *(TriShapeData**)(geom+0x138)`, `type = geom[0x150]`, `skin = *(void**)(geom+0x130)`, `triCount = *(u16*)(geom+0x158)`, `tech = *(u32*)(shader+0x90)`, `accumulationHint = pass[0x1C]`, fade via `sp = pass[0x08]` â†’ `fadeNode = *(sp+0x60)`.
   - **CanReplicate** gate (geom!=null, non-stencil `((passEnum-0x2B)&0x1200)!=0x1200`, static-skin vfunc54==0, `(geom[0x109]&8)==0`, typeâˆˆ{3,8}, rd!=null).
   - **Instanceable** subset (stricter): `type==3` whole-TRISHAPE, `skin==0`, `alphaTest==false`.
5. **Group** instanceables by key `rd ^ (tech<<1) ^ (fadeKey<<40)`, `fadeKey = 0xFF` for solids or `(int)(fade*31)`. Store `BSRenderPass*` + tech per instanceable; push everything else to `remainderPasses[]`.
6. **Emit** `MapWork` (two flat arrays for the instanced call + a remainder list). No sorting/baseInst here â€” `RenderShadowInstanced` already does group sort + prefix-sum internally; we just hand it the pre-filtered flat arrays (exactly the `RenderMapInstanced` adapter contract).

> Note: we can even skip building `Group` structs on the worker and just emit the flat `(passes[], techs[])` filtered to the instanceable subset â€” `RenderShadowInstanced` re-groups internally. The worker's only *required* output is: instanceable flat arrays + remainder list. Keep it that thin.

### Render-thread phase (ALL D3D, serial but cheap)

Per map, inside `RenderShadowmapDetour`, replacing `callOriginal()`:

1. **Per-map bind** â€” reproduce `CullSetup` (`ShadowThreaded.cpp:919-929`): `SetCameraData(0xd7bab0)`, `UpdateViewport(0xd69d00)` if `flags&0x400`, `unk_1431D0E68 = camera`, `SetCurrentAccumulator(0x12966b0)`, `Func37(0x12ca100)`. This uploads the per-light `b12 CameraViewProj` (GPU block `0x30282E0`) â€” the projection every instanced/remainder draw needs. Bind RT4 / DSV(atlas slice from `desc+0x58`) / viewport via the snapshotted 0x5D8 state block + `sflags[0]=0xFFFFFFFF`.
2. **Reset technique caches** â€” `g_currentTechnique=0`, `g_currentShader=nullptr`, `g_currentMaterial=nullptr`.
3. **Instanced draws** â€” `RenderShadowInstanced(mw.passes.data(), mw.techs.data(), n, mw.renderFlags)` (unchanged). Internally: group â†’ sorted by (tech,fade) â†’ one `Map(WRITE_DISCARD)` fills the shared 32-byte/instance FP16 World stream â†’ `DrawIndexedInstanced(3*triCount, n, 0, 0, baseInst)` per group.
4. **Serial remainder** â€” for each `remainderPasses[i]`: `ReplicaRenderPassImmediately(pass, tech, alphaTest, renderFlags)` (skinned / alpha-test / SUB_INDEX foliage). This is the inline path that already exists.
5. **Cleanup contract** â€” restore saved 0x5D8 block + Ws* caches + `sflags[0]=0xFFFFFFFF` so IA slot-1 (instance VB) and the `kInstBits` vertexDesc residue don't leak into main-scene rendering (StateValidator canaries `ShadowThreaded.cpp:393-396`).

**Split rule:** a worker may only *read* `BSGeometry`/`BSRenderPass`/`BSShaderProperty` CPU fields and build `std::vector` batches. Anything that touches `ID3D11DeviceContext`, an engine CB Map, `BeginPass`, `SetDirtyStates`, the bone-CB ring, or the pass-node pool stays on the render thread. The FP16 World pack in `RenderShadowInstanced` reads `s_instVB` via `Map(WRITE_DISCARD)` on the **immediate** context â€” so it must stay render-thread (it already is).

---

## 3. WHY THIS AVOIDS THE WALLS

The three proven-serial hazards of Func42 and how the worker phase sidesteps each:

1. **Shared per-frame D3D buffers (CB Maps).** Func42's `BeginPass` fills PerGeometry/PerTechnique CBs via engine Map. The worker phase writes **no CB** â€” it computes nothing that lands in a D3D buffer. The only CB write (the instance World stream) happens once per map on the render thread inside `RenderShadowInstanced`'s single `Map(WRITE_DISCARD)`. No worker ever maps a shared buffer, so the DXVK discard-spinlock affinity that deadlocked kWalkMT (per MEMORY: engine Func42's Map path isn't free-threaded) is never exercised off-thread.
2. **Shared pass-node pool.** Func42 drains `PassGroupData[g].m_Passes[slot]` destructively. Workers only **read** the chains (a pointer walk); they never allocate, free, or null a pass node. Because each `ShadowMapData` accumulator is private, two workers never touch the same chain. The one caveat â€” `m_AutoClearPasses` making the read single-shot â€” is a non-issue because we suppress Func42, so nobody else consumes.
3. **`BeginPass` technique-setup D3D.** All technique/shader/material binding is deferred to the render thread's `RenderShadowInstanced` (`FlushSetupTechniqueReplica`) and the remainder's `ReplicaRenderPassImmediately`. Workers read `tech = *(shader+0x90)` as a plain integer for grouping â€” no bind, no device call.

Net: workers do CPU reads + `std::vector` grouping; the render thread does 100% of D3D, serial, but **instancing collapses ~9Ã— the draws into one `DrawIndexedInstanced` per (mesh,tech,fade) group**, so the serial render-thread cost drops even before the enumeration parallelizes. The parallelism win is moving the per-pass classify/group off the render thread; the draw-count win is the instancing itself.

---

## 4. STAGING

Each milestone is independently buildable and validated with the existing atlas-hash + ~50-LOC harnesses. Reuse the `Func42-entry` hook slot and the mode-N dispatch already wired in `ShadowThreaded.cpp`.

**M0 â€” Enumeration oracle (no rendering change).** Add `ShadowCasterBatcher::Enumerate(accum)` reading the chains at the **correct** point: hook `Func42 entry (0x1412CAC20)` (or read per-`ShadowMapData` accumulator right after `CalculateAndDrawShadowCasterLights` returns). Do NOT skip Func42 yet â€” call original. Per map, log `{instanceable, remainder, total}` and probe `*(br+0x6C)`. **Gate: our per-map `total` == `CaptureHook`'s claimed set (diverged=0)** across Dragonsreach + Riverwood + exterior. This proves the offsets + hook timing and kills the mode-10 ghost. This is the single most important milestone â€” it converts "chains are readable pre-walk" from a decompile claim into a runtime-proven count match.

**M1 â€” Instanced draw from our enumeration, remainder via engine.** Per map: skip engine Func42, drive `CullSetup` + `RenderShadowInstanced(ourInstanceable)` on the render thread, but render the **remainder by re-running the engine walk filtered** (or fall through to `callOriginal` for a hybrid) to isolate the instanced path. **Gate: atlas depth hash identical to the mode-0 baseline** (the shadow atlas is deterministic; hash RT4 per slice). If hashes match, our instanced World math is byte-exact (already proven byte-exact vs engine b2 World at `UtilityPassReplica.cpp:3026-3043`).

**M2 â€” Full self-drive: instanced + serial remainder, engine Func42 fully skipped.** Add the `ReplicaRenderPassImmediately` remainder loop; drop `callOriginal` entirely. **Gate: atlas hash still identical + motion soak** (per `feedback_validate_culling_in_motion` â€” stills never validate; run a 30s pan + load). Confirm StateValidator canaries clean (IA slot-1 restored, no `kInstBits` residue).

**M3 â€” Parallelize enumeration onto workers.** Move `Enumerate` + classify + group into the thread pool, fanned across `ShadowMapData` list; render thread joins then issues D3D. **Gate: atlas hash identical + PresentMon A/B** (PresentMon = the instrument per MEMORY; `frame_count` pauses unfocused). Measure render-thread CPU delta.

**M4 â€” Directional cascades + tuning.** Extend to renderMode 14 (dir cascades); handle cascade-assignment (a caster can appear in multiple cascade accumulators â€” each private accumulator already contains its own copy, so no dedup needed). Tune worker granularity (per-map vs per-bucket).

Reuse for validation: the atlas-hash harness, the `DirectReadInstanceableCount` walker (M0 is literally that walker at the right hook), and the `readB12("TOP"/"DRAW")` clobber probes to confirm b12 survives when we drive without the engine walk.

---

## 5. RISKS

**Correctness:**
- **`m_AutoClearPasses` single-shot (risk #1, unverified).** If `br+0x6C` is set and some *other* consumer runs between cull and our read, chains are pre-drained. Mitigation: M0 probes it and asserts our read is first; because we suppress Func42 we are the sole consumer. **Must confirm at M0.**
- **LOD.** The pass's LOD index is stamped onto `geom+0x108` from `pass->LODMode.index` during setup â€” for the instanced path `RenderShadowInstanced`/`FlushSetupGeometryReplica` already does `geomBytes[0x108] = pass->LODMode.index`. Reading the pre-selected pass means LOD is already resolved by the engine cull. Low risk; verify via atlas hash.
- **Fade.** `accumulationHint==10` casters carry a per-object stencil-ref fade; it MUST be uniform within a `DrawIndexedInstanced`. Already handled by folding `fadeKey` into the group key. Risk only if we mis-read `fadeNode = *(sp+0x60)` / `pass[0x1E]&0x80` selector â€” covered by hash check.
- **Alpha-test & SUB_INDEX.** Excluded from instancing by design (alpha-test foliage + type-8 SUB_INDEX go to the serial remainder). Risk = mis-routing one into the instanced batch â†’ visible pop. The CanReplicate/instanceable gates are exact; hash check catches it.
- **Skinned.** Excluded (dynamic bone-CB / WRITE_NO_OVERWRITE ring is illegal off the immediate context â€” but note we don't map it on workers anyway; skinned still goes to the render-thread remainder). Static-skin vfunc54==0 casters are the only skinned allowed and only via remainder.
- **Cascade / culling parity.** Because we read the engine's own per-light accumulator (already culled + cascade-assigned), parity is inherited â€” this is the core reason to pick source (a) over (c). No independent frustum cull means no divergence surface.

**FPS estimate (realistic, honest per MEMORY constraints):**
- Shadow â‰ˆ **1.19 ms** in Dragonsreach (SSGI off). This is the *entire* ceiling the rebuild can attack.
- Two components move: (i) **draw-count collapse** from instancing (~9Ã— fewer draws) cuts the render-thread D3D submission cost of the instanceable majority â€” this is the bulk of the win and lands at M1/M2 even before parallelism. (ii) **enumeration/classify/group** moves off the render thread at M3 â€” smaller, since enumeration is cheap relative to draw submission.
- Realistic expectation: a **meaningful shadow-submit reduction on CPU-bound scenes** (Dragonsreach/Riverwood, where the render thread is the wall), **~0 on GPU-bound scenes** (Save345 â€” shadow depth fill is GPU work instancing doesn't remove; per MEMORY the +30% over dev is structurally blocked by GPU 4.04+ and dev wins GPU-bound). Frame this as: shrinks the shadow slice of the CPU frame; does not touch the GPU shadow-fill cost. Do NOT promise a global FPS number â€” validate per-scene with PresentMon A/B, interleaved (rig warms ~20fps/session).

---

## 6. EXACT REUSED vs NEW

### Reuse verbatim (no changes)
- **`UtilityPassReplica::RenderShadowInstanced`** (`UtilityPassReplica.cpp:2773`) â€” the instanced draw engine. Contract: two parallel flat arrays `(passes[], techs[])` + `count` + `renderFlags`; internal group/sort/prefix-sum/draw. **We feed it the same arrays `RenderMapInstanced` builds today.**
- **`ReplicaRenderPassImmediately`** (`UtilityPassReplica.cpp:1153`) â€” the serial remainder renderer.
- **`RenderMapInstanced` adapter body** (`ShadowThreaded.cpp:2010-2047`) â€” the per-map state bind (0x5D8 block memcpy, `sflags[0]=0xFFFFFFFF`, Ws* cache reset, cleanup). This IS the render-thread wrapper; we call it with *our* enumerated arrays instead of `CaptureHook`'s.
- **`CullSetup`** (`ShadowThreaded.cpp:919-929`) â€” self-driven per-map camera/viewport bind (needed once we skip `callOriginal`).
- **`BindPersistentStandalone`** (`ShadowThreaded.cpp:193-209`) â€” frame-persistent CB binds if any draw runs off the immediate context (it won't at M2, but keep for later).
- **`TriShapeData` struct** (`UtilityPassReplica.cpp:692`), **`BSRenderPass` layout** (`CommonLibSSE-NG/.../BSRenderPass.h`), **DirectRead chain-walk layout** (`UtilityPassReplica.cpp:3608-3670`) â€” the enumeration reads.
- **Atlas-hash + `DirectReadInstanceableCount` + `readB12` probes** â€” validation harnesses.
- **`fplanes` frustum test** (`MOC.h:208-400`) + per-map VP `0x30282E0` + `desc+0x40/0x48/0x54/0x58` â€” kept for the raw/ortho fallback (source b/c); NOT on the critical path for (a).

### New code
- **`ShadowCasterBatcher::Enumerate(ShadowMapData*)`** â€” the worker-side chain walker + CanReplicate/instanceable classifier + `(mesh,tech,fade)` grouping. ~150 LOC; reuses the DirectRead layout constants and the classification gates verbatim from the context (just moved to a worker, D3D-free).
- **`ShadowCasterBatcher::MapWork`** â€” POD result: `std::vector<BSRenderPass*> instPasses; std::vector<u32> instTechs; std::vector<RemainderEntry> remainder; u32 renderFlags;` per map.
- **Fork/join scheduler** â€” fan `Enumerate` across the active `ShadowMapData` list on the thread pool after `CalculateAndDrawShadowCasterLights`, join before the render loop. ~60 LOC.
- **`RenderShadowmapDetour` rewrite (mode â‰¥ 10)** â€” replace `callOriginal()` with: `CullSetup` â†’ `RenderShadowInstanced(mw.instPassesâ€¦)` â†’ remainder loop over `mw.remainder` â†’ cleanup. Skip engine Func42. ~80 LOC.
- **`m_AutoClearPasses` probe + assert** â€” M0 gate, ~10 LOC.

### Net
Zero new D3D code â€” every draw path already exists and is byte-exact-validated. The rebuild is: **read the engine's already-culled per-light chains on workers, then feed the existing instanced+remainder render path** â€” the novelty is entirely the parallel CPU enumeration and the Func42 skip, not the rendering.

---

**Files:** `src/Vanilla/UtilityPassReplica.cpp` (chains 3608-3670, RenderShadowInstanced 2773, ReplicaRenderPassImmediately 1153, TriShapeData 692), `src/Vanilla/ShadowThreaded.cpp` (RenderShadowmapDetour 1333, RenderMapInstanced 2010-2047, CullSetup 919-929, CaptureHook gates 256-339, StateValidator 393-396), `docs/development/shadow-walk-parallelization.md`, `docs/development/shadow-keystone-re.md` (risk #1). New: `src/Vanilla/ShadowCasterBatcher.{h,cpp}`.
