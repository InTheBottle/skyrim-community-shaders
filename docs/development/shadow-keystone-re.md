# Shadow-map per-map replica driver — RE spec (SE 1.5.97)

_Adversarially-verified reverse-engineering blueprint for the G3 keystone: a
standalone `ShadowMapReplay::RenderMap(MapWork&, WorkerState&)` that reproduces one
shadow map from private state, so map recording can be parallelised across worker
threads. Every address/offset below was independently re-decompiled. See
`docs/development/mt-shadow-plan.md` for the architecture and gates._

## 1. The per-map accumulator walk (the pass sequence to replay)

Chain (one shadow map = one call): `DrawWorld::RenderShadowmaps 0x1412E3480` →
per-light vfunc10 → **`RenderShadowmap 0x141305610`** (ID 100820, one `{DSV, slice}`:
sun cascade / VL slice / focus / local-light face).

`RenderShadowmap(light /*a1*/, desc /*a2*/, counter /*a3*/, renderFlags /*a4*/)`:
- `camera = *(desc+0x40)`, `accumulator = *(desc+0x48)`.
- DSV-slot alloc: if `*(desc+0x54)==-1` → set `=4`, pop lowest set bit of global
  `dword_141E10538` into slice `*(desc+0x58)`.
- `*(desc+0xE8)` gates RT setup (SetDepthStencilRenderTarget/SetRenderTarget on RT-mgr
  `0x14302BB20` + clear setup).
- **Only geometry-emitting call:** `NiCamera::Render 0x1412C15C0`(camera, accumulator,
  `renderFlags|0x400`). (IDA: NiCamera::RenderPreAndPostResolveDepth.)
- Light view/proj written back to `desc+0x00/0x10/0x20/0x30` at the tail.
- (The per-map SRV-cache release array is on the **light** object: `light+0x528` ptr /
  `light+0x538` count.)

`NiCamera::Render → sub_1412C1600`(cam, acc, flags): `SetCameraData(0x14302C890, cam,
flags)`; if `flags&0x400` `Renderer::UpdateViewPort(0x143028490,0,0,1)`; `currentCamera
0x1431D0E68 = cam`; `SetBSShaderAccumulator(acc)` (`0x1412966B0`); `acc->vtbl[+0x128]`
(Func37); return `acc->vtbl[+0x150]` (**Func42 `0x1412CAC20`** — all passes emit here).

`Func42`: `SetRenderMode(acc+0x150)`; mode 0 → FinishAccumulating (main view); else
tail-call `unk_1431D1C40[mode]` (8-byte stride). **Shadow modes 12–17 all →
`sub_1412CC3C0`** (mode-table init `FUN_141294060 @0x1412948de`, verified).

**Shadow finish `sub_1412CC3C0`** (`container = *(acc+0x130)`):
- `flags&0x100` (VL slice): one group — `v=*(container+0xE8)`; persistent(`*(v+38)&1`)?
  `RenderPersistentPassList(v)` : `RenderBatches(acc, 1, 0x5C006074, flags, 15)`.
- else (cascade/focus/local), **IN ORDER**:
  1. `RenderBatches(acc, 0x2B, 0x4000002B, flags, -1)`
  2. `RenderBatches(acc, 0x5C000030, 0x5C00005C, flags, -1)`
  3. `v=*(container+0x78)`: persistent? …List : `RenderBatches(acc,1,0x5C006074,flags,1)`
  4. `v=*(container+0xB8)`: persistent? …List(+decals) : `RenderBatches(acc,1,…,flags,9)`
  5. `sub_1412CBCC0(acc,flags)` decals.
  (Group-id → container offset: `*(container + 8*id + 112)`; id 1→0x78, 9→0xB8, 15→0xE8.)

**`RenderGeometryGroup`(=RenderBatches) `0x1412CCE40`**: sub-renderer `br`: `groupId<=-1`
→ `br=*(acc+0x130)`; else `br=*(*(acc+0x130)+8*groupId+112)`. Writes bounds **into br**:
`*(br+0x50)=startKey`, `*(br+0x54)=endKey`. Scratch cursors in **accumulator**:
`acc+0x138`=curKey, `acc+0x13C`=groupType, `acc+0x140`=continuation; iterator `br+0x58`.
Loop while cont: if `(curKey-0x5C000018)<=3 && (skip)` → `sub_1413083B0` (clear, no draw)
else **BeginPass `0x141308030`**. (Skip flags `acc+0x128/0x129`.)

**`BeginPass 0x141308030`**: hash curKey → bucket via base `*(br+0x48)`, mask
`*(br+0x2C)-1`, sentinel `*(br+0x38)` (16-byte nodes {key@0, bucketIdx@+4, next@+8}).
Per-groupType global toggles: `unk_143027F4C`→main bit 0x20; `F64`→0x100; `F5C`→0x80;
alphaTest set for groupTypes 1/3/4. Pass list head =
`*(*(br+8) + 8*(groupType + 6*bucket))`; **`for(pass=head; pass; pass=pass->m_PassGroupNext /*+0x30*/) RenderPassImmediately(pass, curKey /*technique*/, alphaTest, flags)`**.
Destructive-consume (if `*(br+0x6C)`): zero the (bucket,groupType) head + clear mask bit.
Zeroes last-bound caches `0x143283BA8/0x143283BA4/0x143490BB0`. Tail: `++groupType` then
`sub_141307DD0`. Key advance/bounding `sub_141307E80`: walks the sorted key list
ASCENDING, skips key`<startKey`, done once key`>endKey`. Group scan `sub_141307FC0`:
group-types 0..4 ascending, first non-empty.

**Net per-map order:** prep → groups in `sub_1412CC3C0` order → sub-renderer → ascending
keys in `[startKey,endKey]` → group-types 0..4 → per (bucket,group) `BSRenderPass`
head→tail via `m_PassGroupNext` → `RenderPassImmediately` (already RE'd by
`UtilityPassReplica`). Self-contained per map **iff** the worker owns that accumulator
(`desc+0x48`) and its `br` (`acc+0x130` / `*(container+8*id+112)`).

## 2. SetDirtyStates `0x140D705B0` (ID 75580) — reimplement against a private block

`SetDirtyStates(char a1)`: `a1=false` full flush; `a1=true` flushes all **except** input
layout (bit 0x400 preserved). Sink = worker's own deferred ctx (not `*(0x143027EA0)`).
State-object pool `0x1430261B0`, RT/DSV pool `0x143025F00` — **read-only shared**.

**Main word (16-bit @0x143027EB0) — EMIT IN THIS ORDER** (raster re-sets the viewport
bit, so viewport MUST come after rasterizer):
1. `0x0001` RT/DSV → `OMSetRenderTargets vtbl+0x108` (single-DSV if `[block+0x40]!=-1`
   else 8-slot MRT; `ClearRenderTargetView +0x190`, `ClearDepthStencilView +0x1A8`).
2. `0x000C` DS-state → `OMSetDepthStencilState +0x120` (pool idx `40*F38+F40`, ref `F44`).
3. `0x1070` rasterizer → `RSSetState +0x158` (pool idx from `F48/F4C/F50/F54`); if
   `0x40` also set, recompute depth bias into `[block+0x74]/[0x78]` and OR `0x2` back in.
4. `0x0002` viewport → `RSSetViewports +0x160` (1, `&block+0x70`). **After raster.**
5. `0x0080` blend → `OMSetBlendState +0x118` (pool idx `F58/F5C/F60`, factor
   `&unk_141E07168`, mask 0xFFFFFFFF).
6. `0x0300` alpha CB → `Map +0x70`(alphaCB `pool[783]=0x143027A28`, DISCARD) write float
   `(F64?F68:0)`, `Unmap +0x78`.
7. `0x0400` input layout (only if `a1==0`) → hash lookup `qword_141E07160`; miss →
   create + **insert into shared hash** (`FUN_140d730e0/FUN_140d73f70`) → `IASetInputLayout
   +0x88`. (Race — see risks.)
8. `0x0800` topology → `IASetPrimitiveTopology +0xC0`.
   Writeback: `main = a1 ? (main & 0x400) : 0`.

**Per-stage masks** (`_BitScanForward` lowest-first, count 1, bit cleared), IN ORDER:
`block+0x14` CS-UAV `+0x220` · `block+0x08` PS-SAMP `+0x50` · `block+0x04` PS-SRV `+0x40`
· `block+0x10` CS-SAMP `+0x230` · `block+0x0C` CS-SRV `+0x218`.

**Scope:** binds ONLY targets/viewport/DS/raster/blend/alphaCB/IL/topology + PS-SRV/SAMP,
CS-SRV/SAMP/UAV. Does **not** bind CBs/shaders/VS-GS-HS-DS resources — those come from the
technique/material setup already in `UtilityPassReplica`. Combine both for a correct pass.

## 3. WorkerState — what each worker must privatise

**Private (mutated while recording one map):**
1. The **0x5D8 render-state block** `0x143027EB0` — mutated by SetDirtyStates AND BeginPass
   (ORs main bits 0x20/0x80/0x100, writes `F4C`/`F5C`/`F64`). Copy the whole struct.
   (IL-key `block+0x340`, topology `block+0x358`.)
2. **Immediate-context slot** `pool[926]=*(0x143027EA0)` → substitute the worker's own
   deferred context (param, or thread-local).
3. **Last-bound caches** `0x143283BA8` (shader) / `0x143283BA4` (technique) / `0x143490BB0`
   (material) — RMW + zeroed in BeginPass; per-worker or serialised.
4. **Render-mode globals** — `SetRenderMode 0x141295E90` writes `0x1431D0E28` (+ caches
   `unk_1431D1C38/C30`); per map.
5. **Accumulator scratch** `acc+0x138/0x13C/0x140`, skip flags `acc+0x128/0x129`; `br`
   bounds `br+0x50/0x54`, iterator `br+0x58`, consume head `br+0x60`. Safe only if each
   worker owns its accumulator + br.
6. **CB/VB ring cursors** (CS's existing dynamic-CB + skinned/dynamic-VB rings) — per worker.
7. **Per-map camera/viewport globals** — `SetCameraData 0x14302C890`, UpdateViewPort
   `0x143028490`, `currentCamera 0x1431D0E68`, currentAccumulator — each worker sets its own.

**Read-only-per-frame (shareable, do NOT copy):** state-object pool `0x1430261B0`, alpha CB
`pool[783]`, RTpool `0x143025F00`; `BSShader`/`BSShaderProperty`/material objects and the
already-built `BSRenderPass` lists — read-only during the walk **iff** non-consuming.

## 4. Risks (verify before/while threading)

1. **Destructive walk** — BeginPass/`sub_141307E80` gate a destructive pop on `*(br+0x6C)`.
   If shadow batch renderers run consume-mode, an accumulator can be walked only once →
   each worker needs its own accumulator or a non-consuming snapshot. **Read `*(br+0x6C)`
   on the live shadow renderers before MT** (mechanism unverified; prior claim that InitSDM
   sets it was FALSE — InitSDM only zeros the last-bound caches).
2. **Shared accumulator across cascades** — if the sun uses one accumulator for all
   cascades, workers MUST partition by accumulator or hold private scratch + br start/end.
3. **Global IL cache race** — SetDirtyStates bit10 inserts new ILs into the shared hash.
   Mitigate: pre-warm the (small) BSUtilityShader IL set on the main thread, OR record with
   `a1!=0` (defer IL), OR lock.
4. **Order-sensitive main word** — keep RT→DS→raster(+bias)→viewport→blend→alphaCB→IL→topo
   exactly; the bias branch re-emits viewport in the same flush.
5. **a1 semantics** — reproduce the IL-skip and partial writeback (`a1 ? main&0x400 : 0`).
6. **Partial flush** — SetDirtyStates alone gives correct targets but no shader inputs;
   pair with the technique/material setup.

## 5. Addresses (SE 1.5.97, base 0x140000000)

| What | Addr | AddrLib |
|---|---|---|
| RenderShadowmap (per-map body) | 0x141305610 | 100820 |
| NiCamera::Render | 0x1412C15C0 | 99789 |
| NiCamera prep sub_1412C1600 | 0x1412C1600 | 99790 |
| SetBSShaderAccumulator | 0x1412966B0 | — |
| Func42 (mode dispatch) | 0x1412CAC20 | 99936 |
| Shadow finish sub_1412CC3C0 | 0x1412CC3C0 | 99947 |
| RenderGeometryGroup/RenderBatches | 0x1412CCE40 | 99963 |
| key walk sub_141307DD0 | 0x141307DD0 | 100849 |
| key advance/bound sub_141307E80 | 0x141307E80 | 100850 |
| group scan sub_141307FC0 | 0x141307FC0 | 100851 |
| BeginPass | 0x141308030 | 100852 |
| group clear sub_1413083B0 | 0x1413083B0 | 100853 |
| RenderPassImmediately | 0x141308440 | 100854 |
| SetDirtyStates | 0x140D705B0 | 75580 |
| mode-table init FUN_141294060 | 0x141294060 | 98972 |
| SetRenderMode | 0x141295E90 | — |

Anchors: block `0x143027EB0` (0x5D8); imm ctx `*(0x143027EA0)`=pool[926]; alpha CB
`0x143027A28`=pool[783]; state pool `0x1430261B0`; RT pool `0x143025F00`; mode fn-ptr table
`0x1431D1C40`; render-mode global `0x1431D0E28`; currentCamera `0x1431D0E68`; slice bitmask
`0x141E10538`; RT mgr `0x14302BB20`; camera-data `0x14302C890`; last shader/tech/mat
`0x143283BA8`/`0x143283BA4`/`0x143490BB0`; BSShaderAccumulator vtable `0x14185CF50` (+0x128
Func37, +0x150 Func42); `BSRenderPass::m_PassGroupNext` = +0x30.

## Appendix: SetDirtyStates transcription reference

Resolved addressing (from disasm 0x140D705B0, settles the `MEMORY[]` ambiguity):
- **State-object pool** = the qword array **at** `0x1430261B0` (address itself). `pool[N] =
  *(uintptr*)(0x1430261B0 + 8*N)`. Context = `pool[926] = *(0x143027EA0)`; alpha CB =
  `pool[783] = *(0x143027A28)`. DS/raster/blend/sampler state objects are `pool[<index>]`.
- **RT pool** = the pointer **stored at** `0x143025F00` (`mov r8, [0x143025F00]`), i.e.
  `rtBase = *(uint8**)0x143025F00`. RTVs at `rtBase + 48*idx + 0xA58` (MRT) /
  `rtBase + 8*([14]+8*[13]) + 0x26D0` (single-DSV); clear color `rtBase + 0x2768`.
- Block field map (S = `0x143027EB0`): `dword_143027EBC[N] = S+0x0C+4N`; PS-SRV mask S+0x04,
  PS-SAMP S+0x08; MRT RT-index[i]=S+0x18+4i, clear-flag[i]=S+0x48+4i, single-DSV clear-flag
  [24]=S+0x6C; DS `unk_F38`=S+0x88/`F40`=S+0x90/`F44`=S+0x94; raster `F48`=S+0x98/`F4C`=S+0x9C/
  `F50`=S+0xA0/`F54`=S+0xA4; blend `F58`=S+0xA8/`F5C`=S+0xAC/`F60`=S+0xB0; alpha `F64`=S+0xB4/
  `F68`=S+0xB8; viewport `[25]`=S+0x70; depth-bias `[29]`=S+0x80/`[30]`=S+0x84; PS-SAMP pool idx
  `dword_143027F6C[i]`=S+0xBC+4i / `dword_143027FAC[i]`=S+0xFC+4i; PS-SRV views
  `qword_143027FF0[i]`=S+0x140+8i; CS views/idx `dword_143028070[i]`=S+0x1C0+4i (IL-key qword
  @[96]=S+0x340, topology @[102]=S+0x358); slope-bias `flt_143028470[i]`=S+0x5C0+4i.
- Shared read-only externals: slope-bias table `unk_143026180` (0x3026180), blend factor
  `unk_141E07168` (0x1E07168).
- Vtable indices (offset/8) for the emitted calls: OMSetRenderTargets 33, ClearRTV 50,
  ClearDSV 53, OMSetDepthStencilState 36, RSSetState 43, RSSetViewports 44, OMSetBlendState
  35, Map 14, Unmap 15, IASetInputLayout 17, IASetPrimitiveTopology 24, PSSetShaderResources
  8, PSSetSamplers 10, CSSetShaderResources 67, CSSetUAV 68, CSSetSamplers 70.

**REMAINING RE GAP (blocks the byte-exact reimpl):** the input-layout branch (bit 0x400,
`a1==false`): key = `*(u64)(S+0x340) & *(u64)(*(u64)(S+0x348)+72)`, then a shared hash-table
lookup in `qword_141E07160` (node stride 24: key@0, IL@+8, next@+16, sentinel
`off_141E07150`, mask `dword_141E07144-1`) with create (`FUN_140d70f90(key)`) + insert
(`FUN_140d730e0(&unk_141E07140, head, hash, &key, &outIL)` → bool) + grow (`FUN_140d73f70`)
on miss, hash via `sub_140C06570(&struct32, key)`. The `struct32` semantics and the four
helper signatures need RE before this branch can be transcribed. This branch ALSO mutates
the shared cache (the race in §4.3); pre-warm the (small) BSUtilityShader IL set on the main
thread to sidestep both. Everything else is mechanical from the map above.
