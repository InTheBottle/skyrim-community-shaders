# BSUtilityShader render-path RE dossier (SkyrimSE 1.5.97)

Reverse-engineered from own IDA analysis of SkyrimSE.exe 1.5.97, cross-referenced with
Nukem9's public skyrim64_test. Covers RenderPassImmediately (0x141308440) -> DrawIndexed
for BSUtilityShader (depth-only: shadow maps + z-prepass). All addresses are 1.5.97.


================================================================================================
## Cluster 1
================================================================================================

# Cluster report: BSGraphics::Renderer::SetDirtyStates (state flush), SkyrimSE 1.5.97

## Scope
`SetDirtyStates(bool isComputeShader)` @ **0x140D705B0** — the single choke point through which every dirty renderer state becomes a D3D11 call before a draw/dispatch. In 1.5.97 **FlushD3DResources is fully inlined** into the tail of this function (Nukem models it as a separate function). Also covered: the input-layout creation fallback `CreateInputLayoutFromVertexDesc` @ **0x140D70F90** and the input-layout BSTScatterTable machinery.

Every offset below was verified against the raw disassembly (not just Hex-Rays output).

## Address model (1.5.97)

- **G** = BSGraphics globals block (Nukem `HACK_Globals`) base = **0x143025EF0** (1.5.23: 0x14304BEF0). Identical internal layout.
- **S** = `RendererShadowState` = **0x143027EB0** = G+0x1FC0 (1.5.23: 0x14304DEB0 = same +0x1FC0 relation — layout unchanged).
- **ctx** = immediate `ID3D11DeviceContext2*` loaded from **qword [0x143027EA0]** (= G+0x1FB0, `Globals.m_DeviceContext`). Reloaded from the global before *every* call.
- **RD** = `RendererData*` loaded from **qword [0x143025F00]** (= G+0x10).
- **device** = `ID3D11Device*` at **qword [0x143025F08]** (= G+0x18) — used only by the CreateInputLayout fallback.

### Renderer-globals block fields touched (G-relative)
| Abs addr | G+off | Field | Use |
|---|---|---|---|
| 0x143025F00 | +0x10 | RendererData* | RTV/DSV/clear-color reads |
| 0x143025F08 | +0x18 | ID3D11Device* | CreateInputLayout |
| 0x143026180 | +0x290 | m_DepthBiasFactors — float[3][4], indexed FLAT as float[12] by biasMode | viewport MaxDepth trim |
| 0x1430261B0 | +0x2C0 | m_DepthStates[6][40] (ID3D11DepthStencilState*) | OMSetDepthStencilState; index = 40*depthMode + stencilMode |
| 0x143026930 | +0xA40 | m_RasterStates[2][3][12][2] (ID3D11RasterizerState*) | RSSetState; index = 72*fill + 24*cull + 2*bias + scissor |
| 0x143026DB0 | +0xEC0 | m_BlendStates[7][2][13][2] (ID3D11BlendState*) | OMSetBlendState; index = 52*mode + 26*atoc + 2*write + extra |
| 0x143027910 | +0x1A20 | m_SamplerStates[6][5] (ID3D11SamplerState*) | PS/CSSetSamplers; index = 5*addressMode + filterMode |
| 0x143027A28 | +0x1B38 | m_AlphaTestRefCB (ID3D11Buffer*, 16 bytes) | Map/Unmap alpha-test ref float |
| 0x143027EA0 | +0x1FB0 | m_DeviceContext (ID3D11DeviceContext2*) | all context calls |

### RendererData fields touched (RD-relative)
| RD+off | Field |
|---|---|
| +0x22 | `bReadOnlyDepth` (byte) — read to pick DSV view; **written 0** when m_SetDepthStencilMode ∈ {0,1,2,6} |
| +0xA48 | `pRenderTargets[]`, stride 48; RTV read at struct+0x10 (i.e. abs RD+0xA58+48*rt) |
| +0x1FA8 | `pDepthStencils[]`, stride 152 (= Texture + Views[8] + ReadOnlyViews[8] + 2 more ptrs); `Views[slice]` at +0x08 (RD+0x1FB0), `ReadOnlyViews[slice]` at +0x48 (RD+0x1FF0) |
| +0x26C8 | `pCubemapRenderTargets[]`, stride 64; `CubeSideRTV[view]` at +0x08 (RD+0x26D0 + 8*(view + 8*target)) |
| +0x2768 | `ClearColor` float[4] |

### RendererShadowState field map (S-relative; all reads/writes by this function)
```
+0x00  m_StateUpdateFlags        RW (cleared / kept per rules below; |=2 inside depth-bias block)
+0x04  m_PSResourceModifiedBits  RW (drained to 0)
+0x08  m_PSSamplerModifiedBits   RW (drained)
+0x0C  m_CSResourceModifiedBits  RW (drained)
+0x10  m_CSSamplerModifiedBits   RW (drained)
+0x14  m_CSUAVModifiedBits       RW (drained)
+0x18  m_RenderTargets[8]        R  (-1 = RENDER_TARGET_NONE terminates list)
+0x38  m_DepthStencil            R  (-1 = none -> dsv = nullptr)
+0x3C  m_DepthStencilSlice       R
+0x40  m_CubeMapRenderTarget     R  (-1 = RENDER_TARGET_CUBEMAP_NONE)
+0x44  m_CubeMapRenderTargetView R
+0x48  m_SetRenderTargetMode[8]  RW (0=SRTM_CLEAR -> clear then set 4=SRTM_NO_CLEAR)
+0x68  m_SetDepthStencilMode     RW (see clear-flag table; set 4 after clear)
+0x6C  m_SetCubeMapRenderTargetMode RW (0 -> clear then 4)
+0x70  m_ViewPort (D3D11_VIEWPORT: X,Y,W,H,MinDepth@+0x80,MaxDepth@+0x84)  R; MinDepth/MaxDepth W in depth-bias block
+0x88  m_DepthStencilDepthMode   R
+0x8C  (m_DepthStencilDepthModePrevious / Nukem "Unknown") NOT touched here
+0x90  m_DepthStencilStencilMode R
+0x94  m_StencilRef              R
+0x98  m_RasterStateFillMode     R
+0x9C  m_RasterStateCullMode     R
+0xA0  m_RasterStateDepthBiasMode R
+0xA4  m_RasterStateScissorMode  R
+0xA8  m_AlphaBlendMode          R
+0xAC  m_AlphaBlendAlphaToCoverage R
+0xB0  m_AlphaBlendWriteMode     R
+0xB4  m_AlphaTestEnabled (byte) R
+0xB8  m_AlphaTestRef (float)    R
+0xBC  m_PSTextureAddressMode[16] R
+0xFC  m_PSTextureFilterMode[16]  R
+0x140 m_PSTexture[16] (SRV*)     R (address passed to PSSetShaderResources)
+0x1C0 m_CSTextureAddressMode[16] R
+0x200 m_CSTextureFilterMode[16]  R
+0x240 m_CSTexture[16] (SRV*)     R
+0x300 m_CSUAV[8] (UAV*)          R
+0x340 m_VertexDesc (uint64)      R
+0x348 m_CurrentVertexShader      R (->m_VertexDescription at VS+0x48; ->bytecode at VS+0x68, len at VS+0x10)
+0x358 m_Topology                 R
+0x380 m_CameraData (ViewData, 0x250 bytes)
+0x5C0 m_CameraData.m_ViewDepthRange (NiPoint2, = ViewData+0x240) R
+0x5D0 m_AlphaBlendModeExtra      R (4th blend index; sits right after ViewData)
```

## Dirty-flag bit table (S->m_StateUpdateFlags), verified bit-exact
| Bit | Nukem name | Triggers |
|---|---|---|
| 0x1 | DIRTY_RENDERTARGET | ClearRenderTargetView (per SRTM_CLEAR slot), ClearDepthStencilView (per m_SetDepthStencilMode), OMSetRenderTargets |
| 0x2 | DIRTY_VIEWPORT | RSSetViewports(1, &S->m_ViewPort) |
| 0x4 | DIRTY_DEPTH_MODE | OMSetDepthStencilState (shared with 0x8; tested together as 0xC) |
| 0x8 | DIRTY_DEPTH_STENCILREF_MODE | OMSetDepthStencilState |
| 0x10 | DIRTY_UNKNOWN1 | RSSetState (tested as mask 0x1070) |
| 0x20 | DIRTY_RASTER_CULL_MODE | RSSetState |
| 0x40 | DIRTY_RASTER_DEPTH_BIAS | RSSetState + viewport Min/MaxDepth resync from m_ViewDepthRange + MaxDepth -= depthBiasFactor[biasMode] (sets 0x2) |
| 0x80 | DIRTY_ALPHA_BLEND | OMSetBlendState |
| 0x100 | DIRTY_ALPHA_TEST_REF | Map/write/Unmap m_AlphaTestRefCB (tested as 0x300) |
| 0x200 | DIRTY_ALPHA_ENABLE_TEST | same CB update |
| 0x400 | DIRTY_VERTEX_DESC | IASetInputLayout (hash-map lookup / CreateInputLayout fallback); **skipped when isComputeShader** |
| 0x800 | DIRTY_PRIMITIVE_TOPO | IASetPrimitiveTopology |
| 0x1000 | DIRTY_UNKNOWN2 | RSSetState |

Flag reset (0x140D70B20, disasm-verified): `S->m_StateUpdateFlags = isComputeShader ? (flags & 0x400) : 0;` — compute preserves **DIRTY_VERTEX_DESC** (because it was skipped), everything else cleared.

SRTM enum (verified where observed): 0=CLEAR, 1=CLEAR_DEPTH, 2=CLEAR_STENCIL, 3=RESTORE, 4=NO_CLEAR, 6=INIT. ClearDepthStencilView flags: {0,6}→3 (DEPTH|STENCIL), 1→1, 2→2, else no clear.

## Cleaned pseudocode (faithful to 1.5.97)
```cpp
// 0x140D705B0 — BSGraphics::Renderer::SetDirtyStates(bool isComputeShader)
// G  = (HACK_Globals*)0x143025EF0;  S = (RendererShadowState*)0x143027EB0 (=G+0x1FC0)
// ctx re-loaded from *(ID3D11DeviceContext2**)0x143027EA0 before EVERY call
// RD = *(RendererData**)0x143025F00
void SetDirtyStates(bool isComputeShader)
{
    uint32_t flags = S->m_StateUpdateFlags;
    if (flags != 0) {
        // ---------- 0x1 DIRTY_RENDERTARGET ----------
        if (flags & 0x1) {
            ID3D11RenderTargetView* rtvs[8];
            uint32_t viewCount = 0;
            if (S->m_CubeMapRenderTarget == (uint32_t)-1) {
                for (uint32_t i = 0; i < 8; i++) {
                    int rt = (int)S->m_RenderTargets[i];
                    if (rt == -1) break;
                    rtvs[i] = RD->pRenderTargets[rt].RTV;          // *(RD + 48*rt + 0xA58)
                    viewCount++;
                    if (S->m_SetRenderTargetMode[i] == SRTM_CLEAR) {
                        ctx->ClearRenderTargetView(rtvs[i], RD->ClearColor); // RD+0x2768
                        S->m_SetRenderTargetMode[i] = SRTM_NO_CLEAR;
                    }
                }
            } else {
                viewCount = 1;
                rtvs[0] = *(ID3D11RenderTargetView**)                       // cubemap side RTV
                    ((uint8_t*)RD + 8*(S->m_CubeMapRenderTargetView
                                       + 8*S->m_CubeMapRenderTarget) + 0x26D0);
                if (S->m_SetCubeMapRenderTargetMode == SRTM_CLEAR) {
                    ctx->ClearRenderTargetView(rtvs[0], RD->ClearColor);
                    S->m_SetCubeMapRenderTargetMode = SRTM_NO_CLEAR;
                }
            }
            if (S->m_SetDepthStencilMode <= SRTM_CLEAR_STENCIL   // 0,1,2
                || S->m_SetDepthStencilMode == SRTM_INIT)        // 6
                RD->bReadOnlyDepth = false;                      // byte RD+0x22

            ID3D11DepthStencilView* dsv = nullptr;
            if (S->m_DepthStencil != (uint32_t)-1) {
                size_t idx = S->m_DepthStencilSlice + 19u * S->m_DepthStencil;
                dsv = RD->bReadOnlyDepth
                    ? *(ID3D11DepthStencilView**)((uint8_t*)RD + 8*idx + 0x1FF0)  // ReadOnlyViews[slice]
                    : *(ID3D11DepthStencilView**)((uint8_t*)RD + 8*idx + 0x1FB0); // Views[slice]
                if (dsv) {
                    uint32_t cf = 0;
                    switch (S->m_SetDepthStencilMode) {
                        case SRTM_CLEAR: case SRTM_INIT:  cf = 3; break; // DEPTH|STENCIL
                        case SRTM_CLEAR_DEPTH:            cf = 1; break;
                        case SRTM_CLEAR_STENCIL:          cf = 2; break;
                    }
                    if (cf) {
                        ctx->ClearDepthStencilView(dsv, cf, 1.0f, 0);    // depth=1.0f, stencil=0 (r15b==0)
                        S->m_SetDepthStencilMode = SRTM_NO_CLEAR;
                    }
                }
            }
            ctx->OMSetRenderTargets(viewCount, rtvs, dsv);  // viewCount may be 0 (shadow maps / z-only)
        }
        // ---------- 0x4|0x8 depth-stencil state ----------
        if (flags & 0xC)
            ctx->OMSetDepthStencilState(
                G->m_DepthStates[S->m_DepthStencilDepthMode][S->m_DepthStencilStencilMode],
                S->m_StencilRef);   // qword[0x1430261B0 + 8*(40*depthMode + stencilMode)]
        // ---------- 0x10|0x20|0x40|0x1000 rasterizer ----------
        if (flags & 0x1070) {
            ctx->RSSetState(G->m_RasterStates[fill][cull][bias][scissor]);
                // qword[0x143026930 + 8*(72*S->m_RasterStateFillMode + 24*S->m_RasterStateCullMode
                //                        + 2*S->m_RasterStateDepthBiasMode + S->m_RasterStateScissorMode)]
            if (flags & 0x40) {  // DIRTY_RASTER_DEPTH_BIAS
                float maxD = S->m_ViewPort.MaxDepth;
                if (S->m_ViewPort.MinDepth != S->m_CameraData.m_ViewDepthRange.x
                    || maxD != S->m_CameraData.m_ViewDepthRange.y) {
                    S->m_ViewPort.MinDepth = S->m_CameraData.m_ViewDepthRange.x;   // S+0x5C0
                    maxD = S->m_CameraData.m_ViewDepthRange.y;                     // S+0x5C4
                    S->m_ViewPort.MaxDepth = maxD;
                    flags |= 0x2; S->m_StateUpdateFlags = flags;      // written back immediately
                }
                if (S->m_RasterStateDepthBiasMode != 0) {
                    S->m_ViewPort.MaxDepth = maxD
                        - ((float*)0x143026180)[S->m_RasterStateDepthBiasMode];  // m_DepthBiasFactors flat[12]
                    flags |= 0x2; S->m_StateUpdateFlags = flags;
                }
            }
        }
        // ---------- 0x2 viewport ----------
        if (flags & 0x2)
            ctx->RSSetViewports(1, &S->m_ViewPort);          // S+0x70
        // ---------- 0x80 blend ----------
        if (flags & 0x80)
            ctx->OMSetBlendState(
                G->m_BlendStates[S->m_AlphaBlendMode][S->m_AlphaBlendAlphaToCoverage]
                                [S->m_AlphaBlendWriteMode][S->m_AlphaBlendModeExtra],
                // qword[0x143026DB0 + 8*(52*mode + 26*atoc + 2*write + extra)]; extra @ S+0x5D0
                (const float*)0x141E07168 /* {1,1,1,1} */, 0xFFFFFFFF);
        // ---------- 0x100|0x200 alpha-test ref CB ----------
        if (flags & 0x300) {
            D3D11_MAPPED_SUBRESOURCE m;
            ID3D11Buffer* cb = *(ID3D11Buffer**)0x143027A28;   // G->m_AlphaTestRefCB (16-byte CB)
            ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
            *(float*)m.pData = S->m_AlphaTestEnabled ? S->m_AlphaTestRef : 0.0f;
            ctx->Unmap(cb, 0);
        }
        // ---------- 0x400 input layout (draw path only) ----------
        if (!isComputeShader && (flags & 0x400)) {
            uint64_t desc = S->m_VertexDesc & S->m_CurrentVertexShader->m_VertexDescription; // VS+0x48
            ID3D11InputLayout* layout;
            uint32_t h = CRC32_u64(desc);                       // 0x140C06570, table 0x14175BF90
            // BSTScatterTable @0x141E07140: cap@+4, sentinel@+0x10, entries@+0x20 (24B: key,val,next). NO LOCK.
            if (Entry* e = lookup(h, desc)) layout = e->value;
            else {
                layout = CreateInputLayoutFromVertexDesc(desc); // 0x140D70F90 (may return nullptr)
                if (layout || desc != 0x300000000407ull)        // never cache the known-bad desc
                    while (!insert(0x141E07140, CRC32_u64(desc), desc, layout))  // 0x140D730E0
                        grow(0x141E07140);                                        // 0x140D73F70
            }
            ctx->IASetInputLayout(layout);
        }
        // ---------- 0x800 topology ----------
        if (flags & 0x800)
            ctx->IASetPrimitiveTopology(S->m_Topology);         // dword S+0x358
        // ---------- reset ----------
        S->m_StateUpdateFlags = isComputeShader ? (flags & 0x400) : 0;  // keeps VERTEX_DESC (NOT topo)
    }

    // ============ inlined FlushD3DResources (runs even if flags was 0) ============
    // Each loop: bsf lowest bit i, clear it in the GLOBAL before the call, then one D3D call per bit.
    for (bits = S->m_CSUAVModifiedBits;    bits; )  // S+0x14
        ctx->CSSetUnorderedAccessViews(i, 1, &S->m_CSUAV[i] /*S+0x300*/, nullptr);
    for (bits = S->m_PSSamplerModifiedBits; bits; ) // S+0x08
        ctx->PSSetSamplers(i, 1, &G->m_SamplerStates[S->m_PSTextureAddressMode[i]]   // S+0xBC
                                                    [S->m_PSTextureFilterMode[i]]);  // S+0xFC
        // qword address 0x143027910 + 8*(5*addr + filter)
    for (bits = S->m_PSResourceModifiedBits; bits; ) // S+0x04
        ctx->PSSetShaderResources(i, 1, &S->m_PSTexture[i]);    // S+0x140; ONE slot per call
    for (bits = S->m_CSSamplerModifiedBits; bits; )  // S+0x10
        ctx->CSSetSamplers(i, 1, &G->m_SamplerStates[S->m_CSTextureAddressMode[i]]   // S+0x1C0
                                                    [S->m_CSTextureFilterMode[i]]);  // S+0x200
    for (bits = S->m_CSResourceModifiedBits; bits; ) // S+0x0C
        ctx->CSSetShaderResources(i, 1, &S->m_CSTexture[i]);    // S+0x240
}
```

## CreateInputLayoutFromVertexDesc @ 0x140D70F90 (Nukem's 1.5.23 sub_140D70620, game-original)
Builds up to 14 `D3D11_INPUT_ELEMENT_DESC` on the stack from the 64-bit vertex desc, then:
`device->CreateInputLayout(elems, count, VS->bytecode /*VS+0x68 inline*/, VS->m_ShaderLength /*VS+0x10*/, &out)` with device = `*(ID3D11Device**)0x143025F08` and **VS = S->m_CurrentVertexShader** (read again from the global). HRESULT ignored; returns `out` (nullptr on failure, pre-zeroed).

Bit semantics — bit N ⇒ attribute present with InputSlot 0, bit N+10 ⇒ InputSlot 1 (dynamic). Attribute byte offsets come from the low nibbles: element offset = `(desc >> (4*k)) & 0xF) * 4`, decompiled as `(desc >> (4k-2)) & 0x3C`:

| Bits (slot0/slot1) | Semantic(s) | DXGI format | AlignedByteOffset | Class/Step |
|---|---|---|---|---|
| always emitted | POSITION 0 | 2 (R32G32B32A32_FLOAT) | 0 | vertex/0; slot = bit44?0 : bit54?1 : **-1** |
| 45 / 55 | TEXCOORD 0 | 34 (R16G16_FLOAT) | (desc>>6)&0x3C | vertex/0 |
| 46 / 56 | TEXCOORD 1 | 10 (R16G16B16A16_FLOAT) | (desc>>10)&0x3C | vertex/0 |
| 47 / 57 | NORMAL 0 | 28 (R8G8B8A8_UNORM) | (desc>>14)&0x3C | vertex/0 |
| 48 / 58 | BINORMAL 0 | 28 | (desc>>18)&0x3C | vertex/0 |
| 49 / 59 | COLOR 0 | 28 | (desc>>22)&0x3C | vertex/0 |
| 50 / 60 | BLENDWEIGHT 0 + BLENDINDICES 0 | 10 + 28 | X, X+8; X=(desc>>26)&0x3C | vertex/0 |
| 51 / 61 | TEXCOORD 2 + TEXCOORD 3 (eye data) | 28 + 28 | X, X+4; X=(desc>>30)&0x3C | vertex/0 |
| 52 / 62 | TEXCOORD 2 (landscape) | 41 (R32_FLOAT) | (desc>>34)&0x3C | vertex/0 |
| 53 / 63 | TEXCOORD 4,5,6,7 (instancing) | 10 ×4 | X, X+8, X+16, X+24; X=(desc>>38)&0x3C | **instance/1** |

The known-bad desc `0x300000000407` (BINORMAL+COLOR present but no POSITION bit → InputSlot -1 → CreateInputLayout fails) is deliberately never cached.

## Input-layout map internals
`BSTScatterTable<uint64, ID3D11InputLayout*>` global at **0x141E07140**: capacity dword at +0x04 (power of two; bucket = hash & (cap-1)), free count at +0x08, sentinel ptr at +0x10 (self-referential 0x141E0718C), entry array ptr at +0x20; entries are 24 bytes `{uint64 key; ID3D11InputLayout* value; Entry* next}`; empty bucket signalled by `next == nullptr`, chain end by `next == sentinel`. Hash = **CRC32 of the 8 key bytes** (fn 0x140C06570, table 0x14175BF90 — standard Bethesda BSCRC32). Insert 0x140D730E0 (updates value if key exists; robin-hood-ish relocation on collision), grow/rehash 0x140D73F70, free-entry allocator 0x140D73A20. **No lock anywhere** — safe only because all callers are on the render thread under the Renderer lock discipline.

## Control-flow notes relevant to utility (depth-only) passes
- `RenderPassImmediately`-driven draws call this with `isComputeShader=false` right before `DrawIndexed`; the flush loops at the tail run **unconditionally** (even with flags==0), so stale PS SRV/sampler dirty bits always drain here.
- Shadow-map rendering: `m_RenderTargets[0] == -1` → `OMSetRenderTargets(0, garbage-but-unread, dsv)`; depth-bias path (0x40 + biasMode 1..11) subtracts `m_DepthBiasFactors[biasMode]` from viewport MaxDepth and forces an RSSetViewports.
- `bReadOnlyDepth` (RD+0x22) selects read-only DSVs — relevant to the z-prepass-then-sample patterns; it is cleared here whenever a clear mode is pending.
- The alpha-test CB is only *written* here; its binding to VS/PS b11 happens elsewhere (per Nukem: CONSTANT_GROUP_LEVEL_ALPHA_TEST_REF, bound at shader-setup time).

### Every D3D11 call (arg -> data source)
- ClearRenderTargetView(rtv, color) <- rtv = RD->pRenderTargets[S->m_RenderTargets[i]].RTV = *(RD + 48*rt + 0xA58) [non-cubemap, per slot with m_SetRenderTargetMode[i]==SRTM_CLEAR] or cubemap side *(RD + 8*(S->m_CubeMapRenderTargetView + 8*S->m_CubeMapRenderTarget) + 0x26D0); color = (float[4])(RD+0x2768) ClearColor; ctx from qword[0x143027EA0]
- ClearDepthStencilView(dsv, clearFlags, 1.0f, 0) <- dsv = *(RD + 8*(S->m_DepthStencilSlice + 19*S->m_DepthStencil) + (RD->bReadOnlyDepth ? 0x1FF0 : 0x1FB0)); clearFlags from S->m_SetDepthStencilMode {0,6->3, 1->1, 2->2}; depth const 1.0f; stencil 0 (r15b zeroed in prologue)
- OMSetRenderTargets(viewCount, rtvs[8], dsv) <- viewCount = count of non -1 entries in S->m_RenderTargets[0..7] (or 1 for cubemap path); rtvs built as above; dsv as above or nullptr when S->m_DepthStencil == -1 [gated on flags & 0x1]
- OMSetDepthStencilState(stateObj, stencilRef) <- stateObj = qword[0x1430261B0 + 8*(40*S->m_DepthStencilDepthMode(S+0x88) + S->m_DepthStencilStencilMode(S+0x90))] = G->m_DepthStates[6][40]; stencilRef = S->m_StencilRef (S+0x94) [gated on flags & 0xC]
- RSSetState(stateObj) <- stateObj = qword[0x143026930 + 8*(72*S->m_RasterStateFillMode(S+0x98) + 24*S->m_RasterStateCullMode(S+0x9C) + 2*S->m_RasterStateDepthBiasMode(S+0xA0) + S->m_RasterStateScissorMode(S+0xA4))] = G->m_RasterStates[2][3][12][2] [gated on flags & 0x1070]
- RSSetViewports(1, &S->m_ViewPort) <- D3D11_VIEWPORT at S+0x70; MinDepth(S+0x80)/MaxDepth(S+0x84) possibly rewritten this call from S->m_CameraData.m_ViewDepthRange (S+0x5C0/S+0x5C4) and MaxDepth -= float[0x143026180 + 4*biasMode] (m_DepthBiasFactors) [gated on flags & 0x2, which the 0x40 block can set]
- OMSetBlendState(stateObj, blendFactor, 0xFFFFFFFF) <- stateObj = qword[0x143026DB0 + 8*(52*S->m_AlphaBlendMode(S+0xA8) + 26*S->m_AlphaBlendAlphaToCoverage(S+0xAC) + 2*S->m_AlphaBlendWriteMode(S+0xB0) + S->m_AlphaBlendModeExtra(S+0x5D0))] = G->m_BlendStates[7][2][13][2]; blendFactor = static {1,1,1,1} at 0x141E07168 [gated on flags & 0x80]
- Map(cb, 0, D3D11_MAP_WRITE_DISCARD(4), 0, &mapped) <- cb = *(ID3D11Buffer**)0x143027A28 = G->m_AlphaTestRefCB (16-byte CB); then *(float*)mapped.pData = S->m_AlphaTestEnabled(byte S+0xB4) ? S->m_AlphaTestRef(S+0xB8) : 0.0f [gated on flags & 0x300]
- Unmap(cb, 0) <- same G->m_AlphaTestRefCB
- IASetInputLayout(layout) <- layout = BSTScatterTable@0x141E07140 lookup of key (S->m_VertexDesc(S+0x340) & S->m_CurrentVertexShader(S+0x348)->m_VertexDescription(VS+0x48)) hashed by CRC32_u64 (0x140C06570); on miss layout = CreateInputLayoutFromVertexDesc(0x140D70F90) result (may be nullptr), cached unless (nullptr && key==0x300000000407) [gated on !isComputeShader && flags & 0x400]
- IASetPrimitiveTopology(S->m_Topology) <- dword S+0x358 [gated on flags & 0x800]
- CSSetUnorderedAccessViews(i, 1, &S->m_CSUAV[i], nullptr) <- per set bit i of S->m_CSUAVModifiedBits (S+0x14, cleared per-bit before call); UAV array at S+0x300 [flush section, unconditional]
- PSSetSamplers(i, 1, &sampler) <- per set bit i of S->m_PSSamplerModifiedBits (S+0x08); sampler = &qword[0x143027910 + 8*(5*S->m_PSTextureAddressMode[i](S+0xBC+4i) + S->m_PSTextureFilterMode[i](S+0xFC+4i))] = &G->m_SamplerStates[addr][filter] [flush section]
- PSSetShaderResources(i, 1, &S->m_PSTexture[i]) <- per set bit i of S->m_PSResourceModifiedBits (S+0x04); SRV array at S+0x140; exactly ONE slot per call (no adjacent merge in 1.5.97) [flush section]
- CSSetSamplers(i, 1, &sampler) <- per set bit i of S->m_CSSamplerModifiedBits (S+0x10); sampler = &qword[0x143027910 + 8*(5*S->m_CSTextureAddressMode[i](S+0x1C0+4i) + S->m_CSTextureFilterMode[i](S+0x200+4i))] [flush section]
- CSSetShaderResources(i, 1, &S->m_CSTexture[i]) <- per set bit i of S->m_CSResourceModifiedBits (S+0x0C); SRV array at S+0x240 [flush section]
- ID3D11Device::CreateInputLayout(elems, numElems, pBytecode, bytecodeLen, &out) <- device = *(ID3D11Device**)0x143025F08 (G+0x18); elems built from vertexDesc bits per the table in the report; pBytecode = (uint8*)S->m_CurrentVertexShader + 0x68 (inline bytecode); bytecodeLen = *(uint32*)(VS+0x10); called only on input-layout-map miss inside 0x140D70F90

### Divergences from Nukem's 1.5.23 RE
- Compute-shader flag retention: 1.5.97 executes `S->m_StateUpdateFlags = isComputeShader ? (flags & 0x400 /*DIRTY_VERTEX_DESC*/) : 0` (disasm-verified `and edx,400h; cmovnz` at 0x140D70B20). Nukem's source says `flags & DIRTY_PRIMITIVE_TOPO (0x800)` — a transcription bug in skyrim64_test (keeping VERTEX_DESC is the logically correct behavior since that block is skipped for compute; topology IS issued for compute).
- FlushD3DResources is fully inlined into SetDirtyStates in 1.5.97 (label at 0x140D70B35); there is no separate call. Nukem models it as a distinct member function.
- Flush-loop order differs: binary drains CSUAVModifiedBits FIRST, then PSSamplers, PSResources, CSSamplers, CSResources. Nukem's rewrite orders PSSamplers, PSResources, CSSamplers, CSResources, CSUAV last. Only matters for compute paths (calls are independent slots), but replicate the binary order for bit-exactness.
- PSSetShaderResources adjacent-slot merge (PSSSR(i,2,...) when bits i and i+1 both set) is Nukem's own optimization; the 1.5.97 binary issues exactly one PSSetShaderResources per dirty bit.
- InputLayoutLock + tbb concurrent map is Nukem's replacement. The 1.5.97 binary uses an UNLOCKED BSTScatterTable<uint64, ID3D11InputLayout*> at 0x141E07140 (CRC32-of-u64 hash fn 0x140C06570, insert 0x140D730E0, grow 0x140D73F70).
- Nukem writes m_DepthBiasFactors[0][biasMode] with declared dims float[3][4]; the binary indexes a flat float[12] at 0x143026180 by biasMode (0..11) — same memory, but Nukem's [0][...] notation only 'works' by overflowing rows; treat as flat.
- Address relocation 1.5.23 -> 1.5.97: HACK_Globals 0x304BEF0 -> 0x3025EF0, RendererShadowState 0x304DEB0 -> 0x3027EB0 (same G+0x1FC0 relation), input-layout creator 0xD70620 -> 0xD70F90. All internal struct layouts (HACK_Globals, RendererShadowState, RendererData, VertexShader, ViewData) verified byte-identical to Nukem's headers.
- State-array dimensions all confirmed identical to Nukem: m_DepthStates[6][40], m_RasterStates[2][3][12][2], m_BlendStates[7][2][13][2], m_SamplerStates[6][5]; index math verified instruction-by-instruction.
- Nukem omits (via his cleaner C++) that the binary writes the flags word back to the global immediately inside the DIRTY_RASTER_DEPTH_BIAS block (S->m_StateUpdateFlags |= DIRTY_VIEWPORT stored at 0x140D70875/0x140D70893 before the viewport branch reads it) — same net effect, but relevant if hooking mid-function.

### Open questions
- POSITION is always emitted as DXGI_FORMAT_R32G32B32A32_FLOAT (2) with offset 0 regardless of desc bits — consistent with SSE's float3-position + packed bitangentX-in-w vertex layout, but half-precision-position meshes (if any survive at runtime in 1.5.97) would mismatch; assume the runtime vertex buffers are always full-precision (verify in the geometry/vertex-buffer cluster).
- If a desc has neither position bit 44 nor 54, the POSITION element gets InputSlot = -1 -> CreateInputLayout fails -> nullptr is still passed to IASetInputLayout AND is cached for any desc other than 0x300000000407. IASetInputLayout(NULL) is legal API-wise; whether such descs ever reach a utility draw is unverified.
- SRTM enum values 3 (RESTORE) and 5 (FORCE_COPY_RESTORE) are taken from Nukem, not observed in this function (only 0,1,2,4,6 appear here).
- S+0x8C (Nukem m_DepthStencilUnknown / likely 'previous depth mode') is never touched by this function; its writer is in DepthStencilStateSetDepthMode (different cluster).
- The alpha-test-ref CB (G+0x1B38 = 0x143027A28) is only Mapped/written here; the VS/PSSetConstantBuffers binding to slot 11 (CONSTANT_GROUP_LEVEL_ALPHA_TEST_REF per Nukem) happens elsewhere and must be captured by the shader-setup cluster.
- CRC32 hash (0x140C06570, table 0x14175BF90) assumed to be standard Bethesda BSCRC32 over the 8 key bytes; polynomial not re-derived from the table.
- FUN_140D73A20 (scatter-table free-entry allocator) and FUN_140D73F70 (grow/rehash) not fully decompiled — treated as standard BSTScatterTable machinery; only their contract (insert retry loop) is load-bearing here.
- DepthStencilData assumed layout {Texture, Views[8], ReadOnlyViews[8], +2 trailing ptrs} = 152 bytes: Views base RD+0x1FB0 and ReadOnlyViews base RD+0x1FF0 are disasm-proven; the trailing two pointers (SRV/stencil SRV per Nukem) are inferred from the 19-qword stride only.
- Viewport TopLeftX/Y/Width/Height (S+0x70..0x7C) are produced by UpdateViewPort (not in this cluster); this function only resyncs MinDepth/MaxDepth.
- RENDER_TARGET count inferred ~114 from (0x1FA8-0xA48)/48 and DEPTH_STENCIL count = 12 from (0x26C8-0x1FA8)/152; enum values themselves not re-verified against 1.5.97.

### Key addresses
- 0x140D705B0 BSGraphics::Renderer::SetDirtyStates(bool isComputeShader) — state flush; FlushD3DResources inlined at tail (0x140D70B35)
- 0x140D70F90 CreateInputLayoutFromVertexDesc(uint64 desc) -> ID3D11InputLayout* (Nukem 1.5.23: 0x140D70620)
- 0x140C06570 CRC32_u64 hash for BSTScatterTable (writes u32 hash to out param)
- 0x140D730E0 InputLayoutMap insert (returns false if table null/alloc fail; updates value on existing key)
- 0x140D73A20 BSTScatterTable free-entry allocator (called by insert)
- 0x140D73F70 BSTScatterTable grow/rehash (retry loop partner of insert)
- 0x14175BF90 CRC32 lookup table (BSCRC32)
- 0x141E07140 InputLayoutMap BSTScatterTable<uint64 vertexDesc, ID3D11InputLayout*> (cap@+4, freeCount@+8, sentinel@+0x10=0x141E0718C, entries@+0x20; 24B entries {key,value,next}; NO lock)
- 0x141E07168 static const float blendFactor[4] = {1,1,1,1} for OMSetBlendState
- 0x143025EF0 BSGraphics globals block G (Nukem HACK_Globals; 1.5.23 0x304BEF0)
- 0x143025F00 G+0x10 RendererData* (RD)
- 0x143025F08 G+0x18 ID3D11Device*
- 0x143026180 G+0x290 m_DepthBiasFactors float[12] (Nukem [3][4]) indexed by rasterDepthBiasMode
- 0x1430261B0 G+0x2C0 m_DepthStates[6][40] ID3D11DepthStencilState* (index 40*depthMode+stencilMode)
- 0x143026930 G+0xA40 m_RasterStates[2][3][12][2] ID3D11RasterizerState* (index 72*fill+24*cull+2*bias+scissor)
- 0x143026DB0 G+0xEC0 m_BlendStates[7][2][13][2] ID3D11BlendState* (index 52*mode+26*atoc+2*write+extra)
- 0x143027910 G+0x1A20 m_SamplerStates[6][5] ID3D11SamplerState* (index 5*addressMode+filterMode; shared PS/CS)
- 0x143027A28 G+0x1B38 m_AlphaTestRefCB ID3D11Buffer* (16-byte CB, slot 11 alpha-test ref)
- 0x143027EA0 G+0x1FB0 ID3D11DeviceContext2* immediate context (reloaded before every call)
- 0x143027EB0 RendererShadowState S base = G+0x1FC0 (m_StateUpdateFlags)
- 0x143027EB4 S+0x04 m_PSResourceModifiedBits
- 0x143027EB8 S+0x08 m_PSSamplerModifiedBits
- 0x143027EBC S+0x0C m_CSResourceModifiedBits (IDA's dword_143027EBC array anchor)
- 0x143027EC0 S+0x10 m_CSSamplerModifiedBits
- 0x143027EC4 S+0x14 m_CSUAVModifiedBits
- 0x143027EC8 S+0x18 m_RenderTargets[8]
- 0x143027EE8 S+0x38 m_DepthStencil; +0x3C slice; +0x40 cubeRT; +0x44 cubeView
- 0x143027EF8 S+0x48 m_SetRenderTargetMode[8]; +0x68 setDepthStencilMode; +0x6C setCubeMode
- 0x143027F20 S+0x70 m_ViewPort (MinDepth@S+0x80=0x143027F30, MaxDepth@S+0x84=0x143027F34)
- 0x143027F38 S+0x88 m_DepthStencilDepthMode; 0x143027F40 S+0x90 stencilMode; 0x143027F44 S+0x94 stencilRef
- 0x143027F48 S+0x98 rasterFill; +0x9C cull; +0xA0 depthBiasMode; +0xA4 scissor
- 0x143027F58 S+0xA8 alphaBlendMode; +0xAC atoc; +0xB0 writeMode; 0x143027F64 S+0xB4 alphaTestEnabled(byte); 0x143027F68 S+0xB8 alphaTestRef(float)
- 0x143027F6C S+0xBC m_PSTextureAddressMode[16]; 0x143027FAC S+0xFC m_PSTextureFilterMode[16]; 0x143027FF0 S+0x140 m_PSTexture[16]
- 0x143028070 S+0x1C0 m_CSTextureAddressMode[16]; 0x1430280B0 S+0x200 m_CSTextureFilterMode[16]; 0x1430280F0 S+0x240 m_CSTexture[16]; 0x1430281B0 S+0x300 m_CSUAV[8]
- 0x1430281F0 S+0x340 m_VertexDesc; 0x1430281F8 S+0x348 m_CurrentVertexShader (VS: bytecodeLen@+0x10, vertexDescription@+0x48, bytecode@+0x68); 0x143028208 S+0x358 m_Topology
- 0x143028230 S+0x380 m_CameraData (ViewData 0x250); 0x143028470 S+0x5C0 m_ViewDepthRange (NiPoint2); 0x143028480 S+0x5D0 m_AlphaBlendModeExtra
- RD+0x22 bReadOnlyDepth (byte); RD+0xA48 pRenderTargets[] stride 48 (RTV@+0x10); RD+0x1FA8 pDepthStencils[] stride 152 (Views@+8, ReadOnlyViews@+0x48); RD+0x26C8 pCubemapRenderTargets[] stride 64 (CubeSideRTV@+8); RD+0x2768 ClearColor float[4]

================================================================================================
## Cluster 2
================================================================================================

# BSUtilityShader cluster — SkyrimSE 1.5.97 (exhaustive RE baseline)

## 0. Identity, vtable, object layout

`BSUtilityShader::Ctor` @ **0x14130DCE0**: calls shared `BSShader::ctor` (IDA mislabel "BSLightingShader::ctor", 0x14131F2F0) with loader type `"Utility"`, sets `m_Type = 8` (BSSM_SHADER_UTILITY), installs 3 vtables, stores singleton `BSUtilityShader::pInstance` = **0x143495D50**.

Vtable **0x1418685B0** (`??_7BSUtilityShader@@6B@`), primary = 10 slots, then RTTI COL + 2 secondary vtables (layout verified via ctor storing 0x141868608 at this+0x10 and 0x141868620 at this+0x18 — BSShader = NiRefObject + NiBoneMatrixSetterI + BSReloadShaderI, exactly Nukem's layout):

| slot | addr | name |
|---|---|---|
| 0 | 0x141310770 | ~BSUtilityShader |
| 1 | 0x140C61A30 | DeleteThis |
| 2 | 0x14130DF90 | **SetupTechnique(u32 technique)** |
| 3 | 0x14130DD80 | **RestoreTechnique(u32 technique)** |
| 4 | 0x14130E890 | **SetupMaterial(BSShaderMaterial*)** |
| 5 | 0x14130EC60 | RestoreMaterial = nullsub |
| 6 | 0x14130EC70 | **SetupGeometry(BSRenderPass*, u32 flags)** |
| 7 | 0x141310300 | **RestoreGeometry(BSRenderPass*, u32 flags)** |
| 8 | 0x14131F430 | GetTechniqueName = nullsub |
| 9 | 0x14131F7F0 | ReloadShaders(bool) → thunk to BSShader::sub_14131FB10 |
| — | 0x141868600 | RTTI COL ptr (0x141990B18, .rdata — NOT a function) |
| _0+0 | 0x141310758 | NiBoneMatrixSetterI dtor thunk |
| _0+1 | 0x14131F630 | **SetBoneMatrix(NiSkinInstance*, Partition*, NiTransform*)** |
| — | 0x141868618 | RTTI COL ptr (0x141990B40) |
| _1+0 | 0x14131F800 | ReloadShaders(BSIStream*) |

Instance fields beyond BSShader (0x90): **this+0x90 = current technique flag word F**, **this+0x94 = F & 0x7F** (vertex-format subset). **F = technique − 0x2B (43)** — set in SetupTechnique; every gate below tests F, not the raw technique id.

## 1. Technique flag-bit vocabulary (F; confirmed against Nukem's `BSShaderInfo::BSUtilityShader`)

bit0 VC, bit1 TEXTURE, bit2 SKINNED, bit3 NORMALS, bit4 BINORMAL_TANGENT, bit5/6 unknown ("L"/?; both VS-stripped), bit7 ALPHA_TEST, bit8 LOD_LANDSCAPE (doubles as FOCUS_SHADOW under shadowmask), bit9 RENDER_NORMAL, bit10 RENDER_NORMAL_FALLOFF, bit11 RENDER_NORMAL_CLAMP, bit12 RENDER_NORMAL_CLEAR (bits9+12 = STENCIL_ABOVE_WATER, mask 0x1200), bit13 RENDER_DEPTH, bit14 RENDER_SHADOWMAP (`(F&0x20004000)==0x4000`), bit15 RENDER_SHADOWMAP_CLAMPED (GRAYSCALE_TO_ALPHA when bit29), bit16 RENDER_SHADOWMAP_PB / ADDITIONAL_ALPHA_MASK (`(F&0x14000)==0x10000` = "Aam"), bit17 DEBUG_COLOR / DEPTH_WRITE_DECALS / shadowfilter-q0, bit18 DEBUG_SHADOWSPLIT / shadowfilter-q1, bit19 SIL_COLOR / shadowfilter-q2, bit20 GRAYSCALE_MASK, bit21 RENDER_SHADOWMASK (directional cascades), bit22 SHADOWMASKSPOT, bit23 SHADOWMASKPB, bit24 SHADOWMASKDPB (shadowmask family mask **0x1E00000**), bit25 RENDER_BASE_TEXTURE, bit26 TREE_ANIM, bit27 LOD_OBJECT, bit28 LOCALMAP_FOGOFWAR, bit29 OPAQUE_EFFECT.

**hasPixelShader** (= NOT NO_PIXEL_SHADER; byte-exact = Nukem's define condition):
```cpp
bool hasPS = (F & 0x14000) == 0x14000
          || ((F & 0x20004000) != 0x4000 && (F & 0x1E02000) != 0x2000)
          || (F & 0x80) || (F & 0x14000) == 0x10000;
```
Depth-only passes (plain shadowmap / plain RENDER_DEPTH without alpha-test/Aam) bind **no pixel shader**.

## 2. Technique → shader-ID reducers

**VS id, FUN_141334900**:
```cpp
uint32_t VSID(uint32_t F) {
  uint32_t v = F & 0xF7E5FF9F;                 // strip bits 5,6,17,19,20,27 (PS-only)
  if (byte_141E0DE4C && (v & 0x1E00000))       // focus-shadow shadowmask VS
    v = (v & 0x1E00000) | 0x2002;              // TEXTURE|RENDER_DEPTH + mask bits
  if ((v & 0x14000) != 0x14000 && !NO_PS_cond_fails) // same NO_PIXEL_SHADER predicate
    v &= 0xDFFFE1E4;                           // depth-only: strip bits 0,1,3,4,9,10,11,12,29
  return v;
}
```
(0xC066 → F=0xC03B → VSID 0xC000, confirmed.)

**PS id, FUN_141334970**:
```cpp
uint32_t PSID(uint32_t F) {
  if (NO_PIXEL_SHADER(F)) return 0x2000;       // single depth PS id (unused: no PS bound)
  uint32_t v = F & 0xFFFFFB83;                 // strip bits 2,3,4,5,6,10
  if (F & 0x1E00000) {                         // shadowmask: rebuild from quality INI
    v = (F & 0x1E00100) | 0x2002;
    if (dword_141E0DE30 & 1) v |= 0x20000;     // shadow filter quality bits 17/18/19
    if (dword_141E0DE30 & 2) v |= 0x40000;
    if (dword_141E0DE30 & 4) v |= 0x80000;
  }
  return v;
}
```

**BeginTechnique** @ 0x14131FBD0 (shared BSShader): scatter-table lookup `m_VertexShaderTable` (this+0x48 table: entries@+0x50, mask@+0x34) by VSID and `m_PixelShaderTable` (entries@+0x80, mask@+0x64) by PSID (skipped if ignorePS). Fail→false. Then `SetVertexShader` 0x140D6F9B0 and `SetPixelShader` 0x140D6FD60 (null when ignorePS). **No hull/domain shader handling exists (Nukem's HS/DS was his addition).**

```cpp
// 0x140D6F9B0
void Renderer::SetVertexShader(VertexShader* vs) {
  m_StateUpdateFlags |= 0x400;                          // DIRTY_VERTEX_DESC
  *(VertexShader**)0x1430281F8 = vs;                    // m_CurrentVertexShader
  ctx->VSSetShader(vs->m_Shader /*+0x08*/, nullptr, 0); // IMMEDIATE
}
// 0x140D6FD60
void Renderer::SetPixelShader(PixelShader* ps) {
  *(PixelShader**)0x143028200 = ps;                     // m_CurrentPixelShader (no dirty bit)
  ctx->PSSetShader(ps ? ps->m_Shader : nullptr, nullptr, 0); // IMMEDIATE
}
```
`ctx` = `[0x143027EA0]` = `[[0x1430261B0] + 926*8]` (same object).

**Shader structs (Nukem layout confirmed at every access):** VertexShader: +0 id, +8 ID3D11VertexShader*, +0x18/0x20 PerTechnique{buf,data}, +0x28/0x30 PerMaterial, +0x38/0x40 PerGeometry, +0x50 u8 m_ConstantOffsets[20] (float-offset per constant index). PixelShader: +0 id, +8 ID3D11PixelShader*, +0x10/0x18 PerTechnique, +0x20/0x28 PerMaterial, +0x30/0x38 PerGeometry, +0x40 u8 m_ConstantOffsets[64].

**Constant names (Nukem tables, all offsets verified in use):** VS: 0=World, 1=TexcoordOffset, 2=EyePos, 3=HighDetailRange, 4=ParabolaParam, 5=ShadowFadeParam, 6=TreeParams, 7=WaterParams, 8=Bones. PS: 0=AlphaTestRef, 1=RefractionPower, 2=DebugColor, 3=BaseColor, 4=PropertyColor, 5=FocusShadowMapProj(float3x4[4]), 6=ShadowMapProj(float3x4[3]), 7=ShadowSampleParam, 8=ShadowLightParam, 9=ShadowFadeParam, 10=VPOSOffset, 11=EndSplitDistances, 12=StartSplitDistances, 13=FocusShadowFadeParam. Samplers: 0 Base, 1 Normal, 2 Depth, 3 ShadowMap, 4 ShadowMapComp, 5 Stencil, 6 FocusShadowMapComp, 7 Grayscale.

## 3. SetupTechnique @ 0x14130DF90 (cleaned)

```cpp
bool BSUtilityShader::SetupTechnique(uint32_t technique) {
  uint32_t F = technique - 0x2B;
  uint32_t vsid = VSID(F), psid = PSID(F);
  bool hasPS = HasPixelShader(F);                       // formula above
  if (!BeginTechnique(vsid, psid, !hasPS)) return false;

  auto* vs = m_CurrentVertexShader /*[0x1430281F8]*/;
  auto* ps = m_CurrentPixelShader  /*[0x143028200]*/;
  // Map both PerTechnique CBs WRITE_DISCARD
  if (vs->PerTechnique.buf) { ctx->Map(vs->PerTechnique.buf,0,WRITE_DISCARD,0,&m); vs->PerTechnique.data = m.pData; }
  if (hasPS && ps->PerTechnique.buf) { ctx->Map(ps->PerTechnique.buf,0,WRITE_DISCARD,0,&m); ps->PerTechnique.data = m.pData; }

  this->F /*+0x90*/ = F;  this->vertexFlags /*+0x94*/ = F & 0x7F;

  if ((F & 0x1E00100) == 0x100) {                       // LOD_LANDSCAPE, not shadowmask
    float* hdr = vsData + vsOff[3];                     // HighDetailRange
    hdr[0] = g_HighDetailRange[0] /*0x141E0DF04*/ - PosAdjust.x /*0x14302820C*/;
    hdr[1] = g_HighDetailRange[1] - PosAdjust.y /*0x143028210*/;
    hdr[2] = g_HighDetailRange[2] - 15.0f;
    hdr[3] = g_HighDetailRange[3] - 15.0f;
  }

  if (F & 0x1E00000) {                                  // === SHADOWMASK family ===
    if (m_AlphaTestRef /*0x143027F64*/) { m_AlphaTestRef = 0; dirty |= 0x100; }
    if (F & 0x2000) {                                   // depth-reading mask (RENDER_DEPTH bit within mask)
      // stencil-off + depth mode 5 (DEPTHSTENCIL_STATE... test-only variant):
      if (F & 0x200000) { if (m_DepthMode/*F38*/) SetDepthMode(0); }   // directional: mode 0
      else if (m_DepthMode != 5) SetDepthMode(5);                      // local lights: mode 5
      // VPOSOffset = 1/RTdims of current render target:
      psData[psOff[10]+0] = 1.f / RTprops[m_RenderTargets[0]].width;   // 0x14302BB20 stride 28
      psData[psOff[10]+1] = 1.f / RTprops[...].height;
      psData[psOff[10]+2] = 0;
      // Bind main depth SRV to PS t2 (cached):
      int dsIdx = GetDepthStencilTarget_MAIN();          // 0x140D74E50 — returns 0 ALWAYS in 1.5.97
      SRV d = dsIdx==-1 ? 0 : DSData[dsIdx].depthSRV;    // 0x14302A4D0 + 152*idx + 0
      if (m_PSTexture[2] != d) { m_PSResourceModifiedBits|=4; dirty|=1; m_PSTexture[2]=d; g_DepthSRVBound/*0x1430284C2*/=1; }
      // sampler2 refresh: if addr[2]/filter[2] caches set, zero them + dirty sampler bit 4
      if (bool_141E0DE43 && g_FocusShadowCount /*0x1431D0FB8*/) {      // stencil SRV to t5
        SRV s = DSData[0].stencilSRV;                    // +8
        if (m_PSTexture[5] != s) { m_PSResourceModifiedBits|=0x20; dirty|=1; m_PSTexture[5]=s; g_DepthSRVBound=1; }
      }
      if (m_DepthBiasMode /*0x143027F50*/ != 1) { dirty|=0x40; m_DepthBiasMode = 1; }
    }
    float N = camera->frustum.near, Fz = camera->frustum.far;   // [0x1431D0E68]+0x160/+0x164
    if (F & 0x200000) {                                 // directional sun cascades
      auto* sun = shadowSceneNode->sunShadowDirLight;   // [0x141E0DED0]
      float* end = psData + psOff[11], *start = psData + psOff[12];
      for (i < sun->shadowMapCount) {                   // depth-buffer-space split distances
        end[i]   = (endSplit[i]*Fz - Fz*N) / ((Fz-N)*endSplit[i]);   // sun+0x5A4+16i
        start[i] = (startSplit[i]*Fz - Fz*N) / ((Fz-N)*startSplit[i]);// sun+0x598+16i
      }
      end[2] = end[count-1]; end[3] = (float)count; start[3] = 4.0f;
      if (bool_141E0DE4C)  { float* v = vsData + vsOff[3]; v[0..2] = end[0..2]; } // VS HighDetailRange reuse
      if ((u32)(dword_141E0DE34 - 2) <= 1) {            // filter mode 2/3: Poisson
        psData[psOff[7]+2] = fPoissonRadiusScale/*0x141E10670*/ / shadowmapRes/*0x143283B90*/;   // ShadowSampleParam.z
        psData[psOff[7]+3] = same;                                                              // .w
      }
    } else {                                            // spot/pb/dpb masks
      psData[psOff[11]+0] = g_143283B78;  psData[psOff[11]+1] = 4.0f;  psData[psOff[12]+3] = 4.0f;
      if (!m_ScissorEnabled/*F54*/) { dirty|=0x1000; m_ScissorEnabled = 1; }   // scissor ON
      if ((u32)(dword_141E0DE34 - 2) <= 1) {
        float r = fPoissonRadiusScale / shadowmapRes;
        psData[psOff[7]+2] = r;  psData[psOff[7]+3] = fPoissonRadiusScale * r;
      }
    }
    // FocusShadowFadeParam[i] for each active focus shadow:
    float maxD2 = fMaxFocusShadowMapDistance² /*0x141E106B8*/;
    for (i < g_FocusShadowCount)                        // data 0x1431D0FA8, stride 16, [0]=dist²
      psData[psOff[13]+i] = d2 >= fadeStart*maxD2 ? (maxD2-d2)/(maxD2 - fFadeStart*maxD2) : 1.0f; // 0x141E106A0
  }
  else if ((F & 0x40000) && hasPS) {                    // DEBUG_SHADOWSPLIT display
    for (i = 0..1) { t=(i+1)*0.5f; tmp[i] = powf(fShadowDist*0.2f, t)*5*0.5f + ((fShadowDist-5)*t+5)*0.5f; } // 0x143283B7C
    *(float2*)(psData + psOff[11]) = tmp;               // EndSplitDistances.xy
  }

  if ((F & 0x20004000) == 0x4000) {                     // RENDER_SHADOWMAP
    if (m_AlphaBlendMode /*0x143027F58*/) { dirty|=0x80; m_AlphaBlendMode = 0; }
    if (F & 0x10000) {                                  // _PB: ParabolaParam
      vsData[vsOff[4]+0] = 1.0f / g_ParabolaRadius /*0x141E10B78*/;
      vsData[vsOff[4]+1] = g_ParabolaSign /*0x141E10B7C*/;
    }
  }
  if ((F & 0x100000) && m_AlphaTestRef) { dirty|=0x100; m_AlphaTestRef = 0; }  // GRAYSCALE_MASK

  // Unmap + bind PerTechnique (b0):
  ctx->Unmap(vs->PerTechnique.buf, 0);
  ctx->VSSetConstantBuffers(0, 1, &vs->PerTechnique.buf);
  if (hasPS) { ctx->Unmap(ps->PerTechnique.buf, 0); ctx->PSSetConstantBuffers(0, 1, &ps->PerTechnique.buf); }
  return true;
}
```
(SetDepthMode(x) idiom everywhere: `if (m_DepthMode != x) { m_DepthMode = x; dirty = (m_DepthModePrev/*F3C*/ == x) ? dirty & ~4 : dirty | 4; }`.)

## 4. RestoreTechnique @ 0x14130DD80

```cpp
void BSUtilityShader::RestoreTechnique(uint32_t technique) {
  if (technique & 0x1E00000) {                          // NOTE: raw technique, NOT F!
    // clear cached PS SRVs t2,t3,t4,t6 (dirty resource bits 4,8,0x10,0x40)
    if (m_PSTexture[2]) { m_PSTexture[2]=0; resDirty|=4; }  // 0x143028000
    if (m_PSTexture[3]) { ... |=8; }  if (m_PSTexture[4]) { ... |=0x10; }  if (m_PSTexture[6]) { ... |=0x40; }
    if (m_ScissorEnabled) { m_ScissorEnabled=0; dirty|=0x1000; }
    if ((technique - 0x2B) & 0x2000) {                  // F here (mixed usage — replicate exactly)
      if (m_AlphaTestRef) { m_AlphaTestRef=0; dirty|=0x100; }
      if (m_PSTexture[2] == DSData[GetDepthStencilTarget_MAIN()].depthSRV) // 0x14302A4D0 (idx always 0)
        { resDirty|=4; dirty|=1; m_PSTexture[2]=0; g_DepthSRVBound=0; }
      SetDepthMode(3);                                  // default TEST+WRITE
      if (m_DepthBiasMode) { m_DepthBiasMode=0; dirty|=0x40; }
    }
  }
  if ((this->F & 0x1200) == 0x1200) {                   // STENCIL_ABOVE_WATER
    if (m_StencilModeRefPair /*0x143027F40 qword*/ != 0x00000000'FF000000-style (mode=0,ref=0xFF))
      { pair = {0, 0xFF}; dirty|=8; }
    if (m_AlphaBlendWriteMode /*0x143027F60*/ != 1) { m_AlphaBlendWriteMode=1; dirty|=0x80; }
  }
  EndTechnique(); // 0x14131FCE0 = nullsub
}
```

## 5. SetupMaterial @ 0x14130E890

Maps VS PerMaterial (+0x28) and PS PerMaterial (+0x20) WRITE_DISCARD (PS only if a PS is current). Then, only when `(F & 2) && (F & 0x2040280)` (TEXTURE && (ALPHA_TEST|RENDER_NORMAL|DEBUG_SHADOWSPLIT|RENDER_BASE_TEXTURE)):
- VS **TexcoordOffset** (vsOff[1]): 4 floats from material `+12+8*i, +16+8*i, +28+8*i, +32+8*i` where `i = dword_141E0DFF0` (double-buffer index) — texcoord offset xy + scale zw.
- If `F & 0x2040080` (ALPHA_TEST|DEBUG_SHADOWSPLIT|RENDER_BASE_TEXTURE): `t = material->vfunc[7]()` (GetType): type 2 → clampMode=mat+0x70, texture=mat+0x48; type 1 → clampMode=mat+0x80, texture=mat+0x58.
  - If `F & 0x20000000` (OPAQUE_EFFECT): PS **BaseColor** (psOff[3]) = (mat+0x48,+0x4C,+0x50)*mat+0x6C, w=mat+0x54 (baseColor*baseColorScale, w=alpha); bind **PS t7** = `mat->greyscaleTexture(+0x60)->rendererTexture(+0x48)->SRV(+0x10)` (cache 0x143028028, res bit 0x80), **sampler7** = addr 0 / filter 1 (sampler bits 0x80).
  - Bind **PS t0** = `texture->rendererTexture(+0x48)->SRV(+0x10)` (cache 0x143027FF0, bit 1), **sampler0** = addr `clampMode` / filter 3.
- If `F & 0x200` (RENDER_NORMAL): bind **PS t1** = `(mat+0x48)->rendererTexture->SRV` (cache 0x143027FF8, bit 2), **sampler1** = addr mat+0x70 / filter 3; PS **RefractionPower** (psOff[1]) = mat+0x84.

Unmap both; `VSSetConstantBuffers(1,1,&vsPerMat)`; `PSSetConstantBuffers(1,1,&psPerMat)` (PS bind only when PS current; VS-only path binds just b1 VS).

## 6. SetupGeometry @ 0x14130EC70

Maps VS PerGeometry (+0x38) and PS PerGeometry (+0x30) WRITE_DISCARD. **The geometry-CB float map (VS b2, 32 floats: ShadowFadeParam c0 / World c1-c4 / EyePos c5 / WaterParams c6 / TreeParams c7) is exactly the validated map at F:\claudetmp\rtprof\bsutility_geomcb_map.txt** — World written iff `(F&4)==0` (camera-relative `FUN_1412C3440`: 3×3 = scale*rotate, translate −= PosAdjust(0x14302820C/210), transposed to vsOff[0]; particle geoms re-center translate on modelBound.center first). Additional, previously-out-of-scope behavior:

- **Shadowmask branch** (`F & 0x1E00000`): `light = pass->sceneLights[0]` (BSShadowLight);
  - `FUN_14130F960(pass, 0)` binds shadow-map SRVs (below);
  - `SetupShadowLightParameters(pass, 0, psPerGeomBufferStruct)` @0x14130FBE0 (below);
  - VS ShadowFadeParam.x (vsOff[5]) **and PS ShadowLightParam.z (psOff[8]+8)** = `light->lodFade ? ShadowDistanceSquared(0x143283B88) : 1.0e8f`;
  - if `F & 0x1800000` (PB/DPB): PS ShadowLightParam.x (psOff[8]) = `light->light->radius.x`; elif `F & 0x400000` (SPOT): = `light->falloff`;
  - if `!(F & 0x200000)` (all non-directional masks): `RSSetScissorRects` **immediately** via 0x140D70100 with rect {l=bb.x, t=bb.y, r=bb.x+bb.w, b=bb.y+bb.h} from `light->projectedBoundingBox`; VS ShadowFadeParam.z (vsOff[5]+8) = saturate(depth-buffer-space distance of light along camera forward (camera node [0x1431D0F88]+0x7C rotation col0/3/6, position +0x9C) + radius, clamped to lodFade?fShadowDistance(0x143283B7C):10000, nonlinearized with near/far);
  - if `pass->accumulationHint == 10`: stencil mode 11, ref = `(LODMode>=0 ? fadeNode->currentFade : fadeNode->unk14C) * 31` (dirty 8).
- **Non-shadow branch** (`(F & 0x20004000) != 0x4000` required, i.e. skipped entirely for plain shadowmap):
  - `F & 0x100000` (GRAYSCALE_MASK) with `property->effectData->blockOutTexture`: bind **PS t0** = blockOutTexture SRV, sampler0 addr 3/filter 3; depth-bias mode 1 (dirty 0x40); depth mode 3; **save m_AlphaBlendWriteMode into dword_141E10660 and force write mode 0** (color writes off) — restored by RestoreGeometry;
  - `(F & 0x1E00100) == 0x100`: redundant World re-emit (same matrix);
  - `(F & 0x1200) == 0x1200` (STENCIL_ABOVE_WATER): stencil {mode=1, ref=0xFF} (qword 0x143027F40 = 0xFF00000001, dirty 8); blend write mode 0; depth mode 0; VS WaterParams.x (vsOff[7]) = water height `dword_141E0E014`; then **FUN_140D6FCF0(renderer, currentPS): Release()es `ps->m_Shader` and nulls it** (!);
  - else `F & 0x200` (RENDER_NORMAL): VS EyePos.xyz (vsOff[2]) = accumulator's eye pos (+356) transformed by inverse-ish object matrix `FUN_140D42C50(geometry->world)` (only if `F & 0x400`); EyePos.w = float at property+0x10C (`HIDWORD(shaderProperty[1].material)`);
  - `F & 0x20000` (DEBUG_COLOR/decal-count) with PS: e = pass->extraParam (float); if `dword_141E0DF94 == 10` e = 255*hashmapLookup(accumulator+208, fadeNode)/accum+240; PS DebugColor (psOff[2]) = {sat((e−128)/127), sat((128−e)/128), sat((128−|e−128|)/128), 1};
  - `F & 0x80000` (SIL_COLOR) with PS: PS DebugColor = float3 at accumulator+272, w=1.
- **TreeParams** (`F & 0x4000000`, vsOff[6]): {leaf? leaf+356*6 : 0, shadowSceneNode->windMagnitude, wind-curve clamp of leaf+344/348 (fast-rsqrt polynomial, globals 0x141E0DF70/0x141E0DF74), leaf? leaf+352 : 1.0} — leaf = fadeNode->AsLeafAnimNode() unless fadeNode is sentinel [0x1431D0DA8].
- **Alpha-test tail** (PS present AND (alphaProp with blend-enable bit0 set, OR `F & 0x80`, OR `(F&0x14000)==0x10000`)); alphaProp = `BSRenderPass::GetNiProperty(pass)` @0x1412FD8A0:
  - `F & 0x20000000`: PS PropertyColor (psOff[4]) = float3 at property+0xB8 (or 1,1,1 if null), w = property->alpha(+0x30);
  - PS AlphaTestRef.x (psOff[0]): effectData? `effectData->alphaTestRef/255`; elif alphaProp: blend-enabled → 0.99217f (0x3F7DFEFF); else `threshold(+0x32)/255 + 0.0039215293` (+1/255 more if threshold==4);
  - `(F & 0x14000) == 0x10000` (Aam): AlphaTestRef.w (psOff[0]+12) = (propFlags bit14 or bit46 ? float at property+0x104 : fadeNode->currentFade * property->alpha), ×fadeNode->unk14C if LODMode<0.
- Unmap; `VSSetConstantBuffers(2,1,&vsPerGeom)`; `PSSetConstantBuffers(2,1,&psPerGeom)` (PS bind only when PS current).

### FUN_14130F960 (shadow-map SRV binder)
`dsIdx = *( *(light + 0x18) + 0x54 )` (DS-target index of the light's rendered shadowmap — pointer chain typing unverified). If `light` and byte at light+~0x74 set: bind **PS t5** = DSData[GetDepthStencilTarget_MAIN()=0].stencilSRV (bit 0x20, dirty|=1, g_DepthSRVBound=1). If `light->vfunc(...)` predicate (focus shadows): bind **PS t6** = DSData[4].depthSRV (0x14302A730 = SHADOWMAPS target) (bit 0x40), sampler6 = addr 0/filter 4 (comparison). Then the light's own shadowmap depth SRV `DSData[dsIdx].depthSRV`: if `dword_141E0DE30 != 0` (filtered) → **PS t4** (bit 0x10) + sampler4 addr 0/filter 4; else → **PS t3** (bit 8) + sampler3 addr 0/filter 0. Each bind also sets dirty|=1 and g_DepthSRVBound=0.

### BSRenderPass::SetupShadowLightParameters @ 0x14130FBE0 (→ PS PerGeometry)
worldPt = (byte_141E0DE4C ? +origin(0x143012370) : −(renderMode([0x1431D0E28])∈{17,22} ? shadowSceneNode->lightingOffset : origin)) + PosAdjust. Then:
- frustum/parabolic light: **ShadowMapProj** (psOff[6]) = transpose(4×4 lightTransform from `light->shadowMapDataList._data[0]` with translation += M·worldPt) — full 4×4 write;
- directional: per cascade i (**only if `pass->extraParam & (1<<i)`** — the cascade-select bitmask lives in the pass!): 3 rows (float3x4) of the transposed adjusted matrix at psOff[6]+12i (descriptor stride 240, matrix at +0);
- focus shadows: for each of g_FocusShadowCount: float3x4 at **FocusShadowMapProj** psOff[5]+12i from `light->shadowMapData[i].projection`;
- bias into **AlphaTestRef.y/.z** (psOff[0]+4/+8): `.y = light->shadowBiasScale*0.00025*(directional?fShadowDirectionalBiasScale(0x141E10A38):1)*g_143283B8C`, `.z = light->shadowBiasScale*0.00025*(1/3)*g_141E1053C*g_143283B8C`.

## 7. RestoreGeometry @ 0x141310300
```cpp
if (F & 0x100000) { if (m_DepthBiasMode) {m_DepthBiasMode=0; dirty|=0x40;} SetDepthMode(3); }
if ((F & 0x1200) != 0x1200 && pass->accumulationHint == 10 && stencilPair != {0,0xFF}) { stencilPair={0,0xFF}; dirty|=8; }
if (dword_141E10660 != 13) {           // 13 = sentinel "nothing saved"
  if (m_AlphaBlendWriteMode != saved) { m_AlphaBlendWriteMode = saved; dirty|=0x80; }
  dword_141E10660 = 13;
}
```

## 8. SetBoneMatrix @ 0x14131F630
TLS-guarded (`TLS[0x143497408]+0x2A00` last-skin-instance, memory-context marker 26 at TLS+1896): if new skinInstance && partition && partition->m_usBones(+0x3C): calls `BSDismemberSkinInstance::sub_140D74F70(skinInstance)`; `count = 3 * skinData(+0x10)->boneCount(+0x58)`; `GetID3D11Resource(renderer/*0x143028490*/, count, &mapped, level)` (dynamic CB pool, maps a discard buffer of 16*count bytes) twice: level **10** ← memcpy skinInstance->boneMatrices(+0x48), then `Unmap` + `VSSetConstantBuffers(10, 1, &buf)`; level **9** ← skinInstance->prevBoneMatrices(+0x50), `Unmap` + `VSSetConstantBuffers(9, 1, &buf)`. (Bind slot = constant-group level; matches Nukem's BONES/PREVIOUS_BONES semantics.)

## 9. Renderer-globals block (absolute addresses, every access in this cluster)
- 0x1430261B0 Renderer base; +0x1CF0 (=0x143027EA0) **ID3D11DeviceContext\*** (R)
- 0x143027EB0 m_StateUpdateFlags (R/W: 1 RT, 4 depth-mode, 8 stencil-mode/ref, 0x40 depth-bias, 0x80 alpha-blend, 0x100 alpha-test-ref, 0x400 vertex-desc, 0x1000 scissor)
- 0x143027EB4 m_PSResourceModifiedBits (bit n = PS SRV slot n); 0x143027EB8 m_PSSamplerModifiedBits
- 0x143027EC8 m_RenderTargets[0] index (R by RT-dims getters); 0x143027EE8 m_CubeMapRenderTarget index (R)
- 0x143027F38 m_DepthStencilDepthMode (W: 0/1?/3/5); 0x143027F3C m_DepthStencilDepthModePrevious (R)
- 0x143027F40 qword {u32 m_DepthStencilStencilMode, u32 m_StencilRef} (W: {0,255},{1,255},{11,fade*31})
- 0x143027F50 m_RasterStateDepthBiasMode (W: 0/1); 0x143027F54 m_ScissorEnabled (W)
- 0x143027F58 m_AlphaBlendMode (W:0); 0x143027F5C m_AlphaBlendAlphaToCoverage (not touched by Utility); 0x143027F60 m_AlphaBlendWriteMode (W:0/1/restore)
- 0x143027F64 m_AlphaTestRef float (W:0)
- 0x143027F6C m_PSSamplerAddressMode[16] (W slots 0,1,2,3,4,6,7); 0x143027FAC m_PSSamplerFilterMode[16] (W: 0/1/3/4)
- 0x143027FF0 m_PSTexture[16] SRV cache (W slots 0,1,2,3,4,5,6,7)
- 0x1430281F8 m_CurrentVertexShader; 0x143028200 m_CurrentPixelShader (both R/W; = dword_143028070[98]/[100])
- 0x14302820C/0x143028210/0x143028214 PosAdjust xyz (camera-relative origin) (R)
- 0x1430284C2 depth-SRV-bound flag (W; consumed by SetDirtyStates 0x140D705B0)
- 0x14302A4D0 DS-target runtime array, stride 152: +0 depthSRV, +8 stencilSRV; entry4 depthSRV = 0x14302A730 (SHADOWMAPS)
- 0x14302BB20 RT properties array (stride 28: width+0, height+4; cubemap dims at +3192 stride 16) (R via 0x140D74C20/0x140D74C60)

Engine globals: 0x143495D50 pInstance; 0x141E0DED0 shadowSceneNode; 0x1431D0E68 BSShaderManager camera (frustum near/far +0x160/+0x164); 0x1431D0F88 camera node; 0x1431D0E28 render mode; 0x1431D0FA8/0x1431D0FB8 focus-shadow array/count; 0x1431D0DA8 sentinel fade node; 0x141E0DF04 HighDetailRange source; 0x141E0DFF0 texcoord buffer index; 0x141E0DE30 shadow-filter-quality bits; 0x141E0DE34 shadow filter mode; 0x141E0DE43/0x141E0DE4C shadow bools; 0x143283B78/B7C/B88/B8C/B90 shadow distance/dist²/bias-mult/shadowmap-res; 0x141E10670 fPoissonRadiusScale; 0x141E106A0/0x141E106B8 focus fade start/max dist; 0x141E10A38 directional bias scale; 0x141E1053C bias-z mult; 0x141E10B78/7C parabola radius/sign; 0x141E0E014 water height; 0x141E0DF70/74 wind-curve min/max; 0x141E10660 saved blend-write-mode; 0x143012370 default light origin; 0x143497408 TLS index (TLS+0x2A00 last skin instance, TLS+1896 memory-context).

## 10. Control flow within a pass (as called by RenderPassImmediately 0x141308440)
SetupTechnique(pass->passEnum) → [per-pass] SetupMaterial(property->material) → SetupGeometry(pass, flags) → (batch renderer sets vertex desc, alpha-blend state, draws) → RestoreGeometry(pass) → RestoreMaterial(nullsub) → RestoreTechnique on technique switch. All state writes go through the dirty-flag caches, flushed by `BSGraphics::Renderer::SetDirtyStates` (0x140D705B0) before DrawIndexed — EXCEPT: VSSetShader/PSSetShader (BeginTechnique), all cbuffer Map/Unmap/bind, RSSetScissorRects, and SetBoneMatrix binds, which hit the context immediately.

### Every D3D11 call (arg -> data source)
- Map(ctx, curVS->PerTechnique.buf [curVS=[0x1430281F8], buf=+0x18], 0, D3D11_MAP_WRITE_DISCARD, 0, &m) — SetupTechnique @0x14130E085; mapped ptr stored to curVS+0x20
- Map(ctx, curPS->PerTechnique.buf [curPS=[0x143028200], buf=+0x10], 0, WRITE_DISCARD, 0, &m) — SetupTechnique @0x14130E0D1, only when hasPixelShader; mapped ptr to curPS+0x18
- Unmap(ctx, curVS->PerTechnique.buf, 0) — SetupTechnique @0x14130E6C6/0x14130E84B
- Unmap(ctx, curPS->PerTechnique.buf, 0) — SetupTechnique @0x14130E6DE (hasPS only)
- VSSetConstantBuffers(0, 1, &curVS->PerTechnique.buf) — SetupTechnique @0x14130E6F4/0x14130E861; data = HighDetailRange (g_141E0DF04 − PosAdjust.xy, z/w−15) at vsOff[3], EndSplitDistances copy at vsOff[3] (focus), ParabolaParam (1/[0x141E10B78], [0x141E10B7C]) at vsOff[4]
- PSSetConstantBuffers(0, 1, &curPS->PerTechnique.buf) — SetupTechnique @0x14130E70A (hasPS); data = VPOSOffset(1/RTdims from 0x14302BB20[m_RenderTargets[0]]) psOff[10], End/StartSplitDistances (depth-space cascade splits from sunShadowDirLight+0x598/+0x5A4, near/far from [0x1431D0E68]+0x160/164) psOff[11]/[12], ShadowSampleParam.zw (fPoissonRadiusScale/shadowmapRes) psOff[7], FocusShadowFadeParam[i] (fade from 0x1431D0FA8 dist² vs fMaxFocusShadowMapDistance²/fFadeStart) psOff[13]
- VSSetShader(ctx, vertexShader->m_Shader(+8), NULL, 0) — Renderer::SetVertexShader 0x140D6F9B0 via BeginTechnique; shader from m_VertexShaderTable[VSID(F)] scatter lookup
- PSSetShader(ctx, pixelShader ? pixelShader->m_Shader(+8) : NULL, NULL, 0) — Renderer::SetPixelShader 0x140D6FD60 via BeginTechnique; NULL for depth-only techniques (NO_PIXEL_SHADER condition)
- Map/Unmap(ctx, curVS->PerMaterial.buf(+0x28), WRITE_DISCARD) — SetupMaterial 0x14130E890
- Map/Unmap(ctx, curPS->PerMaterial.buf(+0x20), WRITE_DISCARD) — SetupMaterial (PS present)
- VSSetConstantBuffers(1, 1, &curVS->PerMaterial.buf) — SetupMaterial @0x14130EBFA/0x14130EC51; data = TexcoordOffset float4 from material +12/+16/+28/+32 (+8*dword_141E0DFF0 buffer index) at vsOff[1]
- PSSetConstantBuffers(1, 1, &curPS->PerMaterial.buf) — SetupMaterial @0x14130EC19; data = BaseColor (mat+0x48..0x54 × mat+0x6C) psOff[3], RefractionPower (mat+0x84) psOff[1]
- Map/Unmap(ctx, curVS->PerGeometry.buf(+0x38), WRITE_DISCARD) — SetupGeometry @0x14130ECDF; 128B/32-float CB per the validated geomcb map
- Map/Unmap(ctx, curPS->PerGeometry.buf(+0x30), WRITE_DISCARD) — SetupGeometry @0x14130ED26 (PS present)
- VSSetConstantBuffers(2, 1, &curVS->PerGeometry.buf) — SetupGeometry @0x14130F8EC/0x14130F946; data = World (camera-relative FUN_1412C3440(geometry->world), transposed) vsOff[0], ShadowFadeParam.x/.z vsOff[5], EyePos vsOff[2] (accumulator+356 through FUN_140D42C50 matrix; .w=property+0x10C), WaterParams.x (water height 0x141E0E014) vsOff[7], TreeParams (leaf anim + shadowSceneNode->windMagnitude) vsOff[6]
- PSSetConstantBuffers(2, 1, &curPS->PerGeometry.buf) — SetupGeometry @0x14130F90B; data = AlphaTestRef.x (effectData->alphaTestRef/255 | 0.99217 blend | threshold(+0x32)/255+eps) psOff[0], AlphaTestRef.yz = shadow bias (light->shadowBiasScale*0.00025*dirScale*0x143283B8C, and *1/3*0x141E1053C*0x143283B8C) via SetupShadowLightParameters, AlphaTestRef.w = fade alpha (Aam), ShadowLightParam.x=falloff/radius .z=lodFade?distSq:1e8 psOff[8], ShadowMapProj float3x4[3] per cascade gated by pass->extraParam bit i (light->shadowMapDataList._data stride 240, translation += M·(−lightingOffset/origin + PosAdjust)) psOff[6], FocusShadowMapProj float3x4[4] psOff[5], DebugColor psOff[2], PropertyColor (property+0xB8 float3, w=property->alpha) psOff[4]
- RSSetScissorRects(ctx, 1, &{l=bb.x, t=bb.y, r=bb.x+bb.w, b=bb.y+bb.h}) — FUN_140D70100 (ctx vtable+360), called IMMEDIATELY from SetupGeometry @0x14130F025 for spot/PB/DPB shadowmask passes; rect = light->projectedBoundingBox
- ID3D11PixelShader::Release() — FUN_140D6FCF0 from SetupGeometry STENCIL_ABOVE_WATER path @0x14130F3A5: releases curPS->m_Shader(+8) and NULLS it (subsequent PSSetShader of this technique binds NULL)
- Map (inside GetID3D11Resource 0x140D6FFD0 dynamic-CB pool, not decompiled) + Unmap(ctx, bonesBuf, 0) + VSSetConstantBuffers(10, 1, &bonesBuf) — SetBoneMatrix 0x14131F630; data = memcpy of skinInstance->boneMatrices(+0x48), 16*3*boneCount bytes (boneCount = skinData(+0x10)+0x58)
- Unmap(ctx, prevBonesBuf, 0) + VSSetConstantBuffers(9, 1, &prevBonesBuf) — SetBoneMatrix; data = skinInstance->prevBoneMatrices(+0x50)
- DEFERRED (via dirty caches consumed by SetDirtyStates 0x140D705B0, not direct calls): PSSetShaderResources slots 0(base/blockout),1(normal),2(main depth SRV [0x14302A4D0]),3/4(light shadowmap depth SRV, unfiltered/filtered by 0x141E0DE30),5(main stencil SRV +8),6(SHADOWMAPS depth SRV 0x14302A730),7(grayscale); PSSetSamplers 0(addr=matClamp/3,filt=3),1(addr=matClamp,filt=3),3(0/0),4(0/4-comparison),6(0/4),7(0/1); OMSetDepthStencilState (depth modes 0/3/5, stencil {0,255}/{1,255}/{11,fade*31}); RSSetState (depth-bias mode 0/1, scissor on/off); OMSetBlendState (blend mode 0, write mode 0/1); alpha-test-ref 0

### Divergences from Nukem's 1.5.23 RE
- Nukem never RE'd BSUtilityShader as a class (no Shaders/BSUtilityShader.cpp); only the BSShaderInfo name tables exist. All tables verified correct against 1.5.97: every VS/PS constant index maps exactly to the m_ConstantOffsets slot used by the binary, including his guessed PS#13 FocusShadowFadeParam (confirmed by the focus-fade write loop at 0x14130E5B6).
- The 'other CB' in SetupGeometry is the current PIXEL shader's PerGeometry buffer ([0x143028200]+0x30, bound PSSetConstantBuffers slot 2) — the earlier geomcb-map agent's label 'pCurrentVertexShader+48' was wrong; corrected here.
- BSShader::SetupGeometryAlphaBlending (Nukem 1.5.23 0x1413360D0) is NOT called anywhere in the Utility path in 1.5.97; alpha-blend mode selection for utility passes lives in the BSBatchRenderer pass-walk (0x141308030 / 0x141307930 are the only per-pass writers of the alpha-blend globals 0x143027F58/F5C). Utility manages write-mode brackets itself via dword_141E10660.
- BSShader::SetupAlphaTestRef semantics (testRef * property->GetAlpha() → renderer m_AlphaTestRef) do NOT apply to Utility: SetupTechnique force-clears renderer alpha-test-ref (0x143027F64=0), and the per-pass ref goes into the PS PerGeometry AlphaTestRef constant with different math — blend-enabled → 0.99217f (0x3F7DFEFF), else threshold/255 + 0.0039215293 (+1/255 extra if threshold==4), no multiply by property alpha; effectData->alphaTestRef/255 takes priority.
- GetDepthStencilTarget_MAIN (0x140D74E50) is literally 'xor eax,eax; ret' in 1.5.97 — the depth/stencil SRVs always come from DS-target entry 0 (0x14302A4D0/+8); the callers' -1 checks are dead code.
- BeginTechnique 1.5.97 has NO hull/domain shader support (Nukem's HullShaders/DomainShaders maps are his own addition, 'removed from SkyrimSE.exe' as he notes) — only VSSetShader + PSSetShader.
- SetBoneMatrix: Nukem's GetShaderConstantGroup/Flush/ApplyConstantGroupVS abstraction resolves concretely to: dynamic-CB pool map (GetID3D11Resource 0x140D6FFD0), memcpy, Unmap, VSSetConstantBuffers with StartSlot == constant-group level (10=bones, 9=previous bones). His helper sub_140D74600 (1.5.23) = 0x140D74F70 in 1.5.97, called with skinInstance only in the decompile.
- RestoreTechnique mixes flag domains: tests raw technique for the shadowmask family (a2 & 0x1E00000) but (technique−43) for the 0x2000 bit — Nukem-style code that assumes F==technique for high bits; replicate exactly as-is.
- Directional cascade ShadowMapProj writes are gated per-cascade by pass->extraParam & (1<<i) — the cascade-select bitmask is carried in the BSRenderPass extraParam byte, a detail absent from Nukem's sources.
- STENCIL_ABOVE_WATER path Release()es the current PixelShader's ID3D11PixelShader and nulls the table entry (FUN_140D6FCF0, unique callsite) — after first use the technique runs PS-less. Not documented anywhere in skyrim64_test.

### Open questions
- FUN_140D6FCF0 releasing curPS->m_Shader in the STENCIL_ABOVE_WATER path: intent presumed 'demote to PS-less after first use' (the null survives in m_PixelShaderTable), but not runtime-verified; a replicator must decide whether to reproduce the Release or just bind PS=NULL for (F&0x1200)==0x1200.
- FUN_14130F960: derivation of the light's shadowmap DS-target index (v6 = *(*(light+0x18)+0x54)) — pointer-chain typing unverified (IDA's RE::BSLight stub size distorts field math); also the vtable predicate gating the t6/SHADOWMAPS bind (v8->vftable[1].SetLight name is bogus) and the light+~0x74 'shadowmap rendered' byte are unnamed.
- Identity of technique flag bits 5 (Nukem 'L') and 6 — both stripped from the VS id, no other reads found in this cluster.
- INI/global names for byte_141E0DE43 (gates stencil-SRV t5 bind) and byte_141E0DE4C (switches shadowmask VS to the focus/0x2002 variant and flips the SetupShadowLightParameters origin sign); also unk_143283B8C (final bias multiplier) and g_141E1053C exact meaning.
- Shadow bias lands at PS AlphaTestRef.y/.z (psOff[0]+4/+8) — assumed the compiled Utility PS reads bias from that cbuffer slot (CS package/Shaders/Utility.hlsl should confirm); not cross-checked against DXBC.
- Derived-property raw offsets: PropertyColor source ptr at property+0xB8, EyePos.w at property+0x10C, Aam fade override at property+0x104 — computed from Hex-Rays 'shaderProperty[1].field' expressions (sizeof(BSShaderProperty)=0xB8); which derived class (BSEffectShaderProperty/BSWaterShaderProperty?) owns them is unverified.
- GetID3D11Resource (0x140D6FFD0, dynamic CB ring used by SetBoneMatrix) internals not decompiled — Map type/pool recycling assumed WRITE_DISCARD ring per Nukem's GetShaderConstantGroup.
- ctor stores m_Type=8 via a decompiler-ambiguous offset expression (*(a1+8)); assumed the canonical BSShader::m_Type@0x20 — not disasm-verified.
- Depth-mode enum values used (0, 5 vs default 3, plus bias modes 0/1) map to Nukem's DEPTH_STENCIL_DEPTH_MODE / rasterizer depth-bias tables in Renderer::SetDirtyStates — exact D3D11 depth-stencil desc per mode belongs to the SetDirtyStates cluster and was not re-derived here.
- SetupTechnique F&0x40000 non-shadow powf write (DEBUG_SHADOWSPLIT split-display constants into EndSplitDistances.xy) purpose/consumer not verified (debug-only path).

### Key addresses
- 0x1418685B0 BSUtilityShader::vftable (primary, 10 slots; _0 @0x141868608, _1 @0x141868620; 0x141990B18/0x141990B40 are RTTI COL ptrs)
- 0x14130DCE0 BSUtilityShader::ctor (m_Type=8, pInstance store)
- 0x143495D50 BSUtilityShader::pInstance
- 0x141310770 ~BSUtilityShader
- 0x140C61A30 DeleteThis
- 0x14130DF90 BSUtilityShader::SetupTechnique
- 0x14130DD80 BSUtilityShader::RestoreTechnique
- 0x14130E890 BSUtilityShader::SetupMaterial
- 0x14130EC60 BSUtilityShader::RestoreMaterial (nullsub)
- 0x14130EC70 BSUtilityShader::SetupGeometry
- 0x141310300 BSUtilityShader::RestoreGeometry
- 0x14131F430 BSShader::GetTechniqueName (nullsub)
- 0x14131F7F0 BSShader::ReloadShaders(bool) thunk → 0x14131FB10
- 0x14131F800 BSShader::ReloadShaders(BSIStream*)
- 0x14131F630 BSShader::SetBoneMatrix (bones→VS b10, prev→VS b9)
- 0x141334900 UtilityTechniqueToVertexShaderID
- 0x141334970 UtilityTechniqueToPixelShaderID
- 0x14131FBD0 BSShader::BeginTechnique
- 0x14131FCE0 BSShader::EndTechnique (nullsub)
- 0x140D6F9B0 Renderer::SetVertexShader (immediate VSSetShader + dirty 0x400)
- 0x140D6FD60 Renderer::SetPixelShader (immediate PSSetShader)
- 0x140D6FCF0 ReleaseShaderCOM(shader) — releases +8 and nulls (STENCIL_ABOVE_WATER)
- 0x14130F960 BindShadowMapTextures(pass, lightIdx) — PS t3/t4/t5/t6 + samplers
- 0x14130FBE0 BSRenderPass::SetupShadowLightParameters (ShadowMapProj/FocusShadowMapProj/bias)
- 0x140D70100 Renderer::SetScissorRect (immediate RSSetScissorRects, ctx vtbl+360)
- 0x1412C3440 BuildCameraRelativeWorldMatrix (scale*rotate, translate−PosAdjust) [validated]
- 0x140D42C50 BuildObjectMatrixForEyePos [per validated geomcb map]
- 0x140D74C20 GetCurrentRTWidth(0x14302BB20)
- 0x140D74C60 GetCurrentRTHeight(0x14302BB20)
- 0x140D74E50 GetDepthStencilTarget_MAIN — returns 0 constant
- 0x140D74F70 BSDismemberSkinInstance helper (Nukem 1.5.23 0x140D74600)
- 0x140D6FFD0 GetID3D11Resource (dynamic CB pool for constant-group levels)
- 0x140D705B0 BSGraphics::Renderer::SetDirtyStates (consumer of all dirty caches)
- 0x1412966A0 BSShaderAccumulator::GetCurrentAccumulator
- 0x1412FD8A0 BSRenderPass::GetNiProperty (alpha property)
- 0x141308030 BSBatchRenderer pass-list walk (owns per-pass alpha-blend state — other cluster)
- 0x141307930 BSBatchRenderer::Func3 (alpha-blend writer — other cluster)
- 0x143027EA0 ID3D11DeviceContext* global (= [0x1430261B0]+0x1CF0)
- 0x143027EB0 m_StateUpdateFlags
- 0x143027EB4 m_PSResourceModifiedBits
- 0x143027EB8 m_PSSamplerModifiedBits
- 0x143027EC8 m_RenderTargets[0] index
- 0x143027EE8 m_CubeMapRenderTarget index
- 0x143027F38 m_DepthStencilDepthMode
- 0x143027F3C m_DepthStencilDepthModePrevious
- 0x143027F40 {stencilMode,u32 stencilRef} qword
- 0x143027F50 m_RasterStateDepthBiasMode
- 0x143027F54 m_ScissorEnabled
- 0x143027F58 m_AlphaBlendMode
- 0x143027F5C m_AlphaBlendAlphaToCoverage
- 0x143027F60 m_AlphaBlendWriteMode
- 0x143027F64 m_AlphaTestRef (float)
- 0x143027F6C m_PSSamplerAddressMode[16]
- 0x143027FAC m_PSSamplerFilterMode[16]
- 0x143027FF0 m_PSTexture[16] SRV cache
- 0x1430281F8 m_CurrentVertexShader
- 0x143028200 m_CurrentPixelShader
- 0x14302820C PosAdjust.x (camera-relative origin; .y/.z at 0x143028210/0x143028214)
- 0x1430284C2 depth-SRV-bound flag
- 0x143028490 Renderer data mid-block (this for setters; flt_143028470[8])
- 0x14302A4D0 DS-target runtime array (stride 152: +0 depthSRV, +8 stencilSRV)
- 0x14302A730 SHADOWMAPS (entry 4) depth SRV
- 0x14302BB20 render-target properties (stride 28: w,h; cubemap dims +3192)
- 0x141E0DED0 shadowSceneNode (SSN[0])
- 0x1431D0E68 BSShaderManager camera (frustum near/far +0x160/+0x164)
- 0x1431D0F88 camera node (WorldTransform +0x7C)
- 0x1431D0E28 BSShaderManager render mode
- 0x1431D0FA8 focus-shadow array (stride 16, [0]=dist²)
- 0x1431D0FB8 focus-shadow count (_used)
- 0x1431D0DA8 sentinel BSFadeNode
- 0x141E0DF04 HighDetailRange source float4
- 0x141E0DFF0 texcoord double-buffer index
- 0x141E0DE30 shadow-filter-quality bits (drives PS id bits 17-19 + t3/t4 select)
- 0x141E0DE34 shadow filter mode (Poisson gate: ==2/3)
- 0x141E0DE43 stencil-SRV bind gate bool
- 0x141E0DE4C focus-shadow VS variant bool
- 0x143283B78 EndSplitDistances.x for local-light masks
- 0x143283B7C fShadowDistance
- 0x143283B88 ShadowDistanceSquared
- 0x143283B8C shadow bias final multiplier
- 0x143283B90 shadowmap resolution (Poisson divisor)
- 0x141E10670 fPoissonRadiusScale
- 0x141E106A0 focus-shadow fade start fraction
- 0x141E106B8 fMaxFocusShadowMapDistance
- 0x141E10A38 fShadowDirectionalBiasScale
- 0x141E1053C bias .z multiplier global
- 0x141E10B78 parabola radius / 0x141E10B7C parabola sign
- 0x141E0E014 water height (STENCIL_ABOVE_WATER WaterParams.x)
- 0x141E0DF70/0x141E0DF74 wind-curve min/max
- 0x141E10660 saved blend-write-mode (GRAYSCALE_MASK bracket; sentinel 13)
- 0x143012370 default lighting origin float3
- 0x143497408 TLS index (TLS+0x2A00 last NiSkinInstance, TLS+1896 memory-context)

================================================================================================
## Cluster 3
================================================================================================


# Cluster report: shader/technique bind machinery (SkyrimSE 1.5.97)

Scope: BSShader::BeginTechnique + shader-table lookup, Renderer::Set(Vertex|Pixel)Shader, per-shader VS/PS struct layouts (constant-buffer metadata), the per-group Map/Unmap + SetConstantBuffers slot 0/1/2 protocol, the dynamic constant-group ring (bones etc.), and the vertexDesc→input-layout machinery inside SetDirtyStates. Verified instruction-level against the 1.5.97 unpacked binary; Nukem's skyrim64_test (1.5.23) used for naming.

---

## 1. Control flow (RenderPassImmediately → technique bound)

```
BSBatchRenderer::RenderPassImmediately (0x141308440)
  ├─ technique cache check: (g_CurrentTechnique(0x143283BA4)==Technique
  │     && Technique != 0x5C006076
  │     && Pass->m_Shader == g_CurrentShader(0x143283BA8))  → skip BeginPass
  ├─ else: dword_141E0DF8C = Technique (write-only debug global)
  │        BeginPass(Technique, Pass->m_Shader)  (0x1413086C0)
  │          ├─ [EndPass inlined] if g_CurrentShader: g_CurrentShader->vtbl[3] RestoreTechnique(g_CurrentTechnique)
  │          ├─ g_CurrentShader=0, g_CurrentTechnique=0, g_CurrentMaterial(0x143490BB0)=0
  │          ├─ ok = Shader->vtbl[2] SetupTechnique(Technique)
  │          └─ on ok: store g_CurrentShader/g_CurrentTechnique
  ├─ material: if (Pass->m_ShaderProperty ? property->material(+0x78) : 0) != g_CurrentMaterial
  │        → Shader->vtbl[4] SetupMaterial(material); g_CurrentMaterial = material
  └─ dispatch by geometry: skinned (geom+0x130 != 0) → RenderPassImmediately_Skinned 0x1413088C0
        custom flag (geom+0x109 & 8) → RenderPassImmediately_Custom 0x141308B20
        else → RenderPassImmediately_Standard 0x141308970   [other clusters]
```

### BSUtilityShader::SetupTechnique (0x14130DF90) — entry into my cluster

```cpp
bool BSUtilityShader::SetupTechnique(uint32_t Technique)   // vtable idx 2 @ 0x1418685B0+0x10
{
    uint32_t raw = Technique - 0x2B;                       // utility raw flag word
    uint32_t vsId = GetVertexTechniqueID(raw);             // 0x141334900
    uint32_t psId = GetPixelTechniqueID(raw);              // 0x141334970

    // "needs pixel shader" predicate (v6). When false the pass is pure depth:
    bool needsPS = (raw & 0x14000) == 0x14000              // RenderShadowmap|RenderShadowmapPb both
                || ((raw & 0x20004000) != 0x4000 && (raw & 0x1E02000) != 0x2000)
                || (raw & 0x80)                            // AlphaTest
                || (raw & 0x14000) == 0x10000;             // RenderShadowmapPb only

    if (!BSShader::BeginTechnique(this, vsId, psId, /*ignorePixelShader=*/!needsPS))
        return false;

    VertexShader* vs = *(VertexShader**)0x1430281F8;       // shadowState.m_CurrentVertexShader
    PixelShader*  ps = *(PixelShader**) 0x143028200;       // shadowState.m_CurrentPixelShader
    ID3D11DeviceContext* ctx = *(ID3D11DeviceContext**)0x143027EA0;

    // ---- PER-TECHNIQUE constant group (slot 0) protocol ----
    if (vs->m_PerTechnique.m_Buffer) {                     // vs+0x18
        D3D11_MAPPED_SUBRESOURCE m;
        ctx->Map(vs->m_PerTechnique.m_Buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        vs->m_PerTechnique.m_Data = m.pData;               // vs+0x20
    }
    if (needsPS && ps->m_PerTechnique.m_Buffer) {          // ps+0x10
        ctx->Map(ps->m_PerTechnique.m_Buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
        ps->m_PerTechnique.m_Data = m.pData;               // ps+0x18
    }

    this->currentRawTechnique   = raw;          // this+0x90
    this->currentRawTechnique7F = raw & 0x7F;   // this+0x94

    // constant writes go through per-shader offset tables:
    //   VS float* dst = (float*)vs->m_PerTechnique.m_Data + vs->m_ConstantOffsets[i];   (vs+0x50, bytes, x4 = float index... see §3)
    //   PS float* dst = (float*)ps->m_PerTechnique.m_Data + ps->m_ConstantOffsets[i];   (ps+0x40)
    // Observed writes (utility):
    //  (raw & 0x1E00100)==0x100 (LodLandscape, no shadowmask): VS[3] = float4(HighDetailRange(0x141E0DF04) - PosAdjust.xy(0x14302820C/..10), z-15, w-15)
    //  raw & 0x2000 (RenderDepth): PS[10].xy = {1/mainDepthWidth, 1/mainDepthHeight} (FUN_140d74c20/60 on 0x14302BB20), PS[10].z=0
    //     + depth-mode juggling (m_DepthStencilDepthMode=5 unless DebugColor bit), PSTexture[2] = mainDepth.DepthSRV (read-only-depth switch, sets bReadOnlyDepth@0x1430284C2)
    //     + PSTexture[5] = stencil SRV when byte_141E0DE43 && focus shadows active; rasterDepthBiasMode=1 (DIRTY 0x40)
    //  raw & 0x1E00000 (any RenderShadowmask*): per-cascade split distances from
    //     shadowSceneNode(0x141E0DED0)->sunShadowDirLight->cascade array (+0x598, stride 16):
    //       PS[11][i] = (End_i*far - far*near) / ((far-near)*End_i); PS[11].z = last; PS[11].w = shadowMapCount
    //       PS[12][i] = same formula on Start_i; PS[12].w = 4
    //     if byte_141E0DE4C: VS[3].xyz = PS[11].xyz
    //     if (iShadowFilterQuality-2)<=1: PS[7].zw = ShadowSampleParam = fPoissonRadiusScale(0x141E10670)/unk_143283B90
    //     focus-shadow fade array → PS[13][i]
    //  no shadowmask (plain depth): PS[11].x = unk_143283B78, PS[11].y = 4, PS[12].w = 4, rasterScissor=1 (DIRTY 0x1000)
    //  depth-only && raw & 0x10000 (RenderShadowmapPb): VS[4].xy = {1/ShadowRadius(0x141E10B78), ShadowSign(0x141E10B7C)}
    //  depth-only && raw & 0x40000 (OpaqueEffect?): PS[11].xy = tree LOD fade curve (powf on 0x143283B7C)
    //  raw & 0x100000 (GrayscaleMask): force m_AlphaTestEnabled(0x143027F64)=0 (DIRTY 0x100)
    //  (raw & 0x20004000)==0x4000 (plain RenderShadowmap): m_AlphaBlendMode(0x143027F58)=0 (DIRTY 0x80)

    // ---- flush + bind slot 0 ----
    if (vs->m_PerTechnique.m_Buffer) ctx->Unmap(vs->m_PerTechnique.m_Buffer, 0);
    if (needsPS) {
        if (ps->m_PerTechnique.m_Buffer) ctx->Unmap(ps->m_PerTechnique.m_Buffer, 0);
        ctx->VSSetConstantBuffers(0, 1, &vs->m_PerTechnique.m_Buffer);
        ctx->PSSetConstantBuffers(0, 1, &ps->m_PerTechnique.m_Buffer);
    } else {
        ctx->VSSetConstantBuffers(0, 1, &vs->m_PerTechnique.m_Buffer);   // PS group untouched
    }
    return true;
}
```

### BSShader::BeginTechnique (0x14131FBD0) — shared by ALL shaders

```cpp
bool BSShader::BeginTechnique(uint32_t vsId, uint32_t psId, bool ignorePixelShader)
{
    // m_VertexShaderTable at this+0x28, m_PixelShaderTable at this+0x58 (BSTScatterTable)
    //   table+0x0C : capacity (power of two; mask = cap-1)
    //   table+0x18 : end-sentinel entry pointer
    //   table+0x28 : entry array;  entry = 16 bytes { T* value @0, entry* next @8 }
    //   key is IMPLICIT: value->m_TechniqueID (first dword of VertexShader/PixelShader)
    VertexShader* vs = nullptr;  PixelShader* ps = nullptr;
    bool vsFound = scatter_find(this->m_VertexShaderTable, vsId, &vs);   // hash = id & (cap-1), walk ->next chain comparing (*entry.value)->m_TechniqueID
    bool psFound = ignorePixelShader ? true
                                     : scatter_find(this->m_PixelShaderTable, psId, &ps);
    if (!vsFound || !psFound) return false;

    Renderer_SetVertexShader(gRenderer /*0x143028490, unused*/, vs);     // 0x140D6F9B0
    Renderer_SetPixelShader (gRenderer, ignorePixelShader ? nullptr : ps); // 0x140D6FD60
    return true;
}
// BSShader::EndTechnique (0x14131FCE0) is an empty function.
```
Vanilla has **no hull/domain shader handling** here (Nukem's HullShaders/DomainShaders maps are his own additions).

### Renderer::SetVertexShader (0x140D6F9B0) / SetPixelShader (0x140D6FD60)

```cpp
void Renderer::SetVertexShader(VertexShader* s)          // NOTE: NO null check in vanilla
{
    shadowState.m_StateUpdateFlags |= DIRTY_VERTEX_DESC; // 0x400 @ 0x143027EB0  → forces input-layout re-resolve
    shadowState.m_CurrentVertexShader = s;               // 0x1430281F8 (base+0x348)
    ctx->VSSetShader(s->m_Shader /*s+8, unconditional deref*/, nullptr, 0);
}
void Renderer::SetPixelShader(PixelShader* s)
{
    shadowState.m_CurrentPixelShader = s;                // 0x143028200 (base+0x350). NO dirty flag.
    ctx->PSSetShader(s ? s->m_Shader : nullptr, nullptr, 0);
}
```
The only callers of these two setters in the whole binary are BeginTechnique and one standalone compositor FUN_1410a2370 (draws TriShapes with its own VS/PS, bypasses the technique system — not on the batch path).

---

## 2. Per-material (slot 1) and per-geometry (slot 2) — same protocol

BSUtilityShader::SetupMaterial (0x14130E890): Map(WRITE_DISCARD) `vs->m_PerMaterial.m_Buffer` (vs+0x28→data vs+0x30) and `ps->m_PerMaterial.m_Buffer` (ps+0x20→data ps+0x28) if non-null; write constants via offset tables (observed: VS[1] = landscape texture params from material+8*dword_141E0DFF0+12.., PS[3]=texture-fade term, PS[1]=material+132; plus PSTexture[0]/[1] SRV + sampler mode updates via modified-bit globals); Unmap; then **`VSSetConstantBuffers(1,1,&vsBuf)` and (if PS present) `PSSetConstantBuffers(1,1,&psBuf)`** — bound unconditionally, even if the buffer pointer is NULL.

BSUtilityShader::SetupGeometry (0x14130EC70): identical bracket on `m_PerGeometry` (vs+0x38→data vs+0x40; ps+0x30→data ps+0x38), constant writes per the validated geom-CB byte map (uses vs->m_ConstantOffsets[0](World transpose),[5],[6],[7] and ps offsets[0],[2],[4],[8]), then Unmap + `VSSetConstantBuffers(2,1,&vsBuf)` / `PSSetConstantBuffers(2,1,&psBuf)`.

**Slot protocol (verified): slot 0 = per-technique, slot 1 = per-material, slot 2 = per-geometry.** Every group upload is: Map(DISCARD) shader-owned buffer → write through offset table → Unmap → XSSetConstantBuffers(level, 1, &buffer). No partial binds, no SetConstantBuffers1.

If a group's `m_Buffer` is null the Map/Unmap is skipped, constants are written into the persistent CPU pointer `m_Data` (load-time choice), and the null pointer is still bound (unbinds the slot).

Oddity: in SetupGeometry, when `(raw & 0x1200) == 0x1200` (RenderNormal|RenderNormalClear, facegen normals path), after writing VS[7]=dword_141E0E014 it calls 0x140D6FCF0(renderer, currentPS) which does `if (ps && ps->m_Shader) { ps->m_Shader->Release(); ps->m_Shader = nullptr; }` — permanently drops the D3D object of that pixel shader struct (context still holds its bind ref). Flagged in open questions.

---

## 3. 1.5.97 VertexShader / PixelShader struct layouts (all offsets binary-verified)

```
BSGraphics::VertexShader (0x68 header + bytecode)          BSGraphics::PixelShader (0x80)
+0x00 uint32 m_TechniqueID   (scatter-table key)           +0x00 uint32 m_TechniqueID
+0x08 ID3D11VertexShader* m_Shader                         +0x08 ID3D11PixelShader* m_Shader
+0x10 uint32 m_ShaderLength  (bytecode bytes)              +0x10 Buffer m_PerTechnique { buf, data+0x18 }
+0x18 Buffer m_PerTechnique { ID3D11Buffer* @0x18,         +0x20 Buffer m_PerMaterial  { buf, data+0x28 }
                              void* m_Data  @0x20 }        +0x30 Buffer m_PerGeometry  { buf, data+0x38 }
+0x28 Buffer m_PerMaterial  { buf, data+0x30 }             +0x40 uint8 m_ConstantOffsets[64]
+0x38 Buffer m_PerGeometry  { buf, data+0x40 }
+0x48 uint64 m_VertexDescription
+0x50 uint8  m_ConstantOffsets[20]
+0x68 uint8  m_RawBytecode[]   (used by CreateInputLayout)
```
Offset-table semantics: `dst = (uint8*)group.m_Data + 4 * m_ConstantOffsets[i]` (offset unit = one float; 0xFF would be invalid — vanilla code doesn't check, offsets are trusted). Identical to Nukem's layouts.

---

## 4. Dynamic constant-group ring — Renderer::GetShaderConstantGroup (0x140D6FFD0)

```cpp
// returns pointer to a STATIC one-entry slot {ID3D11Buffer*} at 0x14302AC58; *outData = mapped pointer.
ID3D11Buffer** Renderer::GetShaderConstantGroup(uint32 sizeIGNORED, void** outData, uint32 level)
{
    ID3D11Buffer* buf;
    if (level == 7)  buf = *(ID3D11Buffer**)(g + 0x1CE0);            // single dedicated CB (grass/instance slot 7)
    else {
        uint32& idx = *(uint32*)(g + 0x1850);                        // m_NextConstantBufferIndex
        buf = ((ID3D11Buffer**)(g + 0x1858))[idx];                   // m_ConstantBuffers1[4] (3840B each per Nukem)
        idx = (idx + 1) & 3;
    }
    *(ID3D11Buffer**)0x14302AC58 = buf;
    D3D11_MAPPED_SUBRESOURCE m;  ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m);
    *outData = m.pData;
    return (ID3D11Buffer**)0x14302AC58;
}
```
The size argument and (except for 7) the level argument are **ignored**. Users: BSShader::SetBoneMatrix, BSGrassShader instance data, BSEffectShader::SetupGeometry, BSRenderPass::FUN_141307160 (light data), every image-space shader.

### BSShader::SetBoneMatrix (0x14131F630) — canonical ring usage (skinned casters in shadow passes)

```cpp
void BSShader::SetBoneMatrix(NiSkinInstance* si, Partition* part, const NiTransform* xf)
{
    // TLS cache: tls[0x2A00] (TLS index global 0x143497408) — skip if same NiSkinInstance
    if (tls_currentSkin == si || !part || part->m_usBones == 0) return;
    tls_currentSkin = si;
    sub_140D74F70(si /*, xf*/);                            // recompute si->m_pvBoneMatrices (world-space upload copies)
    uint32 regs = 3 * si->m_spSkinData->m_uiBones;          // 3 float4 rows per bone; bytes = 16*regs
    // BONES = slot 10:
    ID3D11Buffer** b = GetShaderConstantGroup(regs, &data, 10);
    memcpy_s(data, 16*regs, si->m_pvBoneMatrices /*si+0x48*/, 16*regs);
    ctx->Unmap(*b, 0);  ctx->VSSetConstantBuffers(10, 1, b);
    // PREVIOUS_BONES = slot 9:
    b = GetShaderConstantGroup(regs, &data, 9);
    memcpy_s(data, 16*regs, si->m_pvPrevBoneMatrices /*si+0x50*/, 16*regs);
    ctx->Unmap(*b, 0);  ctx->VSSetConstantBuffers(9, 1, b);
}
```

### Fixed-slot buffers bound once per frame — Renderer::ResetState (0x140D70290)

```cpp
RendererShadowState_InitDirty(0x140D73BD0);                 // set all dirty/modified bits
rendererData->byte50 = 0;                                    // +0x32-ish flag clear
ctx->PSSetConstantBuffers(11, 1, &g_AlphaTestRefCB);         // 0x143027A28 (16B). PS ONLY here.
ctx->VSSetConstantBuffers(12, 1, &g_PerFrameCB);             // 0x143027E88 (720B)
ctx->PSSetConstantBuffers(12, 1, &g_PerFrameCB);
```
The alpha-test-ref value itself is uploaded lazily in SetDirtyStates (below).

---

## 5. SetDirtyStates (0x140D705B0) — the flush that BeginTechnique's dirty bits feed

`SetDirtyStates(bool isComputeShader)` — full behavior (my cluster's parts in detail):

- **DIRTY_VERTEX_DESC (0x400, skipped when isComputeShader):**
  ```cpp
  uint64 key = shadowState.m_VertexDesc /*0x1430281F0*/ & m_CurrentVertexShader->m_VertexDescription /*vs+0x48*/;
  uint32 h = crc32_64(key);                                  // 0x140C06570, table 0x14175BF90
  // global hash map @ 0x141E07140: capacity 0x141E07144, sentinel 0x141E07150, entries 0x141E07160
  // entry = 24 bytes { uint64 key, ID3D11InputLayout* layout, entry* next }
  ID3D11InputLayout* il = map_find(h & (cap-1), key);
  if (miss) {
      il = CreateInputLayoutFromDesc(key);                   // 0x140D70F90, see below
      if (il != nullptr || key != 0x300000000407)            // that one desc is allowed to cache a NULL layout... 
          map_insert(key, il);                               // 0x140D730E0 (+grow 0x140D73F70)
  }
  ctx->IASetInputLayout(il);
  ```
  At the end: `m_StateUpdateFlags = isComputeShader ? (flags & 0x400) : 0` — a compute flush preserves the pending input-layout update.

- **CreateInputLayoutFromDesc (0x140D70F90):** builds D3D11_INPUT_ELEMENT_DESC[≤~12] from the 64-bit desc and calls `device->CreateInputLayout(elems, n, m_CurrentVertexShader->m_RawBytecode /*vs+0x68*/, vs->m_ShaderLength /*vs+0x10*/, &out)` (device global 0x143025F08). Element table (presence: bit44+n → InputSlot 0, bit54+n → InputSlot 1, neither → slot=-1):

  | n | Semantic | Format | AlignedByteOffset | notes |
  |---|----------|--------|-------------------|-------|
  | 0 | POSITION | 2 R32G32B32A32_FLOAT | 0 | always emitted |
  | 1 | TEXCOORD0 | 34 R16G16_FLOAT | (desc>>6)&0x3C | |
  | 2 | TEXCOORD1 | 10 R16G16B16A16_FLOAT | (desc>>10)&0x3C | |
  | 3 | NORMAL | 28 R8G8B8A8_UNORM | (desc>>14)&0x3C | |
  | 4 | BINORMAL | 28 | (desc>>18)&0x3C | |
  | 5 | COLOR | 28 | (desc>>22)&0x3C | |
  | 6 | BLENDWEIGHT (10) + BLENDINDICES (28) | | (desc>>26)&0x3C, +8 | pair |
  | 7 | TEXCOORD2 (28) + TEXCOORD3 (28) | | (desc>>30)&0x3C, +4 | pair |
  | 8 | TEXCOORD2 | 41 R32_FLOAT | (desc>>34)&0x3C | |
  | 9 | TEXCOORD4..7 | 10 each | (desc>>38)&0x3C +0/8/16/24 | InputSlotClass=1, StepRate=1 (instanced) |

- **DIRTY_RENDERTARGET (0x1):** resolves RT indices m_RenderTargets[8] (state+0x18) → `RendererData->pRenderTargets[idx].RTV` (RTdata* @0x143025F00, RTV @ +0xA58+0x30*i); cubemap via pCubemapRenderTargets (+0x26C8); DSV = pDepthStencils[m_DepthStencil].{Views|ReadOnlyViews}[slice] chosen by bReadOnlyDepth (RendererData+0x22); handles SRTM clear modes with ClearRenderTargetView(ClearColor @RendererData+0x2768)/ClearDepthStencilView; then OMSetRenderTargets(n, rtvs, dsv); mode slots set to SRTM_NO_CLEAR(4) after use.
- **0xC (depth modes):** `OMSetDepthStencilState(g_DepthStates[40*m_DepthStencilDepthMode + m_DepthStencilStencilMode] /*globals+0, [6][40]*/, m_StencilRef)`.
- **0x1070 (raster):** `RSSetState(g_RasterStates[fill*72 + cull*24 + depthBias*2 + scissor] /*globals+0x780*/)`; if 0x40 also rewrites viewport MinDepth/MaxDepth using floats 0x143028470/74 and depth-bias table 0x143026180[mode], setting DIRTY_VIEWPORT.
- **DIRTY_VIEWPORT (0x2):** `RSSetViewports(1, &m_ViewPort /*state+0x70*/)`.
- **DIRTY_ALPHA_BLEND (0x80):** `OMSetBlendState(g_BlendStates[52*mode + 26*a2c + 2*writeMode + extra] /*globals+0xC00*/, blendFactor@0x141E07168, 0xFFFFFFFF)`.
- **0x300 (alpha test):** Map(g_AlphaTestRefCB@0x143027A28, DISCARD); *(float*)p = m_AlphaTestEnabled(0x143027F64) ? m_AlphaTestRef(0x143027F68) : 0; Unmap. (Buffer already bound at slot 11.)
- **DIRTY_PRIMITIVE_TOPO (0x800):** `IASetPrimitiveTopology(m_Topology /*0x143028208*/)`.
- **modified-bit loops:** PSSetShaderResources(i,1,&m_PSTexture[i] /*0x143027FF0*/) per m_PSResourceModifiedBits(0x143027EB4); PSSetSamplers(i,1,&g_SamplerStates[5*addr[i]+filter[i]] /*globals+0x1760, [6][5]*/) per m_PSSamplerModifiedBits(0x143027EB8) with addr=0x143027F6C[], filter=0x143027FAC[]; CS equivalents (CSSetShaderResources/CSSetSamplers/CSSetUnorderedAccessViews) from the CS mirrors.

---

## 6. RendererShadowState (base 0x143027EB0) — verified field map

```
+0x000 0x143027EB0 m_StateUpdateFlags (DIRTY_* identical to Nukem's enum)
+0x004 0x143027EB4 m_PSResourceModifiedBits    +0x008 0x143027EB8 m_PSSamplerModifiedBits
+0x00C 0x143027EBC m_CSResourceModifiedBits    +0x010 m_CSSamplerModifiedBits  +0x014 m_CSUAVModifiedBits
+0x018 m_RenderTargets[8]   +0x038 m_DepthStencil  +0x03C m_DepthStencilSlice
+0x040 m_CubeMapRenderTarget  +0x044 m_CubeMapRenderTargetView
+0x048 m_SetRenderTargetMode[8]  +0x068 m_SetDepthStencilMode  +0x06C m_SetCubeMapRenderTargetMode
+0x070 m_ViewPort (D3D11_VIEWPORT; MinDepth +0x80, MaxDepth +0x84)
+0x088 0x143027F38 m_DepthStencilDepthMode   +0x08C 0x143027F3C m_DepthStencilDepthModePrev(active)
+0x090 0x143027F40 m_DepthStencilStencilMode +0x094 0x143027F44 m_StencilRef
+0x098 0x143027F48 m_RasterStateFillMode  +0x09C m_RasterStateCullMode
+0x0A0 0x143027F50 m_RasterStateDepthBiasMode  +0x0A4 0x143027F54 m_RasterStateScissorMode
+0x0A8 0x143027F58 m_AlphaBlendMode  +0x0AC m_AlphaBlendAlphaToCoverage  +0x0B0 0x143027F60 m_AlphaBlendWriteMode
+0x0B4 0x143027F64 m_AlphaTestEnabled (byte)   +0x0B8 0x143027F68 m_AlphaTestRef (float)
+0x0BC 0x143027F6C m_PSTextureAddressMode[16]  +0x0FC 0x143027FAC m_PSTextureFilterMode[16]
+0x140 0x143027FF0 m_PSTexture[16] (SRV*)
+0x1C0 0x143028070 m_CSTextureAddressMode[16]  +0x200 m_CSTextureFilterMode[16]
+0x240 m_CSTexture[16]  +0x2C0 m_CSTextureMinLodMode[16]  +0x300 0x1430281B0 m_CSUAV[8]
+0x340 0x1430281F0 m_VertexDesc (uint64)
+0x348 0x1430281F8 m_CurrentVertexShader   +0x350 0x143028200 m_CurrentPixelShader
+0x358 0x143028208 m_Topology
+0x35C 0x14302820C m_PosAdjust (NiPoint3) ... (m_PreviousPosAdjust/m_CameraData follow; m_AlphaBlendModeExtra @ +0x5D0 = 0x143028480)
```

Other renderer globals: HACK_Globals base 0x1430261B0 (state-object arrays + CB pools + context, see §5 offsets); RendererData* mirror 0x143025F00; ID3D11Device* 0x143025F08; ID3D11DeviceContext* 0x143027EA0; Renderer singleton 0x143028490 (Data @ +0x10 = 0x1430284A0; RendererData field offsets identical to Nukem: bReadOnlyDepth +0x22, pRenderTargets +0xA48, pDepthStencils +0x1FA8 (element 0x98; DepthSRV +0x88 = 0x14302A4D0 for elem 0), pCubemapRenderTargets +0x26C8, ClearColor +0x2768).

Batch-renderer statics: g_CurrentTechnique 0x143283BA4, g_CurrentShader 0x143283BA8, g_CurrentMaterial 0x143490BB0, write-only last technique 0x141E0DF8C.

## 7. Utility technique→shader-ID converters (raw = Technique − 0x2B)

```cpp
uint32 GetVertexTechniqueID(uint32 raw) {            // 0x141334900
    uint32 v = raw & 0xF7E5FF9F;                      // drop LodObject(27), DebugColor(19), DepthWriteDecals(17), bits 5,6
    if (byte_141E0DE4C && (v & 0x1E00000))            // shadowmask techniques
        v = (v & 0x1E00000) | 0x2002;                 // keep mask bits + Texture|RenderDepth
    if ((v & 0x14000) != 0x14000
        && (((v & 0x20004000)==0x4000) || ((v & 0x1E02000)==0x2000))
        && !(v & 0x80) && (v & 0x14000) != 0x10000)
        v &= 0xDFFFE1E4;                              // pure depth: strip to minimal VS
    return v;
}
uint32 GetPixelTechniqueID(uint32 raw) {             // 0x141334970
    if (/* same pure-depth predicate on raw */) return 0x2000;   // plain RenderDepth PS
    uint32 v = raw & 0xFFFFFB83;                      // drop Skinned/Normals/BinormalTangent/bit5,6/RenderNormalFalloff
    if (raw & 0x1E00000) {                            // shadowmask: rebuild from filter quality
        v = (raw & 0x1E00100) | 0x2002;
        if (dword_141E0DE30 & 1) v |= 0x20000;
        if (dword_141E0DE30 & 2) v |= 0x40000;
        if (dword_141E0DE30 & 4) v |= 0x80000;
    }
    return v;
}
```


### Every D3D11 call (arg -> data source)
- VSSetShader(vs->m_Shader, NULL, 0) <- vs = m_VertexShaderTable scatter-find(vsId) result stored to 0x1430281F8; called from Renderer::SetVertexShader 0x140D6F9B0 (no null check on vs); ctx from 0x143027EA0 (all calls below same ctx)
- PSSetShader(ps ? ps->m_Shader : NULL, NULL, 0) <- ps = m_PixelShaderTable find(psId), forced NULL when ignorePixelShader; stored to 0x143028200; Renderer::SetPixelShader 0x140D6FD60
- Map(vs->m_PerTechnique.m_Buffer /*vs+0x18*/, 0, D3D11_MAP_WRITE_DISCARD, 0, &m) then vs->m_PerTechnique.m_Data=m.pData <- SetupTechnique of each shader (utility 0x14130DF90); only if buffer non-null
- Map(ps->m_PerTechnique.m_Buffer /*ps+0x10*/, 0, WRITE_DISCARD, 0, &m) <- same, only when technique needs a pixel shader
- Unmap(vs->m_PerTechnique.m_Buffer, 0) / Unmap(ps->m_PerTechnique.m_Buffer, 0) <- SetupTechnique tail
- VSSetConstantBuffers(0, 1, &vs->m_PerTechnique.m_Buffer) <- SetupTechnique tail; slot 0 = per-technique; binds NULL if group has no GPU buffer
- PSSetConstantBuffers(0, 1, &ps->m_PerTechnique.m_Buffer) <- SetupTechnique tail, only when PS present
- Map/Unmap(vs->m_PerMaterial.m_Buffer /*vs+0x28*/, WRITE_DISCARD) + Map/Unmap(ps->m_PerMaterial.m_Buffer /*ps+0x20*/) <- SetupMaterial (utility 0x14130E890)
- VSSetConstantBuffers(1, 1, &vs->m_PerMaterial.m_Buffer) + PSSetConstantBuffers(1, 1, &ps->m_PerMaterial.m_Buffer) <- SetupMaterial tail; slot 1 = per-material
- Map/Unmap(vs->m_PerGeometry.m_Buffer /*vs+0x38*/, WRITE_DISCARD) + Map/Unmap(ps->m_PerGeometry.m_Buffer /*ps+0x30*/) <- SetupGeometry (utility 0x14130EC70); constants written at (float*)m_Data + m_ConstantOffsets[i] (vs table +0x50, ps table +0x40)
- VSSetConstantBuffers(2, 1, &vs->m_PerGeometry.m_Buffer) + PSSetConstantBuffers(2, 1, &ps->m_PerGeometry.m_Buffer) <- SetupGeometry tail; slot 2 = per-geometry
- Map(ringCB, 0, WRITE_DISCARD, 0, &m) <- Renderer::GetShaderConstantGroup 0x140D6FFD0; ringCB = level==7 ? globals+0x1CE0 : m_ConstantBuffers1[(idx++)&3] at globals+0x1858 (idx at globals+0x1850); size arg ignored; buffer ptr also stored to static 0x14302AC58
- Unmap(ringCB, 0) + VSSetConstantBuffers(10, 1, &ringCB) <- BSShader::SetBoneMatrix 0x14131F630; data = memcpy of skinInstance->m_pvBoneMatrices (si+0x48), 16*3*boneCount bytes
- Unmap(ringCB, 0) + VSSetConstantBuffers(9, 1, &ringCB) <- SetBoneMatrix, previous bones from si+0x50
- PSSetConstantBuffers(11, 1, &g_AlphaTestRefCB@0x143027A28) <- Renderer::ResetState 0x140D70290 (per-frame, PS only)
- VSSetConstantBuffers(12, 1, &g_PerFrameCB@0x143027E88) + PSSetConstantBuffers(12, 1, same) <- ResetState; per-frame CB (720B)
- Map(g_AlphaTestRefCB, 0, WRITE_DISCARD, 0, &m); *(float*)m.pData = m_AlphaTestEnabled(0x143027F64) ? m_AlphaTestRef(0x143027F68) : 0.0f; Unmap <- SetDirtyStates 0x140D705B0 on flags 0x300
- IASetInputLayout(layout) <- SetDirtyStates on DIRTY_VERTEX_DESC(0x400, skipped for compute); layout from hash map 0x141E07140/60 keyed on crc32_64(m_VertexDesc@0x1430281F0 & vs->m_VertexDescription@vs+0x48)
- CreateInputLayout(elemDescs, n, vs->m_RawBytecode /*vs+0x68*/, vs->m_ShaderLength /*vs+0x10*/, &out) on device @0x143025F08 <- 0x140D70F90 on layout-map miss; element formats/offsets decoded from 64-bit desc (see report table)
- IASetPrimitiveTopology(m_Topology@0x143028208) <- SetDirtyStates on 0x800
- OMSetRenderTargets(n, rtvs, dsv) <- SetDirtyStates on 0x1; rtvs = RendererData->pRenderTargets[m_RenderTargets[i]].RTV (+0xA58+0x30*i via RendererData* @0x143025F00), dsv = pDepthStencils[m_DepthStencil].(bReadOnlyDepth ? ReadOnlyViews : Views)[m_DepthStencilSlice]
- ClearRenderTargetView(rtv, RendererData->ClearColor@+0x2768) / ClearDepthStencilView(dsv, flags from SRTM mode, ...) <- SetDirtyStates SRTM clear modes
- OMSetDepthStencilState(g_DepthStates[40*m_DepthStencilDepthMode + m_DepthStencilStencilMode] @globals+0x0, m_StencilRef@0x143027F44) <- SetDirtyStates on 0xC
- RSSetState(g_RasterStates[72*fill + 24*cull + 2*depthBias + scissor] @globals+0x780) <- SetDirtyStates on 0x1070
- RSSetViewports(1, &m_ViewPort@state+0x70) <- SetDirtyStates on 0x2 (MaxDepth adjusted by depth-bias table 0x143026180[mode] when 0x40)
- OMSetBlendState(g_BlendStates[52*mode + 26*a2c + 2*writeMode + extra@0x143028480] @globals+0xC00, blendFactor@0x141E07168, 0xFFFFFFFF) <- SetDirtyStates on 0x80
- PSSetShaderResources(i, 1, &m_PSTexture[i]@0x143027FF0) <- SetDirtyStates loop over m_PSResourceModifiedBits@0x143027EB4
- PSSetSamplers(i, 1, &g_SamplerStates[5*m_PSTextureAddressMode[i] + m_PSTextureFilterMode[i]] @globals+0x1760) <- loop over m_PSSamplerModifiedBits@0x143027EB8
- CSSetShaderResources / CSSetSamplers / CSSetUnorderedAccessViews <- CS modified-bit loops, arrays at state+0x240/+0x1C0,+0x200/+0x300
- ID3D11PixelShader::Release() on ps->m_Shader (then nulled) <- 0x140D6FCF0 called from utility SetupGeometry when (raw & 0x1200)==0x1200 (RenderNormal|RenderNormalClear)

### Divergences from Nukem's 1.5.23 RE
- Vanilla 1.5.97 BSShader::BeginTechnique (0x14131FBD0) has NO hull/domain shader lookup or HSSetShader/DSSetShader calls - Nukem's HullShaders/DomainShaders maps and Renderer::SetHullShader/SetDomainShader are his additions, not game code.
- Renderer::SetVertexShader in vanilla dereferences Shader->m_Shader unconditionally (no null check); Nukem's reimplementation writes 'Shader ? Shader->m_Shader : nullptr'. Vanilla callers guarantee non-null VS.
- Nukem's constant-group implementation (CustomConstantGroup, unified ring buffer, VSSetConstantBuffers1 with first-constant offsets, FlushConstantGroup writing 0xFEFEFEFE) is his patched renderer, NOT vanilla. Vanilla: per-shader-owned pool buffers mapped WRITE_DISCARD inline in each Setup* function, full-buffer binds via plain VS/PSSetConstantBuffers, no Flush/Apply helper functions exist in the binary flow.
- Nukem's Renderer::GetShaderConstantGroup(VertexShader*,level) reads the buffer size via GetDesc and caches it into m_Buffer - pure Nukem patch behavior. The vanilla ring allocator 0x140D6FFD0 ignores the size argument entirely and only special-cases level==7.
- Vanilla dynamic-group allocator returns a pointer to a single STATIC slot (0x14302AC58) holding the chosen buffer; the result must be consumed (Unmap+bind) before the next call - implicit sequencing contract absent from Nukem's API.
- HACK_Globals base layout differs from Nukem's 1.5.23 header ordering: in 1.5.97 the block at 0x1430261B0 STARTS at m_DepthStates[6][40] (raster +0x780, blend +0xC00, samplers +0x1760, m_NextConstantBufferIndex +0x1850, CB ring +0x1858, alphaTestRefCB +0x1878, perFrameCB +0x1CD8, slot7 CB +0x1CE0, context +0x1CF0); the early fields Nukem lists (clear color, device, window, dynamic VB pool) are not at this base - device is mirrored at 0x143025F08 and RendererData* at 0x143025F00. m_DepthBiasFactors (12 floats) sits immediately BEFORE the base at 0x143026180.
- Input-layout resolution detail not in Nukem's RE: the lookup key is (shadowState.m_VertexDesc BITWISE-AND vertexShader->m_VertexDescription), hashed with a CRC32 over the 8 key bytes (0x140C06570, table 0x14175BF90), cached in a global 24-byte-entry hash map at 0x141E07140/0x141E07160; desc 0x300000000407 is special-cased to allow a permanently-NULL layout without insertion. Nukem's header only notes 'input layout may need to be created'.
- Per-frame slot bindings: vanilla ResetState (0x140D70290) binds the alpha-test-ref CB (slot 11) for PS ONLY, and the per-frame CB (slot 12) for VS+PS; Nukem's comments claim slot 11 is 'PS/VS'. No VSSetConstantBuffers(11,...) was found on this path.
- RendererShadowState is a plain global block at 0x143027EB0 in 1.5.97 (Nukem's 1.5.23 patch accesses it through his threaded-globals hack at different offsets, e.g. current technique/shader at GraphicsGlobals+0x3014/0x3018 vs absolute 0x143283BA4/0x143283BA8 here); field ORDER matches his RendererShadowState struct exactly.
- RendererData field offsets (bReadOnlyDepth 0x22, pDevice 0x38, pContext 0x40, pRenderTargets 0xA48, pDepthStencils 0x1FA8, pCubemapRenderTargets 0x26C8, ClearColor 0x2768) are identical between 1.5.23 and 1.5.97 - confirmed live in SetDirtyStates/SetupTechnique/RestoreTechnique.
- BSBatchRenderer::RenderPassImmediately/BeginPass logic including the 0x5C006076 re-setup exception and the write-only technique global (1.5.23 dword_141E32FDC == 1.5.97 dword_141E0DF8C) match Nukem line-for-line; 1.5.97 inlines EndPass into BeginPass's prologue.
- BSShader::SetBoneMatrix matches Nukem's decompile exactly (TLS slot 0x2A00 skin-instance cache, 3*boneCount*16 bytes, slots 10 then 9); his helper sub_140D74600 is 0x140D74F70 in 1.5.97.

### Open questions
- FUN_140D6FCF0: utility SetupGeometry under (raw & 0x1200)==0x1200 (RenderNormal|RenderNormalClear, facegen normals path) Releases and NULLs the current PixelShader's m_Shader (ps+8). Intent unverified - after this the struct permanently loses its D3D object (subsequent binds of that PS technique set a null pixel shader). Replication must reproduce the observable effect only if those techniques are exercised; flagged as a likely one-shot 'clear' trick.
- POSITION element format in generated input layouts is DXGI_FORMAT_R32G32B32A32_FLOAT (2) unconditionally (disasm-verified at 0x140D70FAD), implying float4 positions in the runtime vertex streams; not cross-checked against actual vertex buffer contents.
- Two TEXCOORD semantic-index-2 element variants exist in the layout builder (bits 51/61: R8G8B8A8_UNORM pair with TEXCOORD3; bits 52/62: lone R32_FLOAT). Assumed mutually exclusive per desc; not proven.
- POSITION InputSlot becomes 0xFFFFFFFF if neither presence bit 44 nor 54 is set - presumably never happens; unverified.
- Special vertex desc 0x300000000407 (POSITION+TEXCOORD0, slot-0) may legitimately produce and cache a NULL input layout - which draw uses it and why is unknown.
- Technique ID 0x5C006076 (BSSM_TILE+1) always forces SetupTechnique re-run in RenderPassImmediately; the technique's name/owner is unidentified (also unnamed in Nukem).
- Depth-bias float table at 0x143026180 (12 floats, indexed flat by m_RasterStateDepthBiasMode, subtracted from viewport MaxDepth) - row semantics ([3][4] selection, who writes it and the floats at 0x143028470/74 compared against viewport MinDepth/MaxDepth) not traced; belongs to the raster-state cluster but affects shadow rendering.
- FUN_1410A2370 (only other caller of Renderer::SetVertexShader/SetPixelShader): a self-contained compositor drawing TriShapes with VS/PS taken from its argument struct (+88/+96), sampler slot 0 forced to addr 0 / filter 2. Identity (likely facegen or LOD atlas blit) unconfirmed; bypasses BeginTechnique and the technique cache entirely.
- No VSSetConstantBuffers(11) (alpha-test-ref for VS) found; if utility vertex shaders reference the AlphaTestRefCB the binding must come from another site or never occurs - unresolved.
- The 4-buffer dynamic CB ring (globals+0x1858) has no visible fencing in the allocator; safety presumably relies on MAP_WRITE_DISCARD renaming. Per-frame wrap behavior unanalyzed.
- GetShaderConstantGroup level argument only distinguishes 7 vs rest; whether any caller besides grass uses level 7 (slot 7 CB at globals+0x1CE0, Nukem's 16-byte m_TempConstantBuffer4 position) unverified.
- SetupTechnique's depth-mode value 5 written to m_DepthStencilDepthMode for RenderDepth (0x2000) techniques does not map to a named value in Nukem's DepthStencilDepthMode enum (he documents 0,1,3,4,6) - actual D3D11_DEPTH_STENCIL_DESC for index 5 not extracted (state-object creation is another cluster).
- dword_141E0DE30 (shadow-mask filter level driving PS technique bits 0x20000/0x40000/0x80000) and byte_141E0DE4C / byte_141E0DE43 ini/setting origins not traced.

### Key addresses
- 0x141308440 BSBatchRenderer::RenderPassImmediately(Pass, Technique, AlphaTest, RenderFlags)
- 0x1413086C0 BSBatchRenderer::BeginPass (inlines EndPass: RestoreTechnique of previous shader, clears current shader/technique/material)
- 0x141308970 RenderPassImmediately_Standard (unskinned draw path, other cluster)
- 0x1413088C0 RenderPassImmediately_Skinned (calls SetBoneMatrix path)
- 0x141308B20 RenderPassImmediately_Custom (geom flag 8 path)
- 0x14131FBD0 BSShader::BeginTechnique(this, vsId, psId, ignorePixelShader) - single shared bind point for all BSShaders + imagespace shaders
- 0x14131FCE0 BSShader::EndTechnique (empty function)
- 0x140D6F9B0 BSGraphics::Renderer::SetVertexShader (VSSetShader + DIRTY_VERTEX_DESC + m_CurrentVertexShader)
- 0x140D6FD60 BSGraphics::Renderer::SetPixelShader (PSSetShader + m_CurrentPixelShader, no dirty flag)
- 0x140D6FCF0 Renderer::ReleasePixelShaderObject-like (Release + null *(arg+8))
- 0x140D6FFD0 Renderer::GetShaderConstantGroup(sizeIgnored, out pData, level) - 4-buffer WRITE_DISCARD ring / slot-7 CB; returns static slot 0x14302AC58
- 0x140D705B0 BSGraphics::Renderer::SetDirtyStates(bool isComputeShader)
- 0x140D70F90 Renderer::CreateInputLayoutFromVertexDesc(uint64 desc) - builds D3D11_INPUT_ELEMENT_DESCs, CreateInputLayout with current VS bytecode
- 0x140C06570 crc32_64(key) hash used for input-layout map (CRC table 0x14175BF90)
- 0x140D730E0 input-layout hash-map insert; 0x140D73F70 map grow
- 0x140D70290 Renderer::ResetState/SetPerFrameBuffers (re-init dirty flags; binds CB slot 11 PS-only, slot 12 VS+PS)
- 0x140D73BD0 RendererShadowState dirty-bit re-init
- 0x140D70100 Renderer::SetScissor-like used by utility SetupGeometry (projected bounding box)
- 0x14130DF90 BSUtilityShader::SetupTechnique (vtbl[2])
- 0x14130DD80 BSUtilityShader::RestoreTechnique (vtbl[3])
- 0x14130E890 BSUtilityShader::SetupMaterial (vtbl[4])
- 0x14130EC60 BSUtilityShader::RestoreMaterial (vtbl[5])
- 0x14130EC70 BSUtilityShader::SetupGeometry (vtbl[6])
- 0x141310300 BSUtilityShader::RestoreGeometry (vtbl[7])
- 0x141334900 BSUtilityShader technique->VS-ID converter (input = technique - 0x2B)
- 0x141334970 BSUtilityShader technique->PS-ID converter
- 0x14131F630 BSShader::SetBoneMatrix (secondary vtable; slots 10=bones, 9=prev bones)
- 0x140D74F70 skin-instance bone-matrix prep helper (Nukem's sub_140D74600)
- 0x14130FBE0 BSRenderPass::SetupShadowLightParameters (called from utility SetupGeometry)
- 0x141307160 BSRenderPass light-data setup (uses CB ring)
- 0x1412966A0 BSShaderAccumulator::GetCurrentAccumulator
- 0x1410A2370 standalone compositor - only non-BeginTechnique caller of Set(Vertex|Pixel)Shader
- 0x1418685B0 BSUtilityShader vtable
- 0x1430261B0 HACK_Globals base: +0x000 m_DepthStates[6][40]; +0x780 m_RasterStates[2][3][12][2]; +0xC00 m_BlendStates[7][2][13][2]; +0x1760 m_SamplerStates[6][5]; +0x1850 m_NextConstantBufferIndex; +0x1858 m_ConstantBuffers1[4] ring; +0x1878 m_AlphaTestRefCB; +0x1CD8 m_PerFrameCB(720B, slot 12); +0x1CE0 slot-7 CB; +0x1CF0 ID3D11DeviceContext*
- 0x143026180 m_DepthBiasFactors float[12] (before globals base)
- 0x143025F00 RendererData* mirror (-> 0x1430284A0); 0x143025F08 ID3D11Device*
- 0x143027EA0 ID3D11DeviceContext* (immediate context global)
- 0x143027EB0 RendererShadowState base (full field map in report §6; m_VertexDesc 0x1430281F0, m_CurrentVertexShader 0x1430281F8, m_CurrentPixelShader 0x143028200, m_Topology 0x143028208, m_PosAdjust 0x14302820C)
- 0x143028490 BSGraphics::Renderer singleton (Data @ +0x10 = 0x1430284A0; bReadOnlyDepth 0x1430284C2; pRenderTargets 0x143028EE8; pDepthStencils 0x14302A448, elem0 DepthSRV 0x14302A4D0)
- 0x14302AC58 static return slot of GetShaderConstantGroup
- 0x143283BA4 g_CurrentTechnique; 0x143283BA8 g_CurrentShader (BSShader*); 0x143490BB0 g_CurrentMaterial; 0x141E0DF8C write-only last-submitted technique
- 0x141E07140 input-layout map struct (capacity 0x141E07144, end sentinel 0x141E07150, entries 0x141E07160; entry = {u64 key, ID3D11InputLayout*, next})
- 0x141E07168 static blend factor passed to OMSetBlendState
- 0x143497408 TLS index global; TLS+0x2A00 current NiSkinInstance cache
- 0x141E0DED0 shadowSceneNode; sunShadowDirLight cascade split array at light+0x598 (stride 16, start/end floats)
- BSShader object layout: +0x20 m_Type, +0x28 m_VertexShaderTable, +0x58 m_PixelShaderTable (scatter tables: mask +0xC, sentinel +0x18, entries +0x28, 16B entries {value,next}, key = value->m_TechniqueID)
- BSUtilityShader: +0x90 currentRawTechnique, +0x94 raw & 0x7F
- VertexShader struct: 0x00 id, 0x08 m_Shader, 0x10 len, 0x18/0x28/0x38 Buffer{buf,data} per-technique/material/geometry, 0x48 m_VertexDescription, 0x50 m_ConstantOffsets[20], 0x68 bytecode
- PixelShader struct: 0x00 id, 0x08 m_Shader, 0x10/0x20/0x30 Buffer groups, 0x40 m_ConstantOffsets[64]

================================================================================================
## Cluster 4
================================================================================================

CLUSTER REPORT — BSBatchRenderer pass walk, SkyrimSE 1.5.97 (idb "skyrim")
==========================================================================

ANCHOR RESOLUTION (task anchors → verified identity)
- 0x141308030  BSBatchRenderer::RenderBatches(this, uint32_t& Technique, uint32_t& GroupIndex, BSSimpleList<uint32>*& PassIndexList, uint32_t RenderFlags)  [Nukem BSBatchRenderer::RenderBatches; state setters fully inlined]
- 0x141308440  BSBatchRenderer::RenderPassImmediately(Pass, Technique, AlphaTest, RenderFlags)
- 0x1413083B0  BSBatchRenderer::DiscardBatches (NOT ShaderSetup — ShaderSetup is 0x141309F80)
- 0x141308520  ClearShaderAndTechnique-noRestore: zeroes LastShader/LastTechnique/LastMaterial WITHOUT calling RestoreTechnique (NOT a render entry point)
- 0x141308540  zeroes the grouped-alpha GeometryGroup stack counter @0x143283BA0 (NOT a render entry point)
- Real per-pass paths: 0x1413088C0 = _Standard, 0x141308970 = _Skinned, 0x141308B20 = _Custom.
  ⚠ IDA autonames are SWAPPED: the function named "RenderPassImmediately_Standard" (0x141308970) is Skinned, the one named "…_Skinned" (0x1413088C0) is Standard. Verified by dispatch disasm at 0x1413084C5-0x1413084FF (skin!=NULL → 0x141308970) and by the bUseEarlyZ OR present only in 0x1413088C0.

CALL TOPOLOGY (verified via xrefs)
BSShaderAccumulator::FinishAccumulating family (0x1412CACD0, 0x1412CB2E0, 0x1412CBB20, 0x1412CBCC0, 0x1412CBE90, 0x1412CC3C0, 0x1412CC550, 0x1412CC620, 0x1412CC730 …)
  → BSShaderAccumulator::RenderBatches 0x1412CCE40 (the technique-range driver; task called it RenderGeometryGroup)
      → sub_141307DD0 (first/next) → {DiscardBatches 0x1413083B0 | RenderBatches 0x141308030} loop
          → RenderPassImmediately 0x141308440
              → BeginPass 0x1413086C0 → shader->SetupTechnique (vtbl+0x10)
              → shader->SetupMaterial (vtbl+0x20) on material change
              → _Standard 0x1413088C0 / _Skinned 0x141308970 / _Custom 0x141308B20
                  → ShaderSetup 0x141309F80 (SetupGeometryAlphaBlending 0x14131F440, SetupAlphaTestRef 0x14131F2A0, shader->SetupGeometry vtbl+0x30 [BSUtilityShader::SetupGeometry = 0x14130EC70])
                  → Draw 0x141307160 (geometry-type switch → renderer draw fns → DrawIndexed)
                  → shader->RestoreGeometry (vtbl+0x38)
  → GeometryGroup::ClearAndFreePasses 0x1413061B0, EndPass 0x141308760
Also reaching 0x141308440 directly (single synthesized passes, all utility-relevant):
- 0x1412E3B80 shadow-light MASK pass loop (game): per shadow-caster light, MakeRenderPass(BSUtilityShader::pInstance @*0x143495D50, prop=0, geom=*(*(0x1431D11A0)+72), technique = 43 + lightTypeBits, NumLights=1, lights=&light), ExtraParam=0xFF; techniques: parabolic: ((cond?0x8448:0x2000)|0x200002)+43; omni: (0x400000|0x2002)+43 (0x400100 variant if light byte set); default 0x800000, spot 0x1000000. Sets m_AlphaBlendWriteMode (0x143027F60, dirty 0x80) from table dword_141861380[lightType], restores to 1 after loop. RT setup first: RenderTargetManager(0x14302BB20)::SetDepthStencilRenderTarget(-1, mode 3) + SetRenderTarget(slot 0, RT 18, mode 0, 1).
- 0x1408D2D70 same loop for menu scene (SSN @0x141E0DED8, write-mode table 0x1416C04A8)
- 0x1412CCAD0 sky-occlusion pre-pass (SkyShader pInstance @*0x1432336C0, technique 2)
- 0x1412CD2C0 decal/TILE loop (technique 0x5C006075), 0x1412CD420 (technique 4653+shader *0x143495D50 on main depth target), ImageSpaceEffect::Render 0x14130CD20 / BSImagespaceShader 0x14130CF80
Alpha/effects paths:
- GeometryGroup::Render 0x141306240: flags&1 → RenderPersistentPassList 0x1413090C0, else batch->VFunc03(1, 0x5C006074=BSSM_BLOOD_SPLATTER, flags) [BSBatchRenderer vtbl slot 3 = 0x141307930]
- 0x1413094A0 depth-sorted grouped-alpha walker (called from BSShaderAccumulator::RenderEffects 0x1412CB930), interleaves batch->m_AlphaGroup(+0xF0) passes with a global stack of GeometryGroups @0x143490BD0 (count @0x143283BA0)

CLEANED PSEUDOCODE (faithful to 1.5.97)
--------------------------------------
// ---- batch-state globals ----
uint32_t&          g_AlphaGroupStackCount = *(uint32_t*)0x143283BA0;   // pending grouped-alpha lists
uint32_t&          g_LastTechnique = *(uint32_t*)0x143283BA4;
RE::BSShader*&     g_LastShader    = *(RE::BSShader**)0x143283BA8;
RE::BSShaderMaterial*& g_LastMaterial = *(RE::BSShaderMaterial**)0x143490BB0;
// BSShader vtbl: +0x10 SetupTechnique, +0x18 RestoreTechnique, +0x20 SetupMaterial, +0x30 SetupGeometry, +0x38 RestoreGeometry

// 0x1413086C0 — BeginPass(Technique@ecx, Shader@rdx)  [arg order!]
bool BeginPass(uint32_t Technique, BSShader* Shader) {
    if (g_LastShader) g_LastShader->RestoreTechnique(g_LastTechnique);   // inlined EndPass
    g_LastShader = nullptr; g_LastTechnique = 0; g_LastMaterial = nullptr;
    if (Shader->SetupTechnique(Technique)) { g_LastShader = Shader; g_LastTechnique = Technique; return true; }
    g_LastShader = nullptr; g_LastTechnique = 0; return false;
}
// 0x141308760 — EndPass(): RestoreTechnique if LastShader, then zero all three (incl. LastMaterial)

// 0x141308440 — RenderPassImmediately(Pass@rcx, Technique@edx, AlphaTest@r8b, RenderFlags@r9d)
void RenderPassImmediately(BSRenderPass* Pass, uint32_t Technique, bool AlphaTest, uint32_t RenderFlags) {
    BSShader* shader = Pass->m_Shader;          // +0x00
    BSGeometry* geom = Pass->m_Geometry;        // +0x10
    bool ready = (g_LastTechnique == Technique && Technique != 0x5C006076 && shader == g_LastShader);
    if (!ready) { *(uint32_t*)0x141E0DF8C = Technique; /*write-only*/ ready = BeginPass(Technique, shader); }
    if (!ready) return;
    BSShaderMaterial* mat = Pass->m_ShaderProperty ? *(BSShaderMaterial**)((uintptr_t)Pass->m_ShaderProperty + 0x78) : nullptr;
    if (mat != g_LastMaterial) { if (mat) shader->SetupMaterial(mat); g_LastMaterial = mat; }  // updated even when mat==NULL
    *(uint8_t*)((uintptr_t)geom + 0x108) = *(uint8_t*)&Pass->m_LODMode;  // FULL byte (Index|SingleLevel bit7) → ucCurrentMeshLODLevel; no masking (verified: movzx eax, byte [rbx+1Eh]; mov [rsi+108h], al)
    if (*(void**)((uintptr_t)geom + 0x130))                 // NiSkinInstance*
        RenderPassImmediately_Skinned(Pass, AlphaTest, RenderFlags);     // 0x141308970
    else if (*(uint8_t*)((uintptr_t)geom + 0x109) & 8)      // custom-render flag
        RenderPassImmediately_Custom(Pass, AlphaTest /*unused*/, RenderFlags);  // 0x141308B20
    else
        RenderPassImmediately_Standard(Pass, AlphaTest, RenderFlags);    // 0x1413088C0
}

// 0x141309F80 — ShaderSetup(Pass@rcx, Shader@rdx, AlphaTest@r8b, RenderFlags@r9d)
void ShaderSetup(BSRenderPass* Pass, BSShader* Shader, bool AlphaTest, uint32_t RenderFlags) {
    if (Shader != *(BSShader**)0x1432336C0 /*BSSkyShader::pInstance, value compare verified*/) {
        if ((RenderFlags & 4) && !IsGrassShadowBlacklist(Pass->m_PassEnum))   // 0x1412CCE20: (t - 0x5C000058) <= 3  == {GRASS_SHADOW_L,LS,LB,LSB}
            BSShader_SetupGeometryAlphaBlending(Shader, QAlphaProperty(Pass), Pass->m_ShaderProperty, AlphaTest);  // 0x14131F440, non-virtual
        if (AlphaTest && QAlphaProperty(Pass))
            BSShader_SetupAlphaTestRef(Shader, QAlphaProperty(Pass), Pass->m_ShaderProperty);   // 0x14131F2A0, non-virtual
    }
    Shader->SetupGeometry(Pass, RenderFlags);   // vtbl+0x30 (tail-jmp)
}
// 0x1412FD8A0 QAlphaProperty = *(NiAlphaProperty**)(geom + 0x120)  (property slot 0)

// 0x1413088C0 — _Standard(Pass@rcx, AlphaTest@dl, RenderFlags@r8d)
void RenderPassImmediately_Standard(BSRenderPass* Pass, bool AlphaTest, uint32_t RenderFlags) {
    // MemoryContextTracker: TLS[*(0x143497408)]+0x768 = 26, restored on exit
    ShaderSetup(Pass, Pass->m_Shader, AlphaTest || *(uint8_t*)0x14302C8E5 /*gState.bUseEarlyZ*/, RenderFlags);
    Draw(Pass);                               // 0x141307160
    Pass->m_Shader->RestoreGeometry(Pass, RenderFlags);
}

// 0x141308970 — _Skinned(Pass@rcx, AlphaTest@dl, RenderFlags@r8d)   (NO bUseEarlyZ OR)
void RenderPassImmediately_Skinned(BSRenderPass* Pass, bool AlphaTest, uint32_t RenderFlags) {
    BSGeometry* geom = Pass->m_Geometry;  BSShader* shader = Pass->m_Shader;
    NiSkinInstance* skin = *(NiSkinInstance**)((uintptr_t)geom + 0x130);
    sub_14131F7C0();   // TLS[tlsIdx]+0x2A00 dword = 0 (per-thread reset; Nukem 1.5.23: sub_141336450)
    if (geom->vfunc_0x1B0(geom)) {   // "IsBSSkinnedDecalTriShape?" (Nukem)
        ShaderSetup(Pass, shader, AlphaTest, RenderFlags);
        NiSkinPartition::Partition part;  /*ctor 0x140C7BAD0*/  part.m_usBones /*+0x3C*/ = 1;
        // shader as NiBoneMatrixSetterI (subobject at shader+0x10), vfunc slot 1 (+0x8):
        ((NiBoneMatrixSetterI*)((uintptr_t)shader + 0x10))->SetBoneMatrix(skin, &part, (NiTransform*)((uintptr_t)geom + 0x7C) /*world, verified lea [rbx+7Ch]*/);
        Draw(Pass);
        /*part dtor 0x140C7BB10*/
    } else {
        ShaderSetup(Pass, shader, AlphaTest, RenderFlags);
        struct SkinRenderData { NiBoneMatrixSetterI* setter; BSGeometry* geom; void* unk10=0;
                                uint32_t singleLevel; uint32_t lodIndex; float unk20=0.f; uint32_t vbOffset=0xFFFFFFFF; } sd;
        sd.setter = shader ? (NiBoneMatrixSetterI*)((uintptr_t)shader+0x10) : nullptr;  sd.geom = geom;
        sd.singleLevel = (Pass->m_LODMode >> 7) & 1;  sd.lodIndex = Pass->m_LODMode & 0x7F;
        BSDynamicTriShape* dyn = geom->vfunc_0x60(geom);   // As-dynamic upcast, may be null
        if (dyn) {
            uint32_t size = *(uint32_t*)((uintptr_t)dyn + 0x170);   // DynamicDataSize
            void* dst = Renderer_AllocateAndMapDynamicVertexBuffer(size, &sd.vbOffset);  // 0x140D6C8A0
            memcpy_s(dst, size, BSDynamicTriShape_LockDynamicDataForRead(dyn) /*0x140C723C0*/, size);  // 0x14130A030
            Renderer_UnmapDynamicVertexBuffer();          // 0x140D6C9E0: ctx->Unmap(dynVB[cur],0)
            BSDynamicTriShape_UnlockDynamicData(dyn);     // 0x140C72420
        }
        skin->vfunc_0x128(&sd);   // NiSkinInstance::Render — per-partition SetBoneMatrix + draw (outside cluster)
    }
    shader->RestoreGeometry(Pass, RenderFlags);   // tail-jmp vtbl+0x38
}

// 0x141308B20 — _Custom(Pass@rcx, [AlphaTest unused], RenderFlags@r8d)
void RenderPassImmediately_Custom(BSRenderPass* Pass, uint32_t RenderFlags) {
    BSShader* shader = Pass->m_Shader;
    shader->SetupGeometry(Pass, RenderFlags);                                   // note: BEFORE blending, unlike Standard
    BSShader_SetupGeometryAlphaBlending(shader, QAlphaProperty(Pass), Pass->m_ShaderProperty, true);
    if (QAlphaProperty(Pass)) BSShader_SetupAlphaTestRef(shader, QAlphaProperty(Pass), Pass->m_ShaderProperty);
    Draw(Pass);
    shader->RestoreGeometry(Pass, RenderFlags);
}

// ---- batch walk ----
// BSBatchRenderer layout (matches Nukem): +0x08 BSTArray<PassGroup>.data, map at +0x20 (capacity mask @+0x2C, sentinel @+0x38, entries @+0x48, 16-byte entries {key,groupIdx,next}),
// +0x50 m_CurrentFirstPass, +0x54 m_CurrentLastPass, +0x58 m_ActivePassIndexList {u32 item; node* next @+0x60}, +0x6C m_AutoClearPasses, +0x70 m_GeometryGroups[16], +0xF0 m_AlphaGroup.
// PassGroup = 0x30 bytes: m_Passes[5] + m_ValidPassBits @+0x28. Pass chain via BSRenderPass::m_PassGroupNext @+0x30.

// 0x141308030 — RenderBatches(this, &Technique, &GroupIndex, &PassIndexList, RenderFlags@r8d…5th arg)
bool RenderBatches(...) {
    uint32_t g = m_RenderPassMap.get(Technique);        // inlined scatter lookup
    bool alphaTest = false;  bool noState = (RenderFlags & 0x108) != 0;
    switch (GroupIndex) {   // compiled as sequential ifs 0,2,3,1,4; all setters inlined on shadow state
      case 0: if(!noState) CullMode(1); AlphaTestEnable(0); A2C(0); break;
      case 1: if(!noState) CullMode(1); AlphaTestEnable(1); alphaTest=true; if (*(uint8_t*)0x1431D0E5D) A2C(1); break;
      case 2: if(!noState) CullMode(0); AlphaTestEnable(0); A2C(0); break;
      case 3: if(!noState) CullMode(0); AlphaTestEnable(1); alphaTest=true; if (*(uint8_t*)0x1431D0E5D) A2C(1); break;
      case 4: if(!noState) CullMode(1); AlphaTestEnable(1); alphaTest=true; A2C(0); break;
    }
    // CullMode: dword 0x143027F4C, dirty|=0x20 @0x143027EB0 ; AlphaTestEnable: dword 0x143027F64, dirty|=0x100 ; A2C: dword 0x143027F5C, dirty|=0x80
    for (BSRenderPass* p = PassGroupData[g].m_Passes[GroupIndex]; p; p = p->m_PassGroupNext)
        RenderPassImmediately(p, Technique, alphaTest, RenderFlags);
    if (m_AutoClearPasses) { PassGroupData[g].m_ValidPassBits &= ~(1<<GroupIndex); PassGroupData[g].m_Passes[GroupIndex] = nullptr; }
    EndPass();  // inlined: RestoreTechnique + zero Last{Shader,Tech,Material}
    if (A2C_on) A2C(0);   // reset alpha-to-coverage
    ++GroupIndex;
    return sub_141307DD0(this, &Technique, &GroupIndex, &PassIndexList);   // tail
}

// 0x1413083B0 — DiscardBatches: g = map.get(Technique); if (m_AutoClearPasses) zero all 6 qwords of PassGroup[g]; tail sub_141307DD0.
// 0x141307DD0 — GetFirstOrNextTechnique: if (!Technique) return AdvanceInActiveList(); g=map.get(Technique); return FindNextSlot(g,&GroupIndex) || AdvanceInActiveList();
// 0x141307E80 — AdvanceInActiveList: node=*PassIndexList; skip items < m_CurrentFirstPass; fail if item > m_CurrentLastPass; Technique=item;
//               if m_AutoClearPasses: pop head of m_ActivePassIndexList (copy next.item into head, free node via 0x141308FE0(node, pool @0x143490BC0) — locked freelist push, or operator delete(16) if pool ptr null);
//               else PassIndexList = node->next; return FindNextSlot(map.get(Technique), &GroupIndex);
// 0x141307FC0 — FindNextSlot(groupIdx, &GroupIndex): if GroupIndex>4 → GroupIndex=0,false; scan slots GroupIndex..4 for non-null m_Passes[slot]; found → GroupIndex=slot,true; else GroupIndex=0,false.

// 0x1412CCE40 — BSShaderAccumulator::RenderBatches(StartTech, EndTech, RenderFlags, GroupIdx)
//   SetCurrentAccumulator(this) [0x1412966B0 → *(0x1431D0E20)=this];
//   GroupIdx<=-1: batch=this->m_BatchRenderer(+0x130); else grp=batch0->m_GeometryGroups[GroupIdx](batch0+0x70+8i), batch=grp->m_BatchRenderer;
//   m_CurrentTech(+0x138)=0; if batch { batch->first(+0x50)=StartTech; last(+0x54)=EndTech; m_CurrentGroup(+0x13C)=0; listPos=&batch->m_ActivePassIndexList(+0x58);
//   m_CurrentActive(+0x140) = batch->GetFirstOrNext(...);
//   while (active) active = (IsGrassShadowBlacklist(m_CurrentTech) && (byte(this+0x128) /*m_1stPerson*/ || byte(this+0x129)))
//        ? batch->DiscardBatches(...) : batch->RenderBatches(..., RenderFlags); }
//   if (grp) GeometryGroup::ClearAndFreePasses(grp) [0x1413061B0: passList head/tail=0; batch→ClearRenderPasses 0x141306DB0 (zero every PassGroup via map iteration, free all active-list nodes); grp->m_Count(+0x24)=0];
//   EndPass() [0x141308760].

// 0x1413090C0 — RenderPersistentPassList(PersistentPassList* list, uint32_t RenderFlags)
//   if (!list->m_Head) return;  EndPass-inline;
//   for (p = list->m_Head; p; p = p->m_PassGroupNext /* +0x30, VERIFIED mov rbx,[rbx+30h] — NOT m_Next(+0x28) */) {
//     if (!p->m_Geometry) continue;
//     if (!(RenderFlags & 0x108)) CullMode( (p->m_ShaderProperty->flags(+0x38) & (1ull<<36) /*kTwoSided*/) ? 0 : 1 );
//     alphaTest = alphaProp(geom+0x120) && (word(alphaProp+0x30) & 0x200);   // NiAlphaProperty::alphaFlags bit9 = alpha testing
//     RenderPassImmediately(p, p->m_PassEnum, alphaTest, RenderFlags);
//   }
//   if (!(RenderFlags & 0x108)) CullMode(1);
//   list->m_Head = list->m_Tail = nullptr;  EndPass-inline.

// 0x141307930 — BSBatchRenderer::VFunc03(StartTech, EndTech, RenderFlags)  (vtbl slot 3; called by GeometryGroup::Render when !(flags&1))
//   if StartTech==0 → range [1, 0x5C006074 /*BSSM_BLOOD_SPLATTER*/];
//   walk every node of m_ActivePassIndexList (non-destructive, follows +0x60/+8 next chain):
//     skip tech > EndTech (break) / < StartTech (continue); g = map.get(tech);
//     if (IsGrassShadowBlacklist(tech) && (curAcc(0x1431D0E20)->byte0x128 || byte0x129)) → zero PassGroup[g] (6 qwords);
//     else for slot 0..4 with non-null m_Passes[slot]: same per-slot state table as RenderBatches; chain-render RenderPassImmediately(p, tech, slotAlphaTest, RenderFlags); inline EndPass (RestoreTechnique + zero Last*) — NO per-slot A2C reset;
//     AutoClear → zero group.
//   epilogue: CullMode(1) unless flags&0x108; AlphaTestEnable(0); A2C(0); EndPass.

// 0x1413094A0 — grouped-alpha depth-sorted walker (BSShaderAccumulator::RenderEffects):
//   own = batch->m_AlphaGroup (+0xF0); global stack: GeometryGroup arr @0x143490BD0 (stride 0x28, depth at +0x20), count @0x143283BA0;
//   depth(own head) = dot(geom->worldBound.center(+0xE4), dir[0..2]) - a3 * radius(+0xF0);
//   pops stack entries with own-depth >= entry-depth-… via BSShaderAccumulator::RenderPersistentPassList (0x141306240 wrapper);
//   for own passes: skip if (dword 0x1431D0E28 == 18 && property->vfunc(+0x10)() == 0x1431D1F00); CullMode from property kTwoSided; RenderPassImmediately(p, p->m_PassEnum, AlphaTest=TRUE, RenderFlags); pop own list (head=head->m_PassGroupNext, --count(+0x24));
//   epilogue CullMode(1).

// 0x141307160 — Draw(Pass@rcx). switch (geom->QType @ +0x150), jumptable indexed by type:
//  type 1 PARTICLES: cnt=min(GetActiveVertexCount() /*vfunc +0x130 on obj at geom+0x158*/, 2048); dynVtxSize=(desc(+0x148)>>2)&0x3C;
//     shape=GetParticlesDynamicTriShape() /*0x140D6C7E0, magic-static @0x14302AE50: VB=*(0x143025F40), IB=*(0x143025F38), desc=0x0840200004000051*/;
//     Map size=4*cnt*dynVtxSize (0x140D6CA10→0x140D6C8A0, offset → shape+0x18); PackParticleData 0x140D76080(cnt, geom, dst); Unmap 0x140D6CA30;
//     DrawDynamicTriShape 0x140D6CA60(renderer, shape, &drawData, startIndex=0, triCount=2*cnt) → 0x140D6CAB0.
//  type 2 STRIP_PARTICLES: builder BSStripParticleSystem 0x140D76D80(@0x143283BB0, geom, @0x143477BB0, &cnt, &verts); if both → strip draw 0x140D6CE60 (writes verts(stride 40)+indices into dyn ring, lazy layout POSITION/TEXCOORD/NORMAL/COLOR @*(0x143025F50), topology 5, DrawIndexed(idxCnt,0,0)).
//  type 3 TRISHAPE: DrawTriShape 0x140D6BFE0(renderer, rendererData=*(geom+0x138), startIndex=0, triCount=word(geom+0x158)).
//  type 4 DYNAMIC_TRISHAPE: dyn=vfunc+0x60; if (dyn->uiFrameCount(+0x174) != *(uint32*)0x14302C8DC /*gState.uiFrameCount*/): stamp; map (size=*(rendererData+0x1C), offset→rendererData+0x18); memcpy_s from LockDynamicDataForRead; Unmap; UnlockDynamicData; then DrawDynamicTriShape(renderer, rendererData=*(dyn+0x138), &dyn->DrawData(+0x178), 0, word(dyn+0x158)).
//  type 5 MESHLOD_TRISHAPE: cnt=0x141330240(geom, lod=byte(geom+0x108)); if cnt: start=0x1413300C0(geom); DrawTriShape(rendererData, start, cnt).   [IMPLEMENTED — Nukem asserted]
//  type 6 LOD_MULTIINDEX: if (Pass->m_AccumulationHint(+0x1C, BYTE cmp verified)==12): DrawTriShapeAltIB 0x140D6C0E0(rendererData, start=0x141330850(geom,1), cnt=0x1413308F0(geom,lod,1), altIB=geom+0x160) else DrawTriShape(rendererData, 0x141330850(geom,0), 0x1413308F0(geom,lod,0)).
//  type 7 MULTIINDEX: hint==12 → DrawTriShapeAltIB(rendererData, 0, cnt=*(ptr(geom+0x150)), geom+0x160) else DrawTriShape(rendererData, 0, word(geom+0x158)).
//  type 8 SUBINDEX: BSSubIndexTriShape 0x140D59430(geom); if (*(uint8*)0x1430243B0) DrawTriShape(rendererData,0,word(geom+0x158)) /*whole shape*/; else per-segment: segs at *(geom+0x160), stride 20 {u32 startIdx@0, u32 triCnt@12, u32 enabled@16}, count *(ptr(geom+0x150)) or 1 if byte(geom+0x181); DrawTriShape per enabled segment (whole-shape counts if byte(geom+0x181)).
//  type 9 SUBINDEX_LAND: if (*(uint8*)0x1434963C8) single DrawTriShape; else per-segment descending, plus direct PS-texture cache writes: m_PSTexture[slot]=srv and m_PSTexture[slot+7]=srv (slots 5..0, srv from *(0x14302C8E8)+0x48→+0x10 default land texture), dirty m_PSResourceModifiedBits(0x143027EB4) |= 1<<slot | 1<<(slot+7); segment flags via 0x141334070.
//  type 10 MULTISTREAMINSTANCE (grass): per instance-group entry v at *(geom+0x160)+8i (count dword(geom+0x180)): if (v && *(v+0x50)): buf=GetCB(0x140D6FFD0, group 7 → CB *(0x143027E90); Map(cb,0,DISCARD,0,&p)); *(uint32*)p = i; ctx->Unmap(cb,0); ctx->VSSetConstantBuffers(7,1,&cb); DrawInstanced 0x140D6C1E0(renderer, rendererData, startIdx=0, triCount=word(geom+0x158), instCount=*(v+0x4C), desc=qword(geom+0x148), instVB=&*(v+0x40)).
//  type 11 PARTICLE_SHADER_DYNAMIC_TRISHAPE: dyn=vfunc+0x60; DrawParticleShaderTriShape 0x140D6CBE0(renderer, data=LockDynamicDataForRead, vtxCount=word(dyn+0x15A)); UnlockDynamicData.
//  type 12 LINES: DrawLineShape 0x140D6D310(renderer, rendererData, count=word(geom+0x158)).
//  type 13 DYNAMIC_LINES: map 0x140D6D5D0 (size *(rd+0x1C), offset→rd+0x18); memcpy_s from LockDynamicData; unmap 0x140D6D5F0; DrawDynamicLines 0x140D6D620(renderer, rd, 0, word(geom+0x158)); then rd->offset(+0x18)=-1. 0x141317E80 = unlock.
//  type 14 INSTANCE_GROUP / default: no-op.

// ---- renderer draw primitives (BSGraphics::Renderer, all use ctx @*(0x143027EA0), flush state via SetDirtyStates 0x140D705B0(0) first) ----
// TriShape rendererData: {+0 ID3D11Buffer* VB, +8 ID3D11Buffer* IB, +0x10 uint64 vertexDesc}; DynamicTriShape adds {+0x18 uint32 dynOffset, +0x1C uint32 dynSize}.
// static stride = (desc<<2)&0x3C ; dynamic stride = (desc>>2)&0x3C.
// 0x140D6BFE0 DrawTriShape(rd, start, triCnt): desc→m_VertexDesc(0x1430281F0, dirty 0x400); topo=4(TRIANGLELIST, 0x143028208, dirty 0x800); SetDirtyStates(0); IASetIndexBuffer(rd->IB, R16_UINT(57), 0); IASetVertexBuffers(0,1,&rd->VB,{staticStride},{0}); DrawIndexed(3*triCnt, start, 0).
// 0x140D6C0E0 DrawTriShapeAltIB: same but IASetIndexBuffer(*altIB, 57, 0).
// 0x140D6CAB0 DrawDynamicTriShape(rd fields, start, triCnt, dynOffset): desc/topo dirty; IASetIndexBuffer(rd->IB,57,0); IASetVertexBuffers(0,2,{rd->VB, dynVB[cur]},{staticStride,dynStride},{0,dynOffset}); DrawIndexed(3*triCnt, start, 0).
// 0x140D6C8A0 AllocateAndMapDynamicVertexBuffer(size,&outOff): ring of 3 4MB buffers @0x143025F18[0..2], cur idx @0x143025F30, used @0x143025F34; on overflow: ctx->End(query[idx] @0x143026168+8i), clear ready byte @0x143026164+i, idx=(idx+1)%3, off=0; if !ready: loop ctx->GetData(query,&d,4,flags) + Sleep(1); Map(dynVB[idx],0,WRITE_NO_OVERWRITE(5),0,&m); return m+off.
// 0x140D6C9E0 / 0x140D6CA30 Unmap(dynVB[cur],0).
// 0x140D6C1E0 DrawInstancedTriShape(rd, startIdx, triCnt, instCnt, desc, &instVB): desc/topo=4; IASetIndexBuffer(rd->IB,57,0); IASetVertexBuffers(0,2,{rd->VB, instVB},{staticStride,dynStride},{0,0}); DrawIndexedInstanced(3*triCnt, instCnt, startIdx, 0, 0).
// 0x140D6FFD0 GetAndMapConstantBuffer(group): group 7 → CB *(0x143027E90); else round-robin 4 CBs @0x1430279E8+8i (idx @0x1430279E0); Map(cb,0,WRITE_DISCARD(4),0,&p); caches cb @0x14302AC58.
// 0x140D6CBE0 DrawParticleShaderTriShape(renderer@rcx, data@rdx, vtxCnt@r8d): stride 48; alloc+fill dyn VB (packer 0x140D74A60); Unmap; topo=4; clear desc-dirty (&~0x400); SetDirtyStates; lazy CreateInputLayout(device *(0x143025F08) vtbl idx11) {POSITION,NORMAL,TEXCOORD,TEXCOORD1} vs current VS bytecode (*(0x1430281F8)+104,len@+16) → cached @0x143025F48; IASetInputLayout; dirty|=0x400; IASetIndexBuffer(*(0x143025F38),57,0); IASetVertexBuffers(0,1,&dynVB[cur],{48},{allocOff}); DrawIndexed(6*(vtxCnt>>2), 0, 0).
// 0x140D6D310 DrawLineShape(rd,cnt): topo=2(LINELIST); IB/VB as TriShape; DrawIndexed(2*cnt, 0, 0).
// 0x140D6D620 DrawDynamicLines(rd, start, cnt): topo=2; two streams like dynamic trishape (offset from rd+0x18); DrawIndexed(2*cnt, start, 0).
// 0x140D6CE60 DrawStripParticles: verts(40B)+indices into dyn ring, topo=5(TRIANGLESTRIP), IASetIndexBuffer(dynVB[cur],57,idxAllocOff), layout POSITION/TEXCOORD/NORMAL/COLOR @0x143025F50, DrawIndexed(idxCnt,0,0).

CONDITIONS FOR SetupTechnique / SetupMaterial (exact):
- SetupTechnique (via BeginPass) is skipped iff: g_LastTechnique == Technique AND Technique != 0x5C006076 AND Pass->m_Shader == g_LastShader. Any EndPass (end of a pass-group slot, end of persistent list, technique change, accumulator RenderBatches end) zeroes the Last* trio, forcing re-setup.
- RestoreTechnique is called (vtbl+0x18, args (shader, lastTechnique)) whenever LastShader non-null at an EndPass point.
- SetupMaterial fires iff (property ? property->material(+0x78) : NULL) != g_LastMaterial AND the new material is non-null; g_LastMaterial is ALWAYS updated (even to NULL). g_LastMaterial is zeroed by every BeginPass/EndPass.
- ucCurrentMeshLODLevel: geometry+0x108 = full m_LODMode byte written on EVERY RenderPassImmediately that passes the technique gate, before geometry dispatch.

RenderFlags bits used in this cluster:
- 0x4  : enable SetupGeometryAlphaBlending in ShaderSetup (unless grass-shadow-blacklist technique 0x5C000058..5B) [matches Nukem]
- 0x108: "don't touch cull mode" — suppresses all cull-mode writes in RenderBatches/VFunc03/RenderPersistentPassList state setup and epilogue restores [matches Nukem's `unknownFlag`]
- passed through opaquely to SetupGeometry/RestoreGeometry (BSUtilityShader::SetupGeometry 0x14130EC70 consumes further bits — other cluster).

### Every D3D11 call (arg -> data source)
- ctx->DrawIndexed(3*triCount, startIndex, 0) <- ctx=*(0x143027EA0); triCount: BSTriShape word@geom+0x158 (type3/7/8-full/9-full), per-segment dword@seg+12 (type8/9 segmented), 0x141330240(geom,lodByte@geom+0x108) (type5), 0x1413308F0(geom,lod,hint12?1:0) (type6); startIndex: 0, seg dword@seg+0, 0x1413300C0(geom) (type5), 0x141330850(geom,hint12?1:0) (type6)  [DrawTriShape 0x140D6BFE0 / AltIB 0x140D6C0E0]
- ctx->DrawIndexed(3*triCount, startIndex, 0) <- dynamic trishapes [0x140D6CAB0]: triCount = 2*min(activeVertexCount,2048) (particles type1) or word@dynShape+0x158 (type4); startIndex=0
- ctx->DrawIndexed(6*(vertexCount>>2), 0, 0) <- vertexCount = word@dynShape+0x15A (type11 particle-shader) [0x140D6CBE0]
- ctx->DrawIndexed(2*count, startIndex, 0) <- count = word@geom+0x158; lines type12 (start=0) [0x140D6D310] / dynamic lines type13 (start=0 from Draw) [0x140D6D620]
- ctx->DrawIndexed(stripIndexCount, 0, 0) <- stripIndexCount produced by BSStripParticleSystem builder 0x140D76D80 (type2) [0x140D6CE60]
- ctx->DrawIndexedInstanced(3*triCount, instanceCount, startIndexLocation, 0, 0) <- grass type10 [0x140D6C1E0]: triCount=word@geom+0x158, instanceCount=dword@instGroupEntry+0x4C, startIndexLocation=0 (from Draw)
- ctx->IASetIndexBuffer(buffer, DXGI_FORMAT_R16_UINT(57), 0) <- buffer = rendererData+0x08 (normal), *(geom+0x160) first qword (AccumulationHint==12 alt depth IB, types 6/7), shared quad IB *(0x143025F38) (particle-shader), dynVB[curIdx] with offset=ringAllocOffset (strip particles ONLY case with nonzero offset)
- ctx->IASetVertexBuffers(0, 1, &rendererData->VB(+0), stride=(vertexDesc<<2)&0x3C, offset=0) <- static single-stream draws (types 3,5,6,7,8,9,12); vertexDesc = rendererData+0x10
- ctx->IASetVertexBuffers(0, 2, {rendererData->VB, dynVB[*(int*)0x143025F30]}, {(desc<<2)&0x3C, (desc>>2)&0x3C}, {0, dynOffset}) <- dynOffset = rendererData+0x18 (written by map call); types 1,4 (dynamic trishape) and 13 (dynamic lines)
- ctx->IASetVertexBuffers(0, 2, {rendererData->VB, instanceVB}, {(desc<<2)&0x3C, (desc>>2)&0x3C}, {0, 0}) <- instanceVB = qword@instGroupEntry+0x40, desc = qword@geom+0x148 (grass type10)
- ctx->IASetVertexBuffers(0, 1, &dynVB[cur], stride=48 (type11) / 40 (type2), offset=ringAllocOffset) <- CPU-packed particle vertex data
- ctx->IASetInputLayout(layout) <- ONLY types 11/2: lazily created layouts cached at *(0x143025F48)/*(0x143025F50); afterwards dirty|=0x400 so next draw rebinds the normal layout. All other draws bind layout inside SetDirtyStates 0x140D705B0 from (m_VertexDesc & currentVS->inputLayoutMask@VS+0x48)
- device->CreateInputLayout(desc[4], 4, currentVS->bytecode(+104), currentVS->bytecodeLen(+16), &cachedLayout) <- device=*(0x143025F08); type11: {POSITION,NORMAL,TEXCOORD,TEXCOORD1}; type2: {POSITION,TEXCOORD,NORMAL,COLOR}; currentVS=*(0x1430281F8)
- ctx->Map(dynVB[idx], 0, D3D11_MAP_WRITE_NO_OVERWRITE(5), 0, &mapped) <- dynamic VB ring alloc 0x140D6C8A0; idx = *(0x143025F30); 3 buffers of 0x400000 bytes @0x143025F18[0..2]; used-offset @0x143025F34
- ctx->Unmap(dynVB[cur], 0) <- 0x140D6C9E0 / 0x140D6CA30 after CPU copy (memcpy_s 0x14130A030 of BSDynamicTriShape dynamic data, size dword@dyn+0x170 or rendererData+0x1C)
- ctx->End(query[idx]) + loop{ ctx->GetData(query[idx], &result, 4, flags); Sleep(1) } <- ring-wrap GPU sync in 0x140D6C8A0; queries @0x143026168+8*idx, ready flags bytes @0x143026164+idx
- ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD(4), 0, &mapped); *(uint32*)mapped = instanceGroupIndex; ctx->Unmap(cb, 0); ctx->VSSetConstantBuffers(7, 1, &cb) <- grass type10 per instance group; cb = *(0x143027E90) (GetAndMapConstantBuffer 0x140D6FFD0 group 7)
- INDIRECT (flushed by SetDirtyStates 0x140D705B0(0) before every draw; values written by this cluster): RSSetState from m_RasterStateCullMode dword@0x143027F4C (dirty 0x20; 1=back-cull, 0=none, set per pass-group slot / two-sided property bit36); OMSetBlendState from m_AlphaBlendMode@0x143027F58 + m_AlphaBlendAlphaToCoverage@0x143027F5C + m_AlphaBlendWriteMode@0x143027F60 (dirty 0x80; A2C=1 for alpha-tested slots 1/3 when *(uint8*)0x1431D0E5D, writeMode from per-light-type tables 0x141861380/0x1416C04A8 in shadow-mask emitters); alpha-test-ref CB from m_AlphaTestEnabled dword@0x143027F64 (dirty 0x100, set for slots 1/3/4, and by BSShader::SetupAlphaTestRef 0x14131F2A0); PSSetShaderResources from m_PSTexture[16]@0x143027FF0 + m_PSResourceModifiedBits@0x143027EB4 (SUBINDEX_LAND type9 writes slots i and i+7 with default-land SRV from *(0x14302C8E8)); IASetPrimitiveTopology from m_Topology@0x143028208 (dirty 0x800; 4=TRIANGLELIST, 2=LINELIST, 5=TRIANGLESTRIP)

### Divergences from Nukem's 1.5.23 RE
- IDA autonames swapped vs reality: 0x1413088C0 (named _Skinned) is RenderPassImmediately_Standard (ORs bUseEarlyZ @0x14302C8E5); 0x141308970 (named _Standard) is _Skinned. Dispatch verified in disasm: skin@geom+0x130 != NULL -> 0x141308970.
- Persistent pass lists (RenderPersistentPassList 0x1413090C0 AND the grouped-alpha walker 0x1413094A0) advance via BSRenderPass+0x30 (m_PassGroupNext), verified 'mov rbx,[rbx+30h]'. Nukem's BSBatchRenderer.cpp uses m_Next (+0x28) for RenderPersistentPassList — in 1.5.97 +0x28 is not read by any walker in this cluster.
- Nukem marks GeometryGroup::Render's VFunc03 branch 'never called / Assert(false)'; in 1.5.97 the path is fully implemented: 0x141306240 vcalls BSBatchRenderer vtbl slot 3 = 0x141307930, a complete non-destructive walk of m_ActivePassIndexList with per-slot state setup AND its own grass-shadow-blacklist group-discard (checks current accumulator @0x1431D0E20 bytes +0x128/+0x129) that Nukem's transcription lacks.
- Nukem's Draw() is his reimplementation with asserts; 1.5.97 vanilla Draw (0x141307160) fully implements MESHLOD (type5: LOD start/count helpers 0x1413300C0/0x141330240), LOD_MULTIINDEX (6) and MULTIINDEX (7) with an AccumulationHint==12 alternate-index-buffer path (geom+0x160), SUBINDEX (8) and SUBINDEX_LAND (9) per-segment draws with direct PS-texture cache writes, grass instanced draws (10: DrawIndexedInstanced + VS cb7 instance index), strip particles (2) and dynamic lines (13).
- Pass->m_AccumulationHint == 12 (byte compare at Pass+0x1C, verified 'cmp byte ptr [rcx+1Ch],0Ch') selects the alternate ('depth-only'?) index buffer for geometry types 6/7 — absent from Nukem's sources.
- BeginPass register order is (Technique@ecx, Shader@rdx), reverse of Nukem's C++ signature (cosmetic).
- 1.5.23 globals relocated in 1.5.97: LastTechnique/LastShader 0x1432A8214/18 -> 0x143283BA4/A8; LastMaterial 0x1434B5220 -> 0x143490BB0; write-only technique mirror dword_141E32FDC -> 0x141E0DF8C; alpha-to-coverage-enabled byte_1431F54CD -> 0x1431D0E5D; node pool 0x1434B5230 -> 0x143490BC0. These are NOT at a constant offset from one renderer base — Nukem's '+0x3014/+0x3018/+0x3500' offsets do not hold as a single-block layout in 1.5.97; treat as independent globals.
- RendererShadowState matches Nukem's struct field-for-field at base 0x143027EB0 (cullMode +0x9C, A2C +0xAC, writeMode +0xB0, alphaTestEnabled +0xB4, m_PSTexture +0x140, vertexDesc +0x340, topology +0x358); all DIRTY_ bits identical (0x20/0x80/0x100/0x400/0x800).
- Nukem's Draw type-1 particles called DrawDynamicTriShapeUnknown(shape,drawData,0,cnt*2) — 1.5.97 identical semantics but the alloc offset is stored in the DynamicTriShape struct (+0x18), and the drawData out-param of the Map wrapper (0x140D6CA10 arg4) is dead; count arg passes on the stack (decompiler drops it — disasm required).
- BSShaderAccumulator::RenderBatches grass-discard condition confirmed identical (m_1stPerson@+0x128 || byte@+0x129), and IsGrassShadowBlacklist compiled to a range check (t-0x5C000058)<=3 covering exactly Nukem's 4 enum values.
- Skinned decal branch: partition ctor 0x140C7BAD0 runs AFTER ShaderSetup, m_usBones at partition+0x3C=1, SetBoneMatrix vcall is on the NiBoneMatrixSetterI subobject at shader+0x10 slot 1, world transform at geom+0x7C — all consistent with Nukem's transcription.
- EndPass exists standalone at 0x141308760 (also zeroes LastMaterial, like Nukem's) but is INLINED at every use inside RenderBatches/VFunc03/RenderPersistentPassList/BeginPass; the '+0x3010' dword Nukem commented in EndPass is the grouped-alpha stack counter 0x143283BA0 in 1.5.97 and is NOT zeroed by EndPass — only by 0x141308540.

### Open questions
- Semantic of TLS dword +0x2A00 zeroed by 0x14131F7C0 (per-thread block via TLS index *(0x143497408)) before every skinned render (1.5.23 analog sub_141336450) — likely a bone-matrix/setter cache reset; unverified.
- Geometry vfunc +0x1B0 (single-bone decal branch selector) — Nukem's 'IsBSSkinnedDecalTriShape?' guess adopted, not proven.
- Byte at BSShaderAccumulator+0x129 (paired with m_1stPerson@+0x128 to discard grass-shadow techniques) — meaning unknown (Nukem also had it as raw offset 297).
- AccumulationHint==12 interpreted as 'depth/shadow render mode hint' selecting the alternate index buffer at geom+0x160 for types 6/7 — name/semantics inferred from ShadowMapOrMask render modes 12..17, not proven.
- 0x143283B78: qword copied from (light+0x140-ish struct)+0x58 before each shadow-mask pass in 0x1412E3B80/0x1408D2D70 — presumably consumed by BSUtilityShader::SetupGeometry (other cluster); exact field unresolved.
- Strip-particles (type 2) builder 0x140D76D80 output contract and the __usercall rbx argument to 0x140D6CE60 are partially decompiled; low relevance to utility passes but not fully pinned.
- SUBINDEX_LAND (type 9) texture loop: slots i (0..5) and i+7 both set to the default-land SRV chain from *(0x14302C8E8); which material slots these correspond to (diffuse/normal banks?) unverified; gates *(0x1430243B0) (type 8 full-draw) and *(0x1434963C8) (type 9 single-draw) unidentified settings.
- Alpha-group walker skip condition (dword 0x1431D0E28==18 && property->vfunc(+0x10)() == 0x1431D1F00) — looks like current-render-mode==18 && property RTTI equals a specific class; classes unresolved.
- FUN_1412CD2C0 (TILE technique 0x5C006075 loop) shows MakeRenderPass shader arg as 0 in decompile — likely a dropped register; unverified.
- 0x5C006076 in the BeginPass fast-path exclusion is one past BSSM_TILE; the technique it denotes in 1.5.x is unidentified (Nukem left it literal too).
- Whether rcx=0x143028490 passed to all renderer draw fns is Renderer::QInstance or an interior pointer — irrelevant for replication (all functions address globals absolutely) but the exact Renderer struct nesting around device @0x143025F08 / dyn-VB ring @0x143025F18 / context @0x143027EA0 was not mapped.
- 0x1412CCAD0 (technique 2 = BSSM_AMBIENT_OCCLUSION with SkyShader on a captured trishape, RT push/pop via 0x140D70150/190/1B0) — purpose (sun occlusion?) not confirmed.

### Key addresses
- 0x141308030 BSBatchRenderer::RenderBatches (per-technique group render; state setters + EndPass inlined; tail-calls advance)
- 0x141308440 BSBatchRenderer::RenderPassImmediately (technique gate, SetupMaterial, LOD byte write, geometry dispatch)
- 0x1413083B0 BSBatchRenderer::DiscardBatches (zero PassGroup if AutoClear, advance)
- 0x141307DD0 BSBatchRenderer::GetFirstOrNextTechnique (Nukem sub_14131E700)
- 0x141307E80 BSBatchRenderer::AdvanceInActivePassList (Nukem sub_14131E7B0; pops+frees list nodes when AutoClear)
- 0x141307FC0 BSBatchRenderer::FindNextValidGroupSlot (Nukem sub_14131E8F0)
- 0x141307930 BSBatchRenderer::VFunc03 (vtbl slot 3; full active-list walk, technique range [1,0x5C006074] default)
- 0x1413086C0 BSBatchRenderer::BeginPass(Technique@ecx, Shader@rdx)
- 0x141308760 BSBatchRenderer::EndPass (standalone)
- 0x141308520 ClearShaderAndTechnique without RestoreTechnique (state reset helper)
- 0x141308540 zero grouped-alpha stack count @0x143283BA0
- 0x1413088C0 RenderPassImmediately_Standard (IDA-named _Skinned; ORs bUseEarlyZ)
- 0x141308970 RenderPassImmediately_Skinned (IDA-named _Standard; decal single-bone + SkinInstance::Render paths)
- 0x141308B20 RenderPassImmediately_Custom (SetupGeometry first, forced alpha blending)
- 0x141309F80 BSBatchRenderer::ShaderSetup (grass blacklist + alpha blend/test + SetupGeometry vcall)
- 0x141307160 BSBatchRenderer::Draw (geometry-type jumptable, QType@geom+0x150)
- 0x1412FD8A0 BSRenderPass::QAlphaProperty = *(geom+0x120)
- 0x1412CCE20 BSShaderAccumulator::IsGrassShadowBlacklist ((t-0x5C000058)<=3)
- 0x1412CCE40 BSShaderAccumulator::RenderBatches(StartTech, EndTech, RenderFlags, GroupIdx) — the walk driver
- 0x1412966B0 SetCurrentAccumulator / 0x1412966A0 GetCurrentAccumulator (global @0x1431D0E20)
- 0x1413061B0 GeometryGroup::ClearAndFreePasses
- 0x141306DB0 BSBatchRenderer::ClearRenderPasses
- 0x141306240 GeometryGroup::Render (accumulator wrapper: flags&1 -> persistent list, else VFunc03)
- 0x1413090C0 BSBatchRenderer::RenderPersistentPassList (walks m_PassGroupNext +0x30!)
- 0x1413094A0 grouped-alpha depth-sorted walker (from BSShaderAccumulator::RenderEffects 0x1412CB930)
- 0x141308FE0 pass-index-node free (locked freelist push into pool @0x143490BC0)
- 0x14131F7C0 per-thread pre-skin reset (TLS+0x2A00=0)
- 0x14131F440 BSShader::SetupGeometryAlphaBlending (non-virtual)
- 0x14131F2A0 BSShader::SetupAlphaTestRef (non-virtual)
- 0x1412FDCC0 BSShader::MakeRenderPass / 0x1412FDD60 ClearRenderPass / 0x1412FDDD0 pass-pool mutex getter
- 0x1412E3B80 shadow-light mask pass emitter (game; techniques 43+lightBits, BSUtilityShader)
- 0x1408D2D70 shadow-light mask pass emitter (menu scene)
- 0x1412CCAD0 sky-occlusion single-pass emitter (technique 2, SkyShader)
- 0x1412CD2C0 TILE(0x5C006075) pass loop / 0x1412CD420 technique-4653 utility pass on main DS
- 0x14130DCE0 BSUtilityShader::Ctor (writes pInstance)
- 0x14130EC70 BSUtilityShader::SetupGeometry (vtbl+0x30 target for utility passes — other cluster)
- 0x140D705B0 BSGraphics::SetDirtyStates (state flush — other cluster)
- 0x140D6BFE0 Renderer::DrawTriShape / 0x140D6C0E0 DrawTriShapeAltIndexBuffer
- 0x140D6CA60 DrawDynamicTriShape wrapper -> 0x140D6CAB0 (two-stream dynamic draw)
- 0x140D6CA10 MapDynamicTriShapeDynamicData (offset->shape+0x18, size from shape+0x1C if 0)
- 0x140D6C8A0 AllocateAndMapDynamicVertexBuffer (3x4MB ring, query-synced, MAP_NO_OVERWRITE)
- 0x140D6C9E0 / 0x140D6CA30 UnmapDynamicVertexBuffer
- 0x140D6C7E0 GetParticlesDynamicTriShape (magic-static @0x14302AE50, desc 0x0840200004000051)
- 0x140D6C1E0 DrawInstancedTriShape (grass; DrawIndexedInstanced)
- 0x140D6FFD0 GetAndMapConstantBuffer (group 7 = grass instance CB @*(0x143027E90); ring @0x1430279E8, idx @0x1430279E0)
- 0x140D6CBE0 DrawParticleShaderTriShape (stride 48, custom input layout @*(0x143025F48))
- 0x140D6CE60 DrawStripParticles (stride 40, layout @*(0x143025F50), strip topology)
- 0x140D6D310 DrawLineShape / 0x140D6D5D0 MapDynamicLines / 0x140D6D5F0 UnmapDynamicLines / 0x140D6D620 DrawDynamicLines
- 0x14130A030 memcpy_s clone
- 0x140D76080 PackParticleData (CPU quad expansion) / 0x140D76D80 BSStripParticleSystem builder
- 0x1413300C0 / 0x141330240 MeshLOD start-index / tri-count; 0x141330850 / 0x1413308F0 LOD-multiindex start / count; 0x141334070 subindex-land segment flags; 0x140D59430 BSSubIndexTriShape update
- 0x140C723C0 BSDynamicTriShape::LockDynamicDataForRead / 0x140C72420 UnlockDynamicData / 0x140C723F0 lock (particle variant)
- 0x140C7BAD0 / 0x140C7BB10 NiSkinPartition::Partition ctor/dtor
- 0x140D74D10 RenderTargetManager::SetDepthStencilRenderTarget / 0x140D74CF0 SetRenderTarget / 0x140D74E50 GetDepthStencilTarget_MAIN (RTM @0x14302BB20)
- 0x1412BC7D0 ShadowSceneNode::GetShadowCasterLightArrayEntry
- GLOBAL 0x143283BA0 grouped-alpha GeometryGroup stack count
- GLOBAL 0x143283BA4 LastTechnique (uint32)
- GLOBAL 0x143283BA8 LastShader (BSShader*)
- GLOBAL 0x143490BB0 LastMaterial (BSShaderMaterial*)
- GLOBAL 0x143490BC0 pass-index BSSimpleList node freelist pool (head + mutex@+8)
- GLOBAL 0x143490BD0 grouped-alpha GeometryGroup stack array (stride 0x28, depth @+0x20)
- GLOBAL 0x141E0DF8C write-only current-technique mirror (1.5.23 dword_141E32FDC)
- GLOBAL 0x1431D0E20 current BSShaderAccumulator*
- GLOBAL 0x1431D0E28 current render mode(?) (==18 check in alpha walker)
- GLOBAL 0x1431D0E5D alpha-to-coverage enabled setting (1.5.23 byte_1431F54CD)
- GLOBAL 0x1432336C0 BSSkyShader::pInstance
- GLOBAL 0x143495D50 BSUtilityShader::pInstance
- GLOBAL 0x14302C8DC BSGraphics::gState.uiFrameCount
- GLOBAL 0x14302C8E5 BSGraphics::gState.bUseEarlyZ
- GLOBAL 0x14302C8E8 default land NiSourceTexture*
- GLOBAL 0x14302C890 BSGraphics::State (SetCameraData target)
- GLOBAL 0x143025F08 ID3D11Device*
- GLOBAL 0x143027EA0 ID3D11DeviceContext* (immediate; == renderer qword array 0x1430261B0[926])
- GLOBAL 0x143027EB0 RendererShadowState BASE: +0x00 m_StateUpdateFlags, +0x04(0x143027EB4) m_PSResourceModifiedBits, +0x9C(0x143027F4C) m_RasterStateCullMode, +0xA8(0x143027F58) m_AlphaBlendMode, +0xAC(0x143027F5C) m_AlphaBlendAlphaToCoverage, +0xB0(0x143027F60) m_AlphaBlendWriteMode, +0xB4(0x143027F64) m_AlphaTestEnabled, +0x140(0x143027FF0) m_PSTexture[16], +0x340(0x1430281F0) m_VertexDesc, +0x348(0x1430281F8) m_CurrentVertexShader, +0x358(0x143028208) m_Topology; DIRTY bits: 0x20 cull, 0x80 alpha-blend, 0x100 alpha-test-ref, 0x400 vertex-desc, 0x800 topology
- GLOBAL 0x143025F18 dynamic-VB ring: [0..2] ID3D11Buffer*, +0x18(0x143025F30) curIdx, +0x1C(0x143025F34) usedOffset, +0x20(0x143025F38) shared quad index buffer, +0x28(0x143025F40) shared particle VB, +0x30(0x143025F48) particle-shader input layout, +0x38(0x143025F50) strip input layout, queries @0x143026168+8i, ready flags @0x143026164+i
- GLOBAL 0x143027E90 grass instance-group VS constant buffer (slot 7)
- GLOBAL 0x1430279E0 temp-CB ring index / 0x1430279E8 temp CBs[4]
- GLOBAL 0x143028490 Renderer instance pointer passed as rcx to draw fns (unused internally)
- GLOBAL 0x14302AE50 shared particles DynamicTriShape (magic-static)
- GLOBAL 0x14302AC58 last-mapped-CB holder
- GLOBAL 0x1430243B0 subindex full-draw gate / 0x1434963C8 land single-draw gate
- GLOBAL 0x143283B78 shadow-mask per-light data staged before RenderPassImmediately (consumed downstream)
- GLOBAL 0x143497408 module TLS index (TLS+0x768 memory context, TLS+0x2A00 pre-skin reset dword)
- GLOBAL 0x141861380 / 0x1416C04A8 per-light-type alpha-blend write-mode tables (game/menu shadow-mask emitters)
- GLOBAL 0x1418685B0 BSUtilityShader vtable
- STRUCT BSRenderPass: +0x00 m_Shader, +0x08 m_ShaderProperty, +0x10 m_Geometry, +0x18 m_PassEnum, +0x1C m_AccumulationHint(byte; ==12 selects alt IB), +0x1D m_ExtraParam, +0x1E m_LODMode(bit7 SingleLevel), +0x1F m_NumLights, +0x28 m_Next(unused by these walkers), +0x30 m_PassGroupNext(ALL chain walks), +0x38 m_SceneLights
- STRUCT BSGeometry: +0x7C world NiTransform, +0xE4 worldBound, +0x108 ucCurrentMeshLODLevel, +0x109 flags(bit3=custom render), +0x120 properties[2] (0=alpha,1=shader), +0x130 NiSkinInstance*, +0x138 rendererData, +0x148 vertexDesc, +0x150 QType, +0x158 triangle/vertex count(word), +0x160 alt-IB/segment/instance array, +0x170 dynamicDataSize(on BSDynamicTriShape), +0x174 uiFrameCount, +0x178 DrawData, +0x180 instance-group count, +0x181 draw-as-one flag

================================================================================================
## Cluster 5
================================================================================================

# SkyrimSE 1.5.97 — BSBatchRenderer geometry-Draw cluster (RenderPassImmediately → DrawIndexed)

## 0. Corrected function map (IDB names at several of these addresses are WRONG/swapped)

| 1.5.97 addr | True identity | Notes |
|---|---|---|
| 0x141308440 | `BSBatchRenderer::RenderPassImmediately(Pass, Technique, AlphaTest, RenderFlags)` | anchor confirmed |
| 0x1413086C0 | `BSBatchRenderer::BeginPass(Technique, Shader)` (arg order: ecx=technique, rdx=shader) | inlines EndPass semantics |
| 0x1413088C0 | `RenderPassImmediately_Standard` | **IDB names it "_Skinned" — wrong** |
| 0x141308970 | `RenderPassImmediately_Skinned` | **IDB names it "_Standard" — wrong** |
| 0x141308B20 | `RenderPassImmediately_Custom` | |
| 0x141309F80 | `BSBatchRenderer::ShaderSetup(Pass, Shader, AlphaTest, RenderFlags)` | **0x1413083B0 is NOT ShaderSetup** — it is a DiscardBatches-style helper (`m_RenderPassMap.get` + `PassGroup::Clear` + advance) |
| 0x141307160 | **`BSBatchRenderer::Draw(Pass)` — THE geometry-type switch** (13 cases) | 1.5.97 merges Nukem's `Draw` and his `sub_14131DDF0`; this IS the 1.5.97 equivalent of 1.5.23 0x14131DDF0 |
| 0x141308520 | `ClearShaderAndTechnique()` — zeroes lastTechnique/lastShader/lastMaterial | **NOT a _Standard entry point** (prompt anchor wrong) |
| 0x141308540 | zeroes u32 @0x143283BA0 only | NOT _Skinned |

## 1. RenderPassImmediately (0x141308440) — cleaned pseudocode

```cpp
void BSBatchRenderer::RenderPassImmediately(BSRenderPass* Pass, uint32_t Technique,
                                            bool AlphaTest, uint32_t RenderFlags)
{
    BSShader*   shader = Pass->m_Shader;        // +0x00
    BSGeometry* geom   = Pass->m_Geometry;      // +0x10

    bool setup = (g_CurrentTechnique == Technique            // u32 @0x143283BA4
               && Technique != 0x5C006076                    // BSSM_BLOOD_SPLATTER special-case
               && shader == g_CurrentShader);                // BSShader* @0x143283BA8
    if (!setup) {
        g_DebugTechnique = Technique;                        // u32 @0x141E0DF8C (write-only)
        setup = BeginPass(Technique, shader);
    }
    if (!setup) return;

    BSShaderMaterial* mat = Pass->m_ShaderProperty ? Pass->m_ShaderProperty->material /*+0x78*/ : nullptr;
    if (mat != g_CurrentMaterial) {                          // @0x143490BB0
        if (mat) shader->SetupMaterial(mat);                 // vtbl[4] (+0x20)
        g_CurrentMaterial = mat;
    }

    // copies WHOLE LODMode byte (incl. bit7 single-level flag)
    *(uint8_t*)((uintptr_t)geom + 0x108) = *(uint8_t*)&Pass->m_LODMode;   // Pass+0x1E

    if (*(void**)((uintptr_t)geom + 0x130))                  // skinInstance
        RenderPassImmediately_Skinned(Pass, AlphaTest, RenderFlags);      // 0x141308970
    else if (*(uint8_t*)((uintptr_t)geom + 0x109) & 8)       // "custom render" flag
        RenderPassImmediately_Custom(Pass, AlphaTest, RenderFlags);       // 0x141308B20
    else
        RenderPassImmediately_Standard(Pass, AlphaTest, RenderFlags);     // 0x1413088C0
}
```

### BeginPass (0x1413086C0)
```cpp
bool BeginPass(uint32_t Technique, BSShader* Shader)
{
    if (g_CurrentShader) g_CurrentShader->RestoreTechnique(g_CurrentTechnique); // vtbl[3]
    g_CurrentShader = nullptr; g_CurrentTechnique = 0;
    g_CurrentMaterial = nullptr;                              // 0x143490BB0
    if (Shader->SetupTechnique(Technique)) {                  // vtbl[2] (+0x10)
        g_CurrentShader = Shader; g_CurrentTechnique = Technique; return true;
    }
    g_CurrentShader = nullptr; g_CurrentTechnique = 0; return false;
}
```

### ShaderSetup (0x141309F80)
```cpp
void ShaderSetup(BSRenderPass* Pass, BSShader* Shader, bool AlphaTest, uint32_t RenderFlags)
{
    if (Shader != BSSkyShader::pInstance) {                   // @0x1432336C0
        if ((RenderFlags & 4) && !IsGrassShadowBlacklist(Pass->m_PassEnum))  // 0x1412CCE20: (t-0x5C000058)<=3
            SetupGeometryAlphaBlending_14131F440(Shader, Pass->QAlphaProperty(), Pass->m_ShaderProperty, AlphaTest);
        if (AlphaTest && Pass->QAlphaProperty())
            SetupAlphaTestRef_14131F2A0(Shader, Pass->QAlphaProperty(), Pass->m_ShaderProperty);
    }
    Shader->SetupGeometry(Pass, RenderFlags);                 // vtbl[6] (+0x30) = 0x14130EC70 for BSUtilityShader
}
```
`GetNiProperty` = 0x1412FD8A0 (reads Pass property list slot 0 = NiAlphaProperty).

### _Standard (0x1413088C0)
```cpp
void RenderPassImmediately_Standard(BSRenderPass* Pass, bool AlphaTest, uint32_t RenderFlags)
{
    // MemoryContextTracker: TLS[+0x768] = 26 (RENDER_ACCUMULATOR), restored at exit
    ShaderSetup(Pass, Pass->m_Shader, AlphaTest || gState.bUseEarlyZ /*byte @0x14302C8E5*/, RenderFlags);
    Draw(Pass);                                               // 0x141307160
    Pass->m_Shader->RestoreGeometry(Pass, RenderFlags);       // vtbl[7] (+0x38) = 0x141310300 for utility
}
```

### _Custom (0x141308B20)
```cpp
void RenderPassImmediately_Custom(BSRenderPass* Pass, bool /*AlphaTest unused*/, uint32_t RenderFlags)
{
    auto s = Pass->m_Shader;
    s->SetupGeometry(Pass, RenderFlags);                      // NOTE: no early-Z or blacklist gates
    SetupGeometryAlphaBlending_14131F440(s, Pass->QAlphaProperty(), Pass->m_ShaderProperty, true);
    if (Pass->QAlphaProperty())
        SetupAlphaTestRef_14131F2A0(s, Pass->QAlphaProperty(), Pass->m_ShaderProperty);
    Draw(Pass);
    s->RestoreGeometry(Pass, RenderFlags);
}
```

### _Skinned (0x141308970)
```cpp
void RenderPassImmediately_Skinned(BSRenderPass* Pass, bool AlphaTest, uint32_t RenderFlags)
{
    BSGeometry* geom = Pass->m_Geometry;
    BSShader*   shader = Pass->m_Shader;
    NiSkinInstance* skin = *(NiSkinInstance**)((uintptr_t)geom + 0x130);

    ClearBoneSetterTLSCache_14131F7C0();      // TLS[+0x2A00] = 0  (last-skin bone-CB cache)

    if (geom->vtbl[+0x1B0](geom)) {           // IsBSSkinnedDecalTriShape (vfunc at vtable+0x1B0/idx54)
        ShaderSetup(Pass, shader, AlphaTest, RenderFlags);
        NiSkinPartition::Partition part = {}; // zero-init 0x48 bytes (0x140C7BAD0), dtor 0x140C7BB10
        part.m_usBones /*+0x3C*/ = 1;
        // SetBoneMatrix via NiBoneMatrixSetterI vtable (shader+0x10), slot 1:
        ((NiBoneMatrixSetterI*)((uintptr_t)shader+0x10))->SetBoneMatrix( // 0x14131F630
            skin, &part, (NiTransform*)((uintptr_t)geom + 0x7C));
        Draw(Pass);
    } else {
        ShaderSetup(Pass, shader, AlphaTest, RenderFlags);
        struct SkinRenderData {               // on stack, 0x28 bytes
            NiBoneMatrixSetterI* setter;      // +0x00 = shader+0x10 (or null)
            BSGeometry* geometry;             // +0x08
            uint64_t    zero;                 // +0x10
            uint32_t    singleLevel;          // +0x18 = (PassByte0x1E >> 7) & 1
            uint32_t    lodIndex;             // +0x1C = PassByte0x1E & 0x7F
            uint32_t    zero2;                // +0x20
            uint32_t    vertexBufferOffset;   // +0x24 = 0xFFFFFFFF
        } srd;
        BSDynamicTriShape* dyn = geom->vtbl[+0x60](geom);     // AsBSDynamicTriShape (idx12)
        if (dyn) {
            uint32_t size = *(uint32_t*)((uintptr_t)dyn + 0x170);   // DynamicDataSize
            void* dst = AllocateAndMapDynamicVertexBuffer(size, &srd.vertexBufferOffset); // 0x140D6C8A0
            memcpy_s(dst, size, LockDynamicDataForRead(dyn) /*0x140C723F0*/, size);       // 0x14130A030
            UnlockDynamicData(dyn);           // 0x140C72420
            UnmapDynamicVertexBuffer();       // 0x140D6C9E0: Unmap(DynVB[cur], 0)
        }
        skin->vtbl[+0x128](skin, &srd);       // NiSkinInstance::Render (idx37) = 0x140C7E170
    }
    shader->RestoreGeometry(Pass, RenderFlags);
}
```

## 2. BSBatchRenderer::Draw (0x141307160) — the 13-case switch on `*(uint8_t*)(geom+0x150) - 1`

Geometry types byte @+0x150: 1=PARTICLES 2=STRIP_PARTICLES 3=TRISHAPE 4=DYNAMIC_TRISHAPE 5=MESHLOD_TRISHAPE 6=LOD_MULTIINDEX_TRISHAPE 7=MULTIINDEX_TRISHAPE 8=SUBINDEX_TRISHAPE 9=SUBINDEX_LAND_TRISHAPE 10=MULTISTREAMINSTANCE_TRISHAPE 11=PARTICLE_SHADER_DYNAMIC_TRISHAPE 12=LINES 13=DYNAMIC_LINES; 14+ / 0 → default: no-op (INSTANCE_GROUP is a no-op).

All leaves get `rcx = 0x143028490` (Renderer "this", = 0x143028470+0x20) but internally address globals absolutely.

```cpp
void BSBatchRenderer::Draw(BSRenderPass* Pass)
{
    BSGeometry* g = Pass->m_Geometry;
    switch (*(uint8_t*)((uintptr_t)g + 0x150)) {

    case 1: { // PARTICLES
        // count = NiParticlesData vfunc: (*(g+0x158))->vtbl[+0x130]() → active vertex count (u16)
        uint32_t count = min(activeCount, 0x800);             // clamp 2048
        uint32_t dynSize = ((uint32_t)g->vertexDesc /*+0x148*/ >> 2) & 0x3C;
        if (!count) return;
        DynamicTriShape* p = GetParticlesDynamicTriShape();   // 0x140D6C7E0 → static @0x14302AE50
        uint32_t drawDataLocal;
        void* dst = MapDynamicTriShapeDynamicData(nullptr, p, &drawDataLocal, 4*count*dynSize); // 0x140D6CA10
        if (dst) { PackParticleData_140D76080(count, g, dst); UnmapDynamicTriShapeDynamicData(); } // 0x140D6CA30
        DrawDynamicTriShapeUnknown(p, &drawDataLocal, /*startIndex*/0, /*triCount*/2*count);   // 0x140D6CA60
    } return;

    case 2: { // STRIP_PARTICLES (not expected in utility passes)
        uint32_t vertCount = 0, indexCount = 0;
        // packs 40-byte verts into static staging @0x143283BB0, u16 indices into @0x143477BB0
        BuildStripParticleGeometry_140D76D80(g, stagingVerts, stagingIdx, &vertCount, &indexCount);
        if (vertCount && indexCount)
            DrawStripParticles_140D6CE60(stagingVerts, vertCount, stagingIdx, indexCount);
    } return;

    case 3: // TRISHAPE  — the main utility-pass path
        DrawTriShape_140D6BFE0(*(TriShape**)((uintptr_t)g + 0x138), 0, *(uint16_t*)((uintptr_t)g + 0x158));
        return;

    case 4: { // DYNAMIC_TRISHAPE
        BSDynamicTriShape* d = g->vtbl[+0x60](g);
        DynamicTriShape* rd = *(DynamicTriShape**)((uintptr_t)d + 0x138);
        if (*(uint32_t*)((uintptr_t)d + 0x174) != gState.uiFrameCount /*@0x14302C8DC*/) {   // once per frame
            *(uint32_t*)((uintptr_t)d + 0x174) = gState.uiFrameCount;
            void* dst = MapDynamicTriShapeDynamicData(d, rd, (uint32_t*)((uintptr_t)d+0x178), 0); // size 0 → rd+0x1C
            uint32_t sz = *(uint32_t*)((uintptr_t)d + 0x170);
            memcpy_s(dst, sz, LockDynamicDataForRead(d), sz);
            UnlockDynamicData(d);
            UnmapDynamicTriShapeDynamicData();
        }
        DrawDynamicTriShapeUnknown(rd, (void*)((uintptr_t)d+0x178), 0, *(uint16_t*)((uintptr_t)d + 0x158));
    } return;

    case 5: { // MESHLOD_TRISHAPE  (implemented — Nukem asserted false)
        uint8_t lod = *(uint8_t*)((uintptr_t)g + 0x108);      // written by RenderPassImmediately
        uint32_t count = MeshLODTriCount_141330240(g, lod);   // table u32[] @g+0x160; bit7 ⇒ single level
        if (!count) return;
        uint32_t start = MeshLODStartIndex_1413300C0(g);      // 3*sum(prev levels) if bit7 else 0
        DrawTriShape_140D6BFE0(*(TriShape**)((uintptr_t)g+0x138), start, count);
    } return;

    case 6: { // LOD_MULTIINDEX_TRISHAPE  (implemented — Nukem asserted false)
        uint8_t lod = *(uint8_t*)((uintptr_t)g + 0x108);
        if (*(uint8_t*)((uintptr_t)Pass + 0x1C) == 12) {      // AccumulationHint byte == 12 → alternate index set
            uint32_t count = MultiIndexCount_1413308F0(g, lod, /*col*/1);   // pairs table @g+0x1D8
            uint32_t start = MultiIndexStart_141330850(g, /*col*/1);        // 3*sum, tables @+0x1D8/+0x1E0
            DrawTriShapeAltIndex_140D6C0E0(*(void**)((uintptr_t)g+0x138), start, count,
                                           *(void**)((uintptr_t)g+0x160)); // struct whose [0]=alt ID3D11Buffer* IB
        } else {
            uint32_t count = MultiIndexCount_1413308F0(g, lod, 0);
            uint32_t start = MultiIndexStart_141330850(g, 0);
            DrawTriShape_140D6BFE0(*(TriShape**)((uintptr_t)g+0x138), start, count);
        }
    } return;

    case 7: // MULTIINDEX_TRISHAPE (Winterhold)
        if (*(uint8_t*)((uintptr_t)Pass + 0x1C) == 12)
            DrawTriShapeAltIndex_140D6C0E0(rd_0x138, 0, *(uint32_t*)((uintptr_t)g+0x168), *(void**)((uintptr_t)g+0x160));
        else
            DrawTriShape_140D6BFE0(rd_0x138, 0, *(uint16_t*)((uintptr_t)g+0x158));
        return;

    case 8: { // SUBINDEX_TRISHAPE
        void* rd = *(void**)((uintptr_t)g + 0x138);
        ConsolidateSegments_140D59430(g);                     // merges adjacent enabled 20-byte segment records
        if (g_DrawWholeSubIndex /*byte @0x1430243B0*/) {
            DrawTriShape_140D6BFE0(rd, 0, *(uint16_t*)((uintptr_t)g+0x158)); return;
        }
        uint32_t n = *(uint8_t*)((uintptr_t)g+0x171) ? 1 : *(uint32_t*)((uintptr_t)g+0x168);
        if (!n) return;
        uint8_t* seg = *(uint8_t**)((uintptr_t)g + 0x160);    // stride 0x14: +0 startIndex,+4 primCount,+8 enabled,+C mergedTriCount,+10 visible
        for (uint32_t i = 0; i < n; i++, seg += 0x14) {
            if (!seg[0x10]) continue;
            bool whole = *(uint8_t*)((uintptr_t)g+0x171);
            uint32_t count = whole ? *(uint16_t*)((uintptr_t)g+0x158) : *(uint32_t*)(seg+0xC);
            uint32_t start = whole ? 0 : *(uint32_t*)(seg+0);
            DrawTriShape_140D6BFE0(rd, start, count);
        }
    } return;

    case 9: { // SUBINDEX_LAND_TRISHAPE  (implemented — Nukem asserted false)
        void* rd = *(void**)((uintptr_t)g + 0x138);
        if (g_DrawWholeSubIndexLand /*byte @0x1434963C8*/) {  // single whole/first-segment draw
            /* same whole-flag logic as case 8, single DrawTriShape */
        } else {
            // iterate segments DESCENDING (i = n-1 … 1); per segment:
            //   mask = *(uint32_t*)(*(g+0x178) + 4*i)  (0x141334070)
            //   while slot s from 5 down where (mask & (1<<s))==0:
            //     def = *( *(g_DefaultLandTextureHolder /*@0x14302C8E8*/ + 0x48) + 0x10 );
            //     if (m_PSTexture[s]   != def) { m_PSTexture[s]   = def; m_PSResourceModifiedBits |= 1<<s;     } // @0x143027FF0+8s / @0x143027EB4
            //     if (m_PSTexture[s+7] != def) { m_PSTexture[s+7] = def; m_PSResourceModifiedBits |= 1<<(s+7); }
            //   then if segment visible (+0x10): DrawTriShape(rd, start(+0) or 0, count(+0xC) or whole)
        }
    } return;

    case 10: { // MULTISTREAMINSTANCE_TRISHAPE (grass!) — DrawIndexedInstanced
        uint32_t nGroups = *(uint32_t*)((uintptr_t)g + 0x180);
        uint16_t triCount = *(uint16_t*)((uintptr_t)g + 0x158);
        for (uint32_t i = 0; i < nGroups; i++) {
            void* grp = ((void**)*(void**)((uintptr_t)g + 0x160))[i];
            if (!grp || !*(uint32_t*)((uintptr_t)grp + 0x50)) continue;
            uint32_t* mapped; 
            ID3D11Buffer** cb = GetDynamicConstantBuffer_140D6FFD0(1, &mapped, /*slotSel*/7);
                 // slotSel==7 → dedicated CB @0x143027E90, Map(...,D3D11_MAP_WRITE_DISCARD=4,...)
            *mapped = i;                                       // instance-group index into cb7[0]
            if (*cb) ctx->Unmap(*cb, 0);
            ctx->VSSetConstantBuffers(7, 1, cb);
            DrawMultiStreamInstanced_140D6C1E0(rd_0x138, /*startIndex*/0, triCount,
                 /*instCount*/ *(uint32_t*)((uintptr_t)grp + 0x4C),
                 /*desc*/ *(uint64_t*)((uintptr_t)g + 0x148),
                 /*instanceVB*/ (ID3D11Buffer**)((uintptr_t)grp + 0x40));
        }
    } return;

    case 11: { // PARTICLE_SHADER_DYNAMIC_TRISHAPE
        BSDynamicTriShape* d = g->vtbl[+0x60](g);
        uint16_t vertCount = *(uint16_t*)((uintptr_t)d + 0x15A);
        const void* data = LockDynamicDataForRead(d);
        DrawParticleShaderTriShape_140D6CBE0(data, vertCount);
        UnlockDynamicData(d);
    } return;

    case 12: // LINES
        DrawLineShape_140D6D310(*(LineShape**)((uintptr_t)g+0x138), *(uint16_t*)((uintptr_t)g+0x158));
        return;

    case 13: { // DYNAMIC_LINES  (implemented — Nukem asserted false)
        void* rd = *(void**)((uintptr_t)g + 0x138);
        void* dst = MapDynamicLines_140D6D5D0(rd, 0);          // size 0 → rd+0x1C; offset → rd+0x18
        uint32_t sz = *(uint32_t*)((uintptr_t)g + 0x160);
        memcpy_s(dst, sz, GetDynamicData_141317E50(g), sz);
        Unmap_140D6D5F0();
        DrawDynamicLines_140D6D620(rd, 0, *(uint16_t*)((uintptr_t)g+0x158)); // resets rd+0x18 = -1 after draw
        UnlockDynamicLines_141317E80(g);
    } return;
    }
}
```

## 3. Renderer draw leaves (exact D3D11 sequences)

Every leaf follows the same prologue: inline SetVertexDescription (compare/set u64 @0x1430281F0, set dirty 0x400 in @0x143027EB0), inline SetPrimitiveTopology (compare/set u32 @0x143028208, dirty 0x800), then `SetDirtyStates(false)` (0x140D705B0). Context = `*(ID3D11DeviceContext**)0x143027EA0` (= renderer[0x1430261B0]+0x1CF0).

**DrawTriShape 0x140D6BFE0**(rd = {VB@+0, IB@+8, desc@+0x10}, startIndex, triCount): topology 4 (TRIANGLELIST); `IASetIndexBuffer(rd->IB, DXGI_FORMAT_R16_UINT, 0)`; stride = `(desc & 0xF) * 4`; `IASetVertexBuffers(0, 1, &rd->VB, &stride, &offset0)`; `DrawIndexed(3*triCount, startIndex, 0)`.

**DrawTriShapeAltIndex 0x140D6C0E0**: identical except `IASetIndexBuffer(*(ID3D11Buffer**)altIBStruct, 57, 0)` — alt IB struct from geometry+0x160.

**DrawDynamicTriShape 0x140D6CAB0**(shapeData{VB,IB,desc}, drawData(UNUSED), startIndex, triCount, vbOffset): topology 4; `IASetIndexBuffer(shapeData->IB, 57, 0)`; buffers = {shapeData->VB, DynVB[cur]} (cur = u32 @0x143025F30, buffers @0x143025F18+8i); strides = {(desc&0xF)*4, (desc>>2)&0x3C}; offsets = {0, vbOffset}; `IASetVertexBuffers(0, 2, ...)`; `DrawIndexed(3*triCount, startIndex, 0)`.
**DrawDynamicTriShapeUnknown 0x140D6CA60** = wrapper: copies shape's {VB,IB,desc} and passes `vbOffset = *(shape+0x18)` (m_VertexAllocationOffset).

**AllocateAndMapDynamicVertexBuffer 0x140D6C8A0**(size, &outOffset): ring of **3× 4 MiB** dynamic VBs @0x143025F18; cur idx @0x143025F30, cur offset @0x143025F34; overflow (offset+size > 0x400000) → `flags[idx]=0` (byte @0x143026164+idx), `ctx->End(query[idx])` (queries @0x143026168+8i), idx=(idx+1)%3, offset=0; if `!flags[idx]`: `hr=ctx->GetData(query,&b,4,1 /*DONOTFLUSH first!*/)`, loop `{Sleep(1); GetData(...,0);}` until `hr>=0 && b`, `flags[idx] = (b==1)`; `ctx->Map(DynVB[idx], 0, D3D11_MAP_WRITE_NO_OVERWRITE /*5*/, 0, &ms)`; commits idx/offset; `*outOffset = oldOffset`; returns `ms.pData + oldOffset`.
**UnmapDynamicVertexBuffer** 0x140D6C9E0 (== 0x140D6CA30 == 0x140D6D5F0): `ctx->Unmap(DynVB[cur], 0)`.
**MapDynamicTriShapeDynamicData 0x140D6CA10**(dynShape UNUSED, rd, drawData UNUSED, size): size==0 → `size = *(rd+0x1C)`; → Allocate(size, (uint32_t*)(rd+0x18)).

**DrawParticleShaderTriShape 0x140D6CBE0**(data, vertCount): stride 48; alloc+map 48*vertCount, memcpy_s, `Unmap(DynVB[cur],0)`; topology 4; clears dirty 0x400 (custom layout); SetDirtyStates(0); lazily `device->CreateInputLayout({POSITION,0,R32G32B32A32_FLOAT(2),0,0},{NORMAL,0,fmt 2,0,16},{TEXCOORD,0,R32G32B32_FLOAT(6),0,32},{TEXCOORD,1,R8G8B8A8_SINT(32),0,44}}, 4, curVS+0x68 /*inline bytecode*/, *(curVS+0x10) /*len*/, &layout @0x143025F48)` (device @0x143025F08); inserts `desc = shadowDesc & *(curVS+0x48)` into InputLayoutMap (buckets @0x141E07160, size @0x141E07144, sentinel @0x141E07150, insert 0x140D71830); `IASetInputLayout(layout)`; re-set dirty 0x400; `IASetIndexBuffer(SharedParticleIB @0x143025F38, 57, 0)`; `IASetVertexBuffers(0,1,&DynVB[cur],{48},{allocOffset})`; `DrawIndexed(6*(vertCount/4), 0, 0)`.

**DrawMultiStreamInstanced (grass) 0x140D6C1E0**(rd, startIndex, triCount, instCount, desc, &instVB): SetVertexDescription(**passed geometry desc**, not rd's); topology 4; `IASetIndexBuffer(rd->IB, 57, 0)`; buffers={rd->VB, *instVB}, strides={(desc&0xF)*4, (desc>>2)&0x3C}, offsets={0,0}; `IASetVertexBuffers(0,2,...)`; **`DrawIndexedInstanced(3*triCount, instCount, startIndex, 0, 0)`**.

**DrawLineShape 0x140D6D310**(rd, count): topology **2** (LINELIST); IB(rd->IB,57,0); VB(0,1,&rd->VB,{(desc&0xF)*4},{0}); `DrawIndexed(2*count, 0, 0)` — no start-index parameter in 1.5.97.

**DrawDynamicLines 0x140D6D620**(rd, startIndex, count): topology 2; IB(rd->IB,57,0); buffers={rd->VB, DynVB[cur]}, strides={(desc&0xF)*4,(desc>>2)&0x3C}, offsets={0, *(rd+0x18)}; `DrawIndexed(2*count, startIndex, 0)`; then `*(rd+0x18) = -1`.

**DrawStripParticles 0x140D6CE60**: two sequential dynamic-VB allocations (verts: 40*vertCount @stride 40; indices: 2*indexCount u16) each map/memcpy/Unmap; `IASetVertexBuffers(0,1,&DynVB[cur],{40},{vertAllocOffset})`; `IASetIndexBuffer(DynVB[cur], 57, indexAllocOffset)` — **index buffer IS the dynamic vertex buffer at the second allocation's offset**; topology **5** (TRIANGLESTRIP); clears dirty 0x400; lazy `CreateInputLayout({POSITION R32G32B32F@0},{TEXCOORD R32G32B32F@12},{NORMAL R32G32B32F@24},{COLOR R8G8B8A8_UNORM(28)@36}}, curVS bytecode)` → layout @0x143025F50; `IASetInputLayout`; `DrawIndexed(indexCount, 0, 0)`.

**GetParticlesDynamicTriShape 0x140D6C7E0**: init-once static @0x14302AE50 = {VB=SharedParticleStaticBuffer(@0x143025F40's value), IB=SharedParticleIB(@0x143025F38's value), desc=0x840200004000051, allocOffset=0xFFFFFFFF, allocSize=0, refcount=1, raw=null,null}.

## 4. Skinned partition flow

`NiSkinInstance::Render` (vtbl idx37 = 0x140C7E170, NiSkinInstance vtable @0x141767CF0; **BSDismemberSkinInstance does NOT override it**) → partition loop 0x140C7CA10: `for i in 0..NiSkinPartition(+0x18)->count(+0x10): skinPartition->vtbl[idx37 +0x128](partitionList, skinRenderData, i)` = **0x140C7CA70** (NiSkinPartition vtable @0x14176A0A0):

```cpp
bool NiSkinPartition::RenderPartition(SkinRenderData* srd, uint32_t i)   // 0x140C7CA70
{
    Partition* p = *(Partition**)((uintptr_t)this + 0x18) + i;   // stride 0x50
    // LOD gate: table @0x141E06650[ 12*srd->singleLevel + 3*srd->lodIndex + p->lodLevel(+0x42 byte) ]
    if (!byte_141E06650[3*(srd->lodIndex + 4*srd->singleLevel) + p->lod]) return false;

    srd->setter->SetBoneMatrix(                                   // vtbl[1] = 0x14131F630
        srd->geometry->skinInstance /*+0x130*/, p, &srd->geometry->worldTransform /*+0x7C*/);

    if (BSDynamicTriShape* d = srd->geometry->AsDynamicTriShape() /*vtbl+0x60*/)
        gRendererIface /*@0x1430136C0, vtbl 0x14186BF80*/ ->vtbl[6 /*+0x30 = 0x141327FF0*/](
            p->rendererData /*+0x48, DynamicTriShapeData*/, (void*)((uintptr_t)d+0x178) /*drawData, unused*/,
            /*startIndex*/0, /*triCount*/ p->numTris /*u16 +0x3A*/, /*vbOffset*/ srd->vertexBufferOffset /*+0x24*/);
    else
        gRendererIface->vtbl[7 /*+0x38 = 0x141327FD0*/](
            p->rendererData /*+0x48, TriShape*/, /*startIndex*/0, /*triCount*/ p->numTris);
    return true;
}
```
The interface thunks 0x141327FF0/0x141327FD0 forward to DrawDynamicTriShape 0x140D6CAB0 / DrawTriShape 0x140D6BFE0 with rcx=0x143028490. **SetupTechnique is NOT re-called per partition** — only SetBoneMatrix + draw (Nukem's comment is wrong for 1.5.97).

**SetBoneMatrix 0x14131F630** (shared BSShader impl; BSUtilityShader NiBoneMatrixSetterI vtable @0x141868608, slot1):
```cpp
void SetBoneMatrix(NiSkinInstance* skin, Partition* p, NiTransform* worldXform)
{
    if (TLS[+0x2A00] == skin || !p || !p->m_usBones /*+0x3C*/) return;  // per-thread last-skin cache
    TLS[+0x2A00] = skin;
    UpdateBoneMatrices_140D74F70(skin, worldXform);   // frame-gated (skin+0x38 vs uiFrameCount), CritSec skin+0x60:
        // bones = SkinData(+0x10)->m_uiBones(+0x58); rows = 3*bones (float4 each, 48 B/bone)
        // memcpy prev(+0x50) ← current(+0x48) BEFORE recompute (motion vectors)
        // per bone: NiTransform::Multiply(boneWorld(+0x30 array)[i], geomChainXform, skinData boneOffset(stride 88))
        //   packed 3x4: {R00,R01,R02,Tx},{R10,R11,R12,Ty},{R20,R21,R22,Tz} (rotation×scale)
    uint32_t rows = 3 * skin->skinData->numBones;
    uint32_t* m; ID3D11Buffer** cb;
    cb = GetDynamicConstantBuffer_140D6FFD0(rows, &m, /*slot*/10);  // rotating pool of 4 CBs @0x143027A08..20, counter @0x143027A00, Map DISCARD
    memcpy_s(m, 16*rows, skin->boneMatrices /*+0x48 current*/, 16*rows);
    if (*cb) ctx->Unmap(*cb, 0);
    ctx->VSSetConstantBuffers(10, 1, cb);             // b10 = CURRENT bones
    cb = GetDynamicConstantBuffer_140D6FFD0(rows, &m, /*slot*/9);
    memcpy_s(m, 16*rows, skin->prevBoneMatrices /*+0x50 previous*/, 16*rows);
    if (*cb) ctx->Unmap(*cb, 0);
    ctx->VSSetConstantBuffers(9, 1, cb);              // b9 = PREVIOUS bones
}
```
`GetDynamicConstantBuffer 0x140D6FFD0(sizeArg unused-for-selection, &mapped, slotSel)`: slotSel==7 → dedicated grass CB @0x143027E90; else rotate 4-buffer pool; `ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD=4, 0, &ms)`; stores buf into static holder @0x14302AC58 and returns its address; `*mapped = ms.pData`.

## 5. Renderer-globals map (absolute, 1.5.97)

Dynamic-geometry block (base 0x143025F00):
- 0x143025F08 ID3D11Device*  (CreateInputLayout)
- 0x143025F18+8i (i=0..2) ID3D11Buffer* DynamicVB[3] (4 MiB, MAP_WRITE_NO_OVERWRITE ring)
- 0x143025F30 u32 currentDynamicVB;  0x143025F34 u32 currentDynamicVBOffset
- 0x143025F38 ID3D11Buffer* SharedParticleIndexBuffer;  0x143025F40 ID3D11Buffer* SharedParticleStaticBuffer
- 0x143025F48 ID3D11InputLayout* ParticleShaderInputLayout;  0x143025F50 ID3D11InputLayout* StripParticleInputLayout
- 0x143026164+i byte DynamicQueryFinished[3];  0x143026168+8i ID3D11Query* DynamicVBAvailQuery[3]

Renderer object (base 0x1430261B0):
- +0x1850 (0x143027A00) u32 dynamicCB rotation counter; +0x1858..0x1870 (0x143027A08..20) ID3D11Buffer* dynCB pool[4]
- +0x1CE0 (0x143027E90) ID3D11Buffer* grass instance-index CB (bound b7)
- +0x1CF0 (0x143027EA0) ID3D11DeviceContext* immediate context

RendererShadowState:
- 0x143027EB0 u32 m_StateUpdateFlags (0x400 DIRTY_VERTEX_DESC, 0x800 DIRTY_PRIMITIVE_TOPO; 0x80/0x4/0x1 seen in other clusters)
- 0x143027EB4 u32 m_PSResourceModifiedBits
- 0x143027EB8 u32 (next dirty word, Nukem: m_PSSamplerModifiedBits)
- 0x143027FF0+8*slot void* m_PSTexture[≥13] (land path writes slots 0..5 and 7..12)
- 0x1430281F0 u64 m_VertexDesc;  0x1430281F8 VertexShader* m_CurrentVertexShader (+0x10 bytecode len, +0x48 vertex-desc mask, +0x68 inline bytecode);  0x143028200 PixelShader* m_CurrentPixelShader;  0x143028208 u32 m_Topology
- 0x143028490 = "Renderer this" pointer value passed in rcx to all leaves (0x143028470+0x20); leaves use absolute addressing

Batch/pass state: 0x143283BA0 u32 (cleared by 0x141308540); 0x143283BA4 u32 currentTechnique; 0x143283BA8 BSShader* currentShader; 0x143490BB0 BSShaderMaterial* currentMaterial; 0x141E0DF8C u32 debugTechnique (1.5.23: 0x141E32FDC).

gState: 0x14302C8DC u32 uiFrameCount; 0x14302C8E5 byte bUseEarlyZ; 0x14302C8E8 default-land-texture holder* (slot reset value = `*(*(holder+0x48)+0x10)`).

Misc: 0x141E06650 byte LOD-partition visibility table [12*single+3*lodIdx+partLOD]; 0x1430243B0 / 0x1434963C8 bools "draw whole (land) subindex geometry" (0x1430243B0 toggled by 0x1404B35E0); 0x1430136C0 virtual renderer interface* (vtable 0x14186BF80); 0x14302AE50 static particles DynamicTriShape; 0x14302AC58 static CB holder; 0x141E07160/44/50 InputLayoutMap; 0x143283BB0/0x143477BB0 strip-particle CPU staging; TLS: +0x768 memory-context id, +0x2A00 last-skin bone-CB cache (cleared by 0x14131F7C0 before every skinned pass).

Struct offsets (1.5.97): BSRenderPass {+0 shader, +8 property, +0x10 geometry, +0x18 passEnum u32, +0x1C accumulationHint u8 (==12 → alt index set), +0x1E LODMode u8 (bit7 single, 0..6 index)}. BSGeometry {+0x7C worldTransform, +0x108 currentLOD byte (copied from pass each RenderPassImmediately — Nukem's noted MT hazard), +0x109 flags (bit3 custom), +0x130 skinInstance, +0x138 rendererData, +0x148 vertexDesc u64, +0x150 type u8, +0x158 triCount u16, +0x15A vertCount u16}. BSDynamicTriShape {+0x160 pDynamicData, +0x168 lockOwnerTid, +0x16C lockRecursion, +0x170 dynamicDataSize, +0x174 uiFrameCount, +0x178 drawData/offset u32}. BSGraphics::TriShape rendererData {+0 VB, +8 IB, +0x10 desc, +0x18 refcount, +0x20/+0x28 raw ptrs}; DynamicTriShape rendererData {+0 VB, +8 IB, +0x10 desc, +0x18 allocOffset, +0x1C allocSize, +0x20 refcount, +0x28/+0x30 raw} — identical to Nukem. NiSkinPartition::Partition stride 0x50 {+0x3A numTris u16, +0x3C numBones u16, +0x42 lod u8, +0x48 rendererData}. NiSkinInstance {+0x10 skinData, +0x18 skinPartition, +0x20 rootParent, +0x30 boneWorldTransforms**, +0x38 frameId, +0x3C numMatrices, +0x40 numRegisters=3, +0x44 allocatedBytes, +0x48 boneMatrices, +0x50 prevBoneMatrices, +0x60 critsec}. SkinRenderData (stack) {+0 setter, +8 geometry, +0x10 0, +0x18 singleLevel, +0x1C lodIndex, +0x20 0, +0x24 vbOffset=-1}.

Vertex strides everywhere: stream0 = (desc & 0xF)*4; dynamic/instance stream = ((desc >> 4) & 0xF)*4 (encoded as (desc>>2)&0x3C). Index format always DXGI_FORMAT_R16_UINT (57).

## 6. Utility-pass relevance
Shadow maps / z-prepass draw through: case 3 (static trishapes — vast majority), case 5/6/7 (LOD terrain/city meshes; **hint==12 alternate-index path is likely the depth-optimized index set — must be replicated**), case 8/9 (landscape), case 10 (grass, when grass shadows on — DrawIndexedInstanced + cb7), skinned path (actors: bone CBs b9/b10 DISCARD + per-partition draws; dynamic skinned (faces) additionally streams vertices through the dynamic-VB ring), case 4 (dynamic trishapes, once-per-frame upload gate on uiFrameCount), case 11/1/2 (particles — only in accumulations with RenderFlags&4-style effect passes, rarely utility).

### Every D3D11 call (arg -> data source)
- ctx->IASetIndexBuffer(rd->IB [TriShape+0x08], DXGI_FORMAT_R16_UINT=57, 0) <- DrawTriShape 0x140D6BFE0; rd from geometry+0x138 or NiSkinPartition::Partition+0x48
- ctx->IASetVertexBuffers(0, 1, &rd->VB [TriShape+0x00], &stride=(rd->desc[+0x10] & 0xF)*4, &offset=0) <- DrawTriShape 0x140D6BFE0
- ctx->DrawIndexed(3*triCount, startIndex, 0) <- DrawTriShape 0x140D6BFE0; triCount/startIndex per Draw case: case3 (0, u16 geom+0x158), case5 (meshlod tables geom+0x160 via 0x141330240/0x1413300C0, level byte geom+0x108), case6 col0 (tables geom+0x1D8/0x1E0 via 0x1413308F0/0x141330850), case7 (0, u16+0x158), case8/9 (segment records geom+0x160 stride 0x14: start +0x00, count +0x0C; or whole u16+0x158 when byte geom+0x171), skinned partition (0, u16 partition+0x3A)
- ctx->IASetIndexBuffer(*(ID3D11Buffer**)(geom+0x160 struct), 57, 0) <- DrawTriShapeAltIndex 0x140D6C0E0, used when BSRenderPass+0x1C (accumulationHint byte) == 12 for types 6/7
- ctx->IASetVertexBuffers(0, 2, {shapeData->VB, DynamicVB[cur @0x143025F30] @0x143025F18+8i}, {(desc&0xF)*4, (desc>>2)&0x3C}, {0, vbOffset}) <- DrawDynamicTriShape 0x140D6CAB0; vbOffset = DynamicTriShape rendererData+0x18 (via 0x140D6CA60) or SkinRenderData+0x24 (partition path)
- ctx->DrawIndexed(3*triCount, startIndex, 0) <- DrawDynamicTriShape 0x140D6CAB0; case4: (0, u16 dynShape+0x158); case1 particles: (0, 2*min(activeVerts,2048)); skinned dynamic partition: (0, u16 partition+0x3A)
- ctx->End(DynamicVBAvailQuery[oldIdx] @0x143026168+8i) <- AllocateAndMapDynamicVertexBuffer 0x140D6C8A0 on 4MiB overflow
- ctx->GetData(DynamicVBAvailQuery[idx], &BOOL, 4, first call flags=1 DONOTFLUSH then 0) + Sleep(1) loop <- 0x140D6C8A0 when flag byte @0x143026164+idx is clear
- ctx->Map(DynamicVB[idx], 0, D3D11_MAP_WRITE_NO_OVERWRITE=5, 0, &ms) <- 0x140D6C8A0; returns ms.pData + prevOffset (offset @0x143025F34)
- ctx->Unmap(DynamicVB[cur], 0) <- UnmapDynamicVertexBuffer 0x140D6C9E0 / 0x140D6CA30 / 0x140D6D5F0
- device->CreateInputLayout({POSITION R32G32B32A32_FLOAT@0, NORMAL R32G32B32A32_FLOAT@16, TEXCOORD0 R32G32B32_FLOAT@32, TEXCOORD1 R8G8B8A8_SINT@44}, 4, curVS+0x68 inline bytecode, *(curVS+0x10) len, &layout @0x143025F48) <- DrawParticleShaderTriShape 0x140D6CBE0, lazy once; device @0x143025F08, curVS @0x1430281F8
- ctx->IASetInputLayout(ParticleShaderInputLayout @0x143025F48) <- 0x140D6CBE0 (with DIRTY_VERTEX_DESC 0x400 cleared before SetDirtyStates, re-set after)
- ctx->IASetIndexBuffer(SharedParticleIndexBuffer @0x143025F38, 57, 0) <- 0x140D6CBE0
- ctx->IASetVertexBuffers(0, 1, &DynamicVB[cur], {48}, {allocOffset}) <- 0x140D6CBE0; data = memcpy of BSDynamicTriShape dynamic data (48 B/vertex * u16 geom+0x15A)
- ctx->DrawIndexed(6*(vertCount/4), 0, 0) <- DrawParticleShaderTriShape 0x140D6CBE0
- ctx->Map(grassCB @0x143027E90, 0, D3D11_MAP_WRITE_DISCARD=4, 0, &ms); *(u32*)ms.pData = instanceGroupIndex; ctx->Unmap(grassCB, 0) <- Draw case10 via GetDynamicConstantBuffer 0x140D6FFD0 slotSel=7
- ctx->VSSetConstantBuffers(7, 1, &grassCB) <- Draw case10 (grass instance-group index in b7)
- ctx->IASetVertexBuffers(0, 2, {rd->VB, *(grp+0x40) instance stream VB}, {(geomDesc&0xF)*4, (geomDesc>>2)&0x3C}, {0, 0}) <- DrawMultiStreamInstanced 0x140D6C1E0; geomDesc = geometry+0x148 (NOT rd->desc)
- ctx->DrawIndexedInstanced(3*u16(geom+0x158), *(grp+0x4C) instanceCount, 0, 0, 0) <- 0x140D6C1E0 (grass)
- ctx->Map(rotatingDynCB pool[4] @0x143027A08+8i, counter @0x143027A00, 0, DISCARD=4, 0, &ms); memcpy_s(ms.pData, 48*numBones, skin+0x48 current bones); ctx->Unmap; ctx->VSSetConstantBuffers(10, 1, &cb) <- SetBoneMatrix 0x14131F630 (b10 = CURRENT bones; numBones = *(skinData+0x58))
- ctx->Map(next rotating CB, DISCARD); memcpy_s(prev bones skin+0x50); ctx->Unmap; ctx->VSSetConstantBuffers(9, 1, &cb) <- SetBoneMatrix 0x14131F630 (b9 = PREVIOUS-frame bones)
- ctx->IASetIndexBuffer(rd->IB, 57, 0); IASetVertexBuffers(0,1,&rd->VB,{(desc&0xF)*4},{0}); DrawIndexed(2*count, 0, 0) with topology=2 LINELIST <- DrawLineShape 0x140D6D310 (count = u16 geom+0x158)
- ctx->IASetVertexBuffers(0, 2, {rd->VB, DynamicVB[cur]}, {(desc&0xF)*4, (desc>>2)&0x3C}, {0, *(rd+0x18)}); DrawIndexed(2*count, startIndex=0, 0); then *(rd+0x18)=-1 <- DrawDynamicLines 0x140D6D620, topology 2
- ctx->IASetVertexBuffers(0, 1, &DynamicVB[cur], {40}, {vertAllocOffset}); ctx->IASetIndexBuffer(DynamicVB[cur], 57, indexAllocOffset); DrawIndexed(indexCount, 0, 0) with topology=5 TRIANGLESTRIP + own layout @0x143025F50 (POSITION R32G32B32F@0, TEXCOORD R32G32B32F@12, NORMAL R32G32B32F@24, COLOR R8G8B8A8_UNORM@36) <- DrawStripParticles 0x140D6CE60 (index buffer IS the dynamic vertex buffer)
- SetDirtyStates(false) 0x140D705B0 <- called by EVERY draw leaf after writing shadow-state m_VertexDesc @0x1430281F0 (dirty 0x400) and m_Topology @0x143028208 (dirty 0x800) into m_StateUpdateFlags @0x143027EB0; emits all other binds (other cluster)
- shadow-state only (consumed by SetDirtyStates, no direct ctx call): m_PSTexture[slot 0..5, 7..12] @0x143027FF0+8*slot = *(*(defaultLandTexHolder @0x14302C8E8 +0x48)+0x10), dirty bits 1<<slot in m_PSResourceModifiedBits @0x143027EB4 <- Draw case9 SUBINDEX_LAND segment loop

### Divergences from Nukem's 1.5.23 RE
- IDB/anchor corrections: 0x1413088C0 is _Standard and 0x141308970 is _Skinned (IDA names swapped); 0x1413083B0 is NOT ShaderSetup (it's a DiscardBatches-style map-clear helper); real ShaderSetup = 0x141309F80; 0x141308520/0x141308540 are static-state clearers, not _Standard/_Skinned entry points
- Nukem's BSBatchRenderer::Draw is his reimplementation; the actual 1.5.97 binary has ONE function (0x141307160) = his Draw + sub_14131DDF0 merged, with ALL 13 geometry types implemented. His AssertDebug(false) cases are real code in 1.5.97: MESHLOD (case5), LOD_MULTIINDEX (case6), SUBINDEX_LAND (case9), DYNAMIC_LINES (case13). Only INSTANCE_GROUP (default) is a no-op
- MULTIINDEX/LOD_MULTIINDEX types have an undocumented alternate-index-buffer path selected by BSRenderPass+0x1C (accumulationHint byte) == 12, drawing from a second ID3D11Buffer index set at geometry+0x160 with separate count tables (geom+0x1D8/0x1E0) — absent from Nukem's RE
- Grass (MULTISTREAMINSTANCE) path fully undocumented by Nukem: per instance-group Map-DISCARD of a dedicated CB @0x143027E90 with the group index, VSSetConstantBuffers(b7), then DrawIndexedInstanced with the instance stream VB as slot-1 vertex stream (stride = dynamic-vertex-size nibble of geometry desc)
- GetData flag order in AllocateAndMapDynamicVertexBuffer: 1.5.97 first call uses D3D11_ASYNC_GETDATA_DONOTFLUSH(1), retries use 0; Nukem wrote first=0, retries=DONOTFLUSH
- DrawLineShape 1.5.97 has no StartIndex parameter (DrawIndexed(2*count, 0, 0)); Nukem's signature has StartIndex
- Nukem's particle-case dead code (after his early return to sub_14131DDF0) matches actual 1.5.97 behavior almost exactly: clamp min(activeCount, 2048), map 4*count*dynamicVertexSize, PackDynamicParticleData 0x140D76080, draw 2*count triangles
- SetBoneMatrix internals not covered by Nukem: rotating pool of 4 dynamic CBs (Map DISCARD), current bones -> VS b10, previous-frame bones -> VS b9, 48 bytes/bone (3 float4 rows), gated by a per-thread TLS[+0x2A00] last-skin cache that _Skinned clears once per pass (his 1.5.23 sub_141336450 = 1.5.97 0x14131F7C0)
- Partition loop does NOT re-call SetupTechnique per partition (Nukem's comment 'Renders multiple skinned instances (SetupTechnique, SetBoneMatrix)' is wrong for 1.5.97): only SetBoneMatrix + draw per partition; BSDismemberSkinInstance does not override Render
- The partition/skin draw goes through a virtual renderer interface (global @0x1430136C0, vtable 0x14186BF80; idx6=DrawDynamicTriShape thunk 0x141327FF0, idx7=DrawTriShape thunk 0x141327FD0) rather than direct calls — Nukem flattened this
- Dynamic-VB ring confirmed as 3 buffers x 4 MiB in 1.5.97 (Nukem's array size implicit)
- BeginPass in 1.5.97 also clears the last-material global (0x143490BB0) before SetupTechnique; write-only technique global moved 0x141E32FDC (1.5.23) -> 0x141E0DF8C (1.5.97)
- RenderPassImmediately copies the WHOLE LODMode byte (incl. bit7 single-level flag) into geometry+0x108, and the mesh-LOD count/start helpers re-read bit7 from there — Nukem's transcription only mentions ucCurrentMeshLODLevel
- Strip-particle draw (case 2) builds BOTH vertices and 16-bit indices in the dynamic vertex buffer (two allocations) and binds the dynamic VB as index buffer with the allocation offset — not in Nukem's RE

### Open questions
- BSRenderPass+0x1C == 12: assumed m_AccumulationHint; which accumulation hint value 12 means (likely depth/shadow-optimized index set) and which utility passes carry it is unverified — needs runtime confirmation before replication of types 6/7
- Bone CB slot assignment measured as b10=CURRENT bones (skin+0x48), b9=PREVIOUS-frame bones (skin+0x50); worth cross-checking against utility/lighting VS bytecode register usage before relying on it
- byte_141E06650 LOD-partition visibility table contents not dumped (indexed 12*singleLevel + 3*lodIndex + partitionLOD; implies bounds single<=1, lodIndex<=3, partLOD<=2) — dump the 24 bytes if replicating skinned LOD gating
- unk_14302C8E8 exact type (default land texture holder; slot value = *(*(g+0x48)+0x10)) not identified; also read by BSLightingShaderProperty 0x1412C56D0
- unk_1430243B0 / unk_1434963C8 ('draw whole sub-index geometry' bools): writer 0x1404B35E0 belongs to an unidentified manager (terrain/decal-merge system); semantics of when they are true during gameplay unverified
- VertexShader struct layout inferred from CreateInputLayout call: bytecode inline at +0x68, length at +0x10, vertex-desc mask at +0x48 — consistent with Nukem but not independently verified
- MapDynamicTriShapeDynamicData's dynShape and drawData params are dead in 1.5.97 (offset is stored in the rendererData +0x18, and drawData u32 at dynShape+0x178 appears vestigial) — matches Nukem, but confirm nothing else reads dynShape+0x178 before replicating
- Strip-particle (case 2) vertex packing (0x140D76D80) and its out-params (vertex count vs index count accumulation) transcribed at medium confidence only — irrelevant for depth-only utility passes but flagged
- PackParticleData 0x140D76080 internals not decompiled (CPU particle vertex packing); needed only if particles appear in a replicated pass
- FUN_14131FF40 / FUN_140D7B670 assumed memcpy_s by analogy with the verified 0x14130A030/0x140D74A60 bodies (identical call shape); not individually decompiled
- FUN_1410A2370 (sets VS/PS directly, PS texture slot 0, 4x DrawTriShape) is a separate facegen/multi-part renderer NOT reachable from RenderPassImmediately — excluded from cluster; flagged in case another cluster needs it
- The writer of the virtual renderer interface global 0x1430136C0 (concrete object ctor) was not located; vtable 0x14186BF80 resolved via thunk xrefs instead — concrete class identity (BSGraphics::Renderer as NiRenderer-style interface) unnamed

### Key addresses
- 0x141308440 BSBatchRenderer::RenderPassImmediately(Pass, Technique, AlphaTest, RenderFlags)
- 0x1413086C0 BSBatchRenderer::BeginPass(technique ecx, shader rdx) — inlined EndPass + SetupTechnique vtbl[2]
- 0x1413088C0 RenderPassImmediately_Standard (IDB mislabels as _Skinned)
- 0x141308970 RenderPassImmediately_Skinned (IDB mislabels as _Standard)
- 0x141308B20 RenderPassImmediately_Custom
- 0x141309F80 BSBatchRenderer::ShaderSetup(Pass, Shader, AlphaTest, RenderFlags)
- 0x141307160 BSBatchRenderer::Draw(Pass) — 13-case geometry switch (= 1.5.23 sub_14131DDF0 merged)
- 0x1413083B0 DiscardBatches-style helper (NOT ShaderSetup)
- 0x141308520 ClearShaderAndTechnique (zeroes 0x143283BA4/A8, 0x143490BB0)
- 0x141308540 clears u32 @0x143283BA0
- 0x1412CCE20 BSShaderAccumulator::IsGrassShadowBlacklist (technique-0x5C000058 <= 3)
- 0x14131F440 BSShader::SetupGeometryAlphaBlending (free fn, IDB says BSEffectShader)
- 0x14131F2A0 BSShader::SetupAlphaTestRef
- 0x1412FD8A0 BSRenderPass::GetNiProperty (alpha property)
- 0x14131F7C0 ClearBoneSetterTLSCache (TLS+0x2A00=0; 1.5.23 sub_141336450)
- 0x140D6BFE0 BSGraphics::Renderer::DrawTriShape(rd, startIndex, triCount)
- 0x140D6C0E0 DrawTriShape with alternate index buffer (hint==12 multi-index path)
- 0x140D6CAB0 BSGraphics::Renderer::DrawDynamicTriShape(shapeData, drawData, startIdx, triCount, vbOffset)
- 0x140D6CA60 DrawDynamicTriShapeUnknown (wrapper, vbOffset from rd+0x18)
- 0x140D6CA10 MapDynamicTriShapeDynamicData (size 0 -> rd+0x1C; offset out -> rd+0x18)
- 0x140D6CA30 UnmapDynamicTriShapeDynamicData (= Unmap DynVB[cur])
- 0x140D6C8A0 AllocateAndMapDynamicVertexBuffer (3x4MiB ring, queries, MAP_NO_OVERWRITE)
- 0x140D6C9E0 UnmapDynamicVertexBuffer
- 0x140D6CBE0 DrawParticleShaderTriShape (48B verts, custom input layout, DrawIndexed 6*(n/4))
- 0x140D6C1E0 DrawMultiStreamInstanced (grass; DrawIndexedInstanced)
- 0x140D6FFD0 GetDynamicConstantBuffer (slotSel 7 = grass CB @0x143027E90; else 4-CB rotating pool; Map DISCARD)
- 0x140D6D310 DrawLineShape (LINELIST, DrawIndexed(2n,0,0))
- 0x140D6D5D0 MapDynamicLines; 0x140D6D5F0 Unmap; 0x140D6D620 DrawDynamicLines (resets rd+0x18=-1)
- 0x140D6CE60 DrawStripParticles (verts+indices both in dynamic VB, TRIANGLESTRIP, layout @0x143025F50)
- 0x140D6C7E0 GetParticlesDynamicTriShape (static @0x14302AE50, desc 0x840200004000051)
- 0x140D76080 PackDynamicParticleData (CPU pack)
- 0x140D76D80 BuildStripParticleGeometry (CPU pack into 0x143283BB0/0x143477BB0)
- 0x140D705B0 BSGraphics::Renderer::SetDirtyStates (called by every leaf; other cluster)
- 0x14130A030 memcpy_s; 0x140D74A60 memcpy_s (identical); 0x14131FF40/0x140D7B670 memcpy_s (assumed)
- 0x141330240 MeshLOD triangle count (tables geom+0x160); 0x1413300C0 MeshLOD start index (3*sum prev, lod byte geom+0x108)
- 0x1413308F0 multi-index count (pairs @geom+0x1D8, col 0/1); 0x141330850 multi-index start (3*sum, @+0x1D8/+0x1E0)
- 0x140D59430 BSSubIndexTriShape segment consolidation; 0x141334070 land per-segment texture mask (*(geom+0x178))[i]
- 0x140C7E170 NiSkinInstance::Render (vtbl idx37; vtable @0x141767CF0); 0x140C7CA10 partition loop
- 0x140C7CA70 NiSkinPartition::RenderPartition (vtbl idx37; vtable @0x14176A0A0; partitions @this+0x18 stride 0x50)
- 0x14131F630 BSShader::SetBoneMatrix (NiBoneMatrixSetterI vtbl[1] @0x141868608; b10 current / b9 previous bones)
- 0x140D74F70 NiSkinInstance::UpdateBoneMatrices (frame-gated, critsec +0x60, 48B/bone 3x4 rows)
- 0x140C7BAD0 Partition zero-init ctor; 0x140C7BB10 Partition dtor
- 0x140C723F0 BSDynamicTriShape::LockDynamicDataForRead (lock +0x168, returns +0x160); 0x140C72420 UnlockDynamicData
- 0x141317E50 BSDynamicLines get dynamic data; 0x141317E80 BSDynamicLines unlock
- 0x14130DCE0 BSUtilityShader::Ctor; 0x1418685B0 BSUtilityShader vftable (vtbl[2]=SetupTechnique 0x14130DF90, [3]=RestoreTechnique 0x14130DD80, [4]=SetupMaterial 0x14130E890, [5]=RestoreMaterial 0x14130EC60, [6]=SetupGeometry 0x14130EC70, [7]=RestoreGeometry 0x141310300)
- 0x141868608 BSUtilityShader NiBoneMatrixSetterI vftable (this+0x10; slot1 = 0x14131F630)
- 0x143495D50 BSUtilityShader::pInstance
- 0x1430136C0 virtual renderer interface* (vtable 0x14186BF80; idx6 thunk 0x141327FF0 -> 0x140D6CAB0, idx7 thunk 0x141327FD0 -> 0x140D6BFE0)
- 0x143028490 Renderer 'this' passed to leaves (0x143028470+0x20)
- 0x143025F08 ID3D11Device*
- 0x143025F18 DynamicVB[3] (+8i); 0x143025F30 curIdx; 0x143025F34 curOffset; 0x143025F38 SharedParticleIB; 0x143025F40 SharedParticleStaticVB; 0x143025F48 ParticleShaderInputLayout; 0x143025F50 StripParticleInputLayout
- 0x143026164 DynamicQueryFinished[3] bytes; 0x143026168 DynamicVBAvailQuery[3] (+8i)
- 0x1430261B0 renderer object base; 0x143027A00 dynCB rotation counter; 0x143027A08 dynCB pool[4]; 0x143027E90 grass instance-index CB (b7); 0x143027EA0 ID3D11DeviceContext* immediate
- 0x143027EB0 m_StateUpdateFlags (0x400 vertexdesc, 0x800 topology); 0x143027EB4 m_PSResourceModifiedBits; 0x143027FF0 m_PSTexture[] (+8*slot)
- 0x1430281F0 shadow m_VertexDesc u64; 0x1430281F8 m_CurrentVertexShader (bytecode +0x68, len +0x10, descMask +0x48); 0x143028200 m_CurrentPixelShader; 0x143028208 m_Topology
- 0x143283BA4 currentTechnique; 0x143283BA8 currentShader; 0x143490BB0 currentMaterial; 0x143283BA0 aux state u32; 0x141E0DF8C write-only technique dbg
- 0x14302C8DC gState.uiFrameCount; 0x14302C8E5 gState.bUseEarlyZ; 0x14302C8E8 default land texture holder
- 0x141E06650 skinned-partition LOD visibility table; 0x1430243B0 drawWholeSubIndex bool (writer 0x1404B35E0); 0x1434963C8 drawWholeSubIndexLand bool
- 0x141E07160/0x141E07144/0x141E07150 InputLayoutMap buckets/size/sentinel; 0x140D71830 InputLayoutMap insert; 0x140C06570 hash
- 0x14302AE50 static particles DynamicTriShape; 0x14302AC58 static CB-holder returned by 0x140D6FFD0
- 0x143283BB0 strip-particle CPU vertex staging; 0x143477BB0 strip-particle CPU index staging
- 0x1432336C0 BSSkyShader::pInstance

================================================================================================
# PART II: remaining utility render paths (skinned / custom / non-TRISHAPE / stencil)
# Own IDA analysis, adversarially cross-verified against the 1.5.97 disassembly.
================================================================================================


================================================================================================
## Cluster 1
================================================================================================

# RE: BSBatchRenderer custom render path @ 0x141308B20 (SkyrimSE 1.5.97)

## Custom path body

### Entry conditions (from dispatcher 0x141308440, `RenderPassImmediately(Pass, Technique, AlphaTest, RenderFlags)`)

Shared prologue executed for ALL three paths before dispatch:

1. **Technique dedup**: if `MEMORY[0x143283BA4] (currentTechnique) == Technique && Technique != 0x5C006076 && Pass->m_Shader == MEMORY[0x143283BA8] (currentShader)` → skip BeginPass. Otherwise `dword_141E0DF8C = Technique` and call `BeginPass(Technique, shader)` @0x1413086C0; if it returns 0 the pass is aborted (return).
2. **Material dedup**: `mat = Pass->m_ShaderProperty(+0x8) ? shaderProperty->material(+0x78) : NULL`; if `mat != MEMORY[0x143490BB0] (currentMaterial)` → if non-NULL call shader vtbl **+0x20 = SetupMaterial(mat)** (`Func4_20`), then `MEMORY[0x143490BB0] = mat`.
3. **LOD byte**: `geometry->byte[0x108] = Pass->m_LODMode (byte @pass+0x1E)` — written unconditionally for all paths.
4. **Dispatch**:
   - `geometry->skinInstance (qword @+0x130) != 0` → skinned path 0x141308970
   - else `geometry->flags byte @+0x109 & 8` (bit 11 of the dword at geom+0x108) → **CUSTOM path 0x141308B20** — call site 0x1413084F6/0x1413084F8: `test byte [rsi+109h], 8` then tail-ish call with **rcx=Pass, dl=AlphaTest, r8d=RenderFlags**
   - else → standard path 0x1413088C0

**Who sets the bit**: the +0x109 |= 8 flag is set in the **NiParticles constructors** — 0x140C79F70, 0x140C79BB0 (heap-allocating factory, alloc size 0x168), 0x140C795C0 (`NiParticles::CreateClone`), 0x140C79CA0 (`*(obj+265) |= 8u`; 265 = 0x109). The same ctors write `vertexDesc = 0x0840200004000051` at qword offset 41 (= +0x148) and `byte[336 = 0x150] (GeometryType) = 1`. The bit is cleared (`and byte [.. +0x109], 0xF7`) in the base geometry ctor FUN_140C711A0 (@0x140C7129A) and in 0x140C71A80 / 0x140C80AA0 / NiAVObject::sub_140C80B80 (@0x140C80C44). So the custom path is the **NiParticles-family (particle geometry, GeometryType 1 → Draw dispatch case for particles)** path.

### Full body (decompiled, 0x141308B20)

```c
int32 __usercall RenderPassImmediately_Custom@<eax>(char *a1@<rcx>, uint32 a2@<r8d>, float a3@<xmm0>, int64 a4@<rdx>)
{
  v4 = *a1;                                             // 0x141308b2f  pass->m_Shader (pass+0)
  (v4->vftable->Func6_30)(v4, a1);                      // 0x141308b41  shader vtbl+0x30 = SetupGeometry(pass, RenderFlags)
  NiProperty = BSRenderPass::GetNiProperty(a1);          // 0x141308b47
  BSEffectShader::sub_14131F440(v4, NiProperty, *(a1+1), 1); // 0x141308b59  SetupAlphaBlendState(shader, alphaProp, shaderProperty(pass+8), useAlphaTestBit=1)
  if ( BSRenderPass::GetNiProperty(a1) )                 // 0x141308b61
  {
    v8 = BSRenderPass::GetNiProperty(a1);                // 0x141308b6e
    BSEffectShader::SetAlphaTestRef(v4, v8, *(a1+1));    // 0x141308b7d
  }
  BSRenderPass::FUN_141307160(a1, a1, a3);               // 0x141308b85  Draw dispatch (rcx=pass only; other args are decompiler artifacts)
  return (v4->vftable->Func7_38)(v4, a1, a2);            // tail jmp [rax+38h]: shader vtbl+0x38 = RestoreGeometry(pass, RenderFlags)
}
```

Register-level facts (from disasm):
- 0x141308B2F `mov rdi,[rcx]` → shader; 0x141308B32 `mov rbx,rcx` → pass; 0x141308B35 `mov rdx,rcx` — **immediately clobbers rdx = the incoming AlphaTest argument; AlphaTest is NEVER read in this function**; 0x141308B38 `mov esi,r8d` saves RenderFlags.
- 0x141308B41 `call [rax+30h]` with rcx=shader, rdx=pass, and **r8d still holding the incoming RenderFlags** (unclobbered) → this is `shader->SetupGeometry(pass, RenderFlags)`; the decompiler dropped the 3rd arg.
- 0x141308B59 `sub_14131F440(shader, alphaProp, pass->m_ShaderProperty(+0x8), r9b=1)` — 4th arg **hardcoded 1**.
- 0x141308B85 `FUN_141307160` called with rcx=pass (rdx/xmm0 in decompile are dead-register artifacts).
- Epilogue 0x141308BA5: `jmp qword [rax+38h]` with rcx=shader, rdx=pass, r8d=esi(RenderFlags) → `shader->RestoreGeometry(pass, RenderFlags)` as a tail call.

**No TLS write** anywhere in this function (see Delta). **No direct D3D11 immediate-context calls** — all state mutations go through RendererShadowState dirty bits (base 0x143027EB0) and are flushed later by the draw-time state applier invoked from the Draw dispatch chain.

### Exact call sequence (D3D11-relevant, in order)
1. `shader->SetupGeometry(pass, RenderFlags)` — virtual, vtbl+0x30 (per-shader; for NiParticles passes this is BSParticleShader/BSEffectShader::SetupGeometry — sets per-geometry constants/dirty bits; not expanded here, virtual and shader-dependent, same vfunc the standard path uses).
2. Alpha blend-mode selection from the geometry's NiAlphaProperty → writes RendererShadowState `alphaBlendMode @0x143027F58` (dirty 0x80) and `alphaTestEnabled @0x143027F64` (dirty 0x100). Details below.
3. If geometry has an NiAlphaProperty: alpha-test reference → writes `alphaTestRef float @0x143027F68` (dirty 0x200).
4. `FUN_141307160(pass)` — Draw dispatch (already REd; GeometryType switch at geom+0x150; particles = type 1) — this is where deferred state is flushed and DrawIndexed/Draw is issued.
5. `shader->RestoreGeometry(pass, RenderFlags)` — virtual, vtbl+0x38.

## Callees

### BSRenderPass::GetNiProperty @ 0x1412FD8A0
```c
NiAlphaProperty *GetNiProperty(BSRenderPass *a1) {
  return a1->m_Geometry->Properties_120[0];   // qword at geometry+0x120 = NiAlphaProperty*
}
```
Plain helper (not a vfunc): the NiAlphaProperty pointer lives at **geometry+0x120** (second slot of the properties[2] array at +0x118).

### SetupAlphaBlendState (idb: BSEffectShader::sub_14131F440) @ 0x14131F440
Signature: `(BSShader* shader /*unused*/, NiAlphaProperty* alphaProp, BSShaderProperty* shaderProp, char useAlphaTestBit)`.

Reads `alphaFlags = u16 @alphaProp+0x30`, `materialAlpha = float @shaderProp+0x30`. Writes three RendererShadowState globals (base **0x143027EB0** = stateUpdateFlags dword):
- `0x143027F58` (+0xA8) = alphaBlendMode (0..4); change → `stateUpdateFlags |= 0x80` (DIRTY_ALPHA_BLEND)
- `0x143027F64` (+0xB4) = alphaTestEnabled (bool); change → `stateUpdateFlags |= 0x100`
- (companion fn) `0x143027F68` (+0xB8) = alphaTestRef (float); change → `stateUpdateFlags |= 0x200`

Logic (full body quoted in-line above from decompiler; summarized precisely):
```
blendEnable = alphaProp && (alphaFlags & 1)
testEnable  = alphaProp && useAlphaTestBit && (alphaFlags & 0x200)
src = (alphaFlags >> 1) & 0xF        // NiAlphaProperty blend function enum
dstField = alphaFlags & 0x1E0        // dst function << 5

if (materialAlpha >= 1.0 && !blendEnable):        // opaque
    if (alphaBlendMode@F58 != 0) { F58 = 0; flags |= 0x80 }
    if (testEnable != alphaTestEnabled@F64) { F64 = testEnable; flags |= 0x100 }
    return
// translucent:
if (!blendEnable)                     -> mode 1    // materialAlpha < 1 forces standard blend
else if (src==6 /*SRC_ALPHA*/ && dstField==0xE0 /*INV_SRC_ALPHA*/)      -> mode 1
else if ((src==6 && dstField==0) || (src==0 && dstField==0)             // (SRC_ALPHA,ONE) / (ONE,ONE)
      ||  (src==6 && dstField==0x120 /*INV_DST_ALPHA*/))                -> mode 2   // additive
else if (src==4 /*DST_COLOR*/ && dstField==0xE0)                        -> mode 3
else if ((src==1 /*ZERO*/ && dstField==0x40 /*SRC_COLOR*/)
      ||  (src==4 && dstField==0x20 /*ZERO*/))                          -> mode 4   // multiplicative
else: leave mode unchanged
on mode change: F58 = mode; flags |= 0x80
finally: if (testEnable != F64) { F64 = testEnable; flags |= 0x100 }
```

### BSEffectShader::SetAlphaTestRef @ 0x14131F2A0
```c
threshold = byte @alphaProp+0x32;                 // NiAlphaProperty alphaTestRef 0..255
v = (int)(threshold * shaderProp->alpha_0x30);    // scaled by material alpha
if (alphaTestRef@0x143027F68 != v * (1/255.0f)) {
  stateUpdateFlags@0x143027EB0 |= 0x200;
  alphaTestRef@0x143027F68 = v * 0.0039215689f;
}
```

### FUN_141307160 @ 0x141307160 — Draw dispatch. Already REd; called here with rcx=pass, no other live args. Same call as in the standard path.

### Shader vfuncs (both virtual, per-shader implementations)
- vtbl **+0x30** = `SetupGeometry(BSRenderPass*, uint32 RenderFlags)` (confirmed by ShaderSetup 0x141309F80 tail `jmp [rax+30h]`, idb slot name `SetupGeometry_30`)
- vtbl **+0x38** = `RestoreGeometry(BSRenderPass*, uint32 RenderFlags)`
(vtbl map for reference: +0x10 SetupTechnique, +0x18 RestoreTechnique, +0x20 SetupMaterial, +0x28 RestoreMaterial.)

### BeginPass @ 0x1413086C0 (shared prologue, for completeness)
If `currentShader@0x143283BA8` non-NULL → `currentShader->RestoreTechnique(currentTechnique@0x143283BA4)` (vtbl+0x18); zero both; zero `currentMaterial@0x143490BB0`; call `newShader->SetupTechnique(Technique)` (vtbl+0x10); on success cache technique+shader into 0x143283BA4/0x143283BA8, on failure zero them and return 0.

## Delta vs standard path (0x1413088C0)

Standard path body (idb misnames it "Skinned"; it is the **non-skinned, non-custom** path per the dispatcher):
```c
tlsSlot = *(TEB->TlsPointer[ TlsIndex@0x143497408 ]) + 0x768;   // dword marker slot
saved = *tlsSlot; *tlsSlot = 26;                                 // marker value 26
shader = *pass;
alphaTestArg = AlphaTest || byte_14302C8E5;                      // global console-toggled bool
FUN_141309f80(pass, shader, alphaTestArg, RenderFlags);          // ShaderSetup: alpha setup + SetupGeometry
FUN_141307160(pass);                                             // Draw dispatch
shader->RestoreGeometry(pass, RenderFlags);                      // vtbl+0x38
*tlsSlot = saved;
```
And inside ShaderSetup 0x141309F80 (already REd): `if (shader != BSSkyShader_singleton@0x1432336C0) { if ((RenderFlags & 4) && !FUN_1412CCE20(pass->passEnum@+0x18)) SetupAlphaBlendState(shader, alphaProp, shaderProp, alphaTestArg); if (alphaTestArg && alphaProp) SetAlphaTestRef(...); }` then tail `shader->SetupGeometry(pass, RenderFlags)`. `FUN_1412CCE20(e)` = `(e - 0x5C000058) <= 3`, i.e. pass enums 0x5C000058..0x5C00005B are excluded from blend setup.

Differences of the CUSTOM path:

1. **No TLS marker**: custom never touches the per-thread marker slot (TlsIndex@0x143497408, slot offset +0x768). Standard writes 26 around setup+draw and restores it.
2. **Order inversion**: custom calls `shader->SetupGeometry(pass, RenderFlags)` **first**, then does alpha blend/test-ref setup, then draws. Standard does alpha blend/test-ref setup first and SetupGeometry last (as the tail of ShaderSetup), immediately before the draw. Net effect: in the custom path the alpha state derived from the NiAlphaProperty **cannot be overridden by SetupGeometry**; it wins over anything SetupGeometry set.
3. **AlphaTest argument ignored**: dl (AlphaTest) is clobbered at 0x141308B35 and never used. Instead:
   - SetupAlphaBlendState is called with `useAlphaTestBit = 1` **unconditionally** → the NiAlphaProperty's own test-enable bit (alphaFlags & 0x200) always decides alpha testing (standard passes `AlphaTest || byte_14302C8E5`).
   - SetAlphaTestRef is called whenever the geometry **has** an alpha property (standard additionally requires the alphaTestArg to be true).
4. **Gates skipped**: custom performs alpha setup with none of ShaderSetup's three gates — no BSSkyShader-singleton exclusion (0x1432336C0), no `(RenderFlags & 4)` requirement, no passEnum 0x5C000058..0x5C00005B blacklist.
5. **Identical**: shared dispatcher prologue (technique/material dedup, LOD byte write), Draw dispatch FUN_141307160(pass), RestoreGeometry(pass, RenderFlags) tail, and the fact that no D3D11 immediate-context vfunc is called directly — everything is dirty-bit deferred through RendererShadowState 0x143027EB0.
6. **No geometry vfuncs**: the custom path calls **zero** vfuncs on the geometry object (GetNiProperty is a non-virtual helper reading geom+0x120). There is no geometry vfunc called by custom that standard doesn't call — the only extra calls vs standard are the *direct* (ungated) invocations of SetupAlphaBlendState/SetAlphaTestRef, which standard reaches only conditionally via ShaderSetup 0x141309F80.

## CAVEATS

- The semantic meaning of the ShaderSetup passEnum blacklist range 0x5C000058..0x5C00005B (FUN_1412CCE20) was not resolved to named techniques; given the adjacent BSSkyShader-singleton exclusion it is plausibly a sky/sun technique block, but this is unverified (idb autoname "IsGrassShadowBlacklist" is also unverified).
- byte_14302C8E5 (OR'd into the standard path's alphaTest arg) is written by a console handler the idb autonames "ToggleEarlyZ"; its exact gameplay semantics (force-alpha-test when depth-prepass active?) were not verified.
- Dirty-bit naming: I report observed behavior (blend mode change→0x80, test-enable change→0x100, test-ref change→0x200 at stateUpdateFlags 0x143027EB0). Community headers sometimes name 0x100/0x200 the other way around; trust the observed mapping here.
- SetupGeometry/RestoreGeometry (vtbl+0x30/+0x38) are virtual and per-shader; I did not expand any concrete implementation since the same slots are invoked by the standard path (per task scope). For NiParticles passes the bound shader is typically BSParticleShader or BSEffectShader — which one depends on the pass, not on this dispatcher.
- The bit-3 writer search used specific x86 encodings (`80 /r disp32=0x109, imm 8` and the `F7` clear); writers using a different addressing form (e.g. dword-wide flag ops on +0x108, or reg+reg addressing) would not have been found. All found setters are NiParticles ctor/clone/factory functions; other setters may exist.
- Decompiler artifacts: the float xmm0 args on RenderPassImmediately_Custom/FUN_141307160 and the duplicated `(a1, a1)` argument are dead-register noise; disasm confirms rcx=pass is the only live argument to the Draw dispatch, and confirms r8d(RenderFlags) is live into the SetupGeometry vfunc even though the decompiler dropped it.

### Machine-extracted caveats
- passEnum blacklist range 0x5C000058..0x5C00005B not resolved to named techniques (likely sky-related; idb autoname 'IsGrassShadowBlacklist' unverified)
- byte_14302C8E5 semantics unverified (console-toggled; idb autoname ToggleEarlyZ)
- dirty-bit names for 0x100/0x200 reported from observed writes (enable->0x100, ref->0x200); community headers may name them oppositely
- SetupGeometry/RestoreGeometry vfunc implementations not expanded (virtual, per-shader, same slots as standard path)
- bit-3 writer search covered only disp32 imm-form encodings; dword-wide or reg+reg writers would be missed
- decompiler artifact args (xmm0 float, duplicated pass arg) documented and disasm-verified as dead

================================================================================================
## Cluster 2
================================================================================================

# BSUtilityShader NiBoneMatrixSetterI (bone-matrix setter) — RE report (SkyrimSE 1.5.97, image base 0x140000000)

## Vtable identification (addresses)

`BSUtilityShader::Ctor` = **0x14130DCE0** (found via xrefs to the main vtable). Body:

```c
BSUtilityShader *__fastcall BSUtilityShader::Ctor(char **a1)
{
  BSLightingShader::ctor(a1, "Utility");   // BSShader base ctor @ 0x14131F2F0
  *(a1 + 8) = 8;                            // this+0x08: shader type id = 8 (Utility)
  *a1   = 0x1418685B0;                      // this+0x00: BSUtilityShader main vftable
  a1[2] = 0x141868608;                      // this+0x10: NiBoneMatrixSetterI vftable
  a1[3] = 0x141868620;                      // this+0x18: BSReloadShaderI vftable
  unk_143495D50 = a1;                       // BSUtilityShader::pInstance global
}
```

- **BSUtilityShader main vftable**: 0x1418685B0 (this+0x00)
- **NiBoneMatrixSetterI vftable (this+0x10)**: **0x141868608**, exactly 2 entries:
  - `[0]` @0x141868608 → **0x141310758** — adjustor-thunk destructor: `BSUtilityShader::dtor_141310770(this - 0x10, deleteFlag)`. Role: virtual (vector-deleting) dtor through the NiBoneMatrixSetterI base.
  - `[1]` @0x141868610 → **0x14131F630** — **SetBoneMatrices implementation** (the +8 entry the skinned dispatcher calls). NOTE: this function is **shared, not Utility-specific** — it is referenced by 100+ vtables in the 0x141850950..0x14185xxxx range (every BSShader-family / NiBoneMatrixSetterI vtable points at the same implementation; it is effectively `BSShader::SetBoneMatrix`).
  - The qword after ([2] @0x141868618 = 0x141990B40) is **not a function**; it is the RTTI CompleteObjectLocator of the *next* vftable (0x141868620), so the NiBoneMatrixSetterI vtable has exactly the 2 entries above.
- **BSReloadShaderI vftable (this+0x18)**: 0x141868620, single entry 0x14131F800 → thunk to `BSShader::LoadShaders_14131F810(this - 0x18, streamOrPath)` (shader reload interface; out of scope).
- `unk_143495D50` = BSUtilityShader singleton pointer global.

### Call site (skinned dispatcher 0x141308970, verified at instruction level)

At 0x141308AB3–0x141308ACA (taken when `geometry->vfunc[54]` i.e. `[vtbl+0x1B0](geometry)` returns nonzero, after `BSRenderPass::FUN_141309f80(pass, shader, flag, a4)` geometry/technique setup):

```asm
lea  rcx, [rsi+10h]        ; rcx = &shader->NiBoneMatrixSetterI (this+0x10)
mov  rax, [rcx]            ; vtable 0x141868608
lea  r9,  [rbx+7Ch]        ; r9  = &geometry->world (NiTransform @ geom+0x7C)
lea  r8,  [rsp+30h]        ; r8  = stack struct (ctor FUN_140c7bad0; word @+0x3C set to 1)
mov  rdx, [rbx+130h]       ; rdx = geometry->skinInstance (NiSkinInstance* @ geom+0x130)
call qword ptr [rax+8]     ; SetBoneMatrices(iface, skinInstance, stackStruct, &geomWorld) ; 5th arg (-2) is a gs-cookie idiom, unused
```

Immediately after: `BSRenderPass::FUN_141307160(pass)` = the geometry Draw dispatch, then stack-struct dtor `FUN_140c7bb10`. The whole dispatcher ends with a tail-jump to `shader->vfunc[7]` (`[vtbl+0x38]`, RestoreGeometry).

The stack struct (0x50 bytes, ctor `FUN_140c7bad0` zeroes +0x08..+0x38, dword +0x3C, qword +0x48; dtor `FUN_140c7bb10` releases the six pointers at +0x08..+0x30): **the setter reads only the WORD at +0x3C** ("skinning enabled" gate) — the dispatcher sets it to 1 unconditionally. The non-hardware branch of the dispatcher instead fills this struct (+0: iface ptr, +8: geometry, +0x18: LOD flag = (pass+0x1E)>>7&1, +0x1C: LODMode&0x7F) and calls `skinInstance->vfunc[37]` (`[vtbl+0x128]`) — the NiSkinInstance software/partition render path, which invokes the same setter internally.

Also note `FUN_14131f7c0` (0x14131F7C0) called at dispatcher entry: zeroes **TLS+0x2A00** = the per-thread "last skin instance uploaded" cache, forcing a fresh upload for the pass (the cache then dedups repeat setter calls within the pass, e.g. per dismember partition).

## SetBoneMatrices full pseudocode

**0x14131F630** — `BSShader::SetBoneMatrix(NiBoneMatrixSetterI* this, NiSkinInstance* skin, SetterArgs* args, NiTransform* geomWorld /*unused here*/ )` (verbatim decompile, corrected against disasm):

```c
int32 __fastcall SetBoneMatrix(NiBoneMatrixSetterI *a1, NiSkinInstance *skin /*rdx*/,
                               SetterArgs *args /*r8*/, NiTransform *geomWorld /*r9*/)
{
  tls = *(TEB->TlsSlots /*gs:58h*/ + 8 * dword_143497408);   // per-thread BSGraphics block
  saved = *(uint32*)(tls + 0x768);
  *(uint32*)(tls + 0x768) = 26;                              // debug/technique marker, restored on exit

  if ( *(void**)(tls + 0x2A00) != skin                       // per-thread lastSkinInstance cache
    && args && *(uint16*)((char*)args + 0x3C) )              // gate: word @args+0x3C != 0
  {
    *(void**)(tls + 0x2A00) = skin;
    UpdateBoneMatrices_140D74F70(skin, geomWorld);           // rdx = geomWorld (see below)

    float *cur  = *(float**)((char*)skin + 0x48);            // packed 3x4 bone matrices (current)
    float *prev = *(float**)((char*)skin + 0x50);            // packed 3x4 bone matrices (previous frame)
    uint32 rows = 3 * *(uint32*)(*(char**)((char*)skin + 0x10) + 0x58); // 3 * NiSkinData->numBones

    // --- current bones -> VS b10 ---
    int64 *buf10 = GetID3D11Resource(0x143028490 /*Renderer, unused*/, rows, &mapped, 10);
        //  = pick ring CB [0x143027A08 + cursor*8], cursor=(cursor+1)&3 @0x143027A00,
        //    ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD(4), 0, &ms); mapped = ms.pData;
        //    qword_14302AC58 = cb; return &qword_14302AC58;
    memcpy_s(mapped, 16*rows, cur, 16*rows);                 // FUN_14131FF40
    if (*buf10) ctx->Unmap(*buf10, 0);                       // [vtbl+0x78]
    ctx->VSSetConstantBuffers(10, 1, buf10);                 // [vtbl+0x38]

    // --- previous bones -> VS b9 ---
    int64 *buf9 = GetID3D11Resource(0x143028490, rows, &mapped2, 9);  // next ring CB
    memcpy_s(mapped2, 16*rows, prev, 16*rows);
    if (*buf9) ctx->Unmap(*buf9, 0);
    ctx->VSSetConstantBuffers(9, 1, buf9);
  }
  *(uint32*)(tls + 0x768) = saved;
}
```

`ctx` = `*(ID3D11DeviceContext**)0x143027EA0` (the immediate/deferred context slot, aka `MEMORY[0x1430261B0][926]`).

### Helper: bone palette build — 0x140D74F70 `NiSkinInstance::UpdateBoneMatrices(skin, NiTransform* geomWorld)`

(IDA shows 1 arg but rdx **is** consumed as `source1` = geomWorld.)

```c
EnterCriticalSection(&skin->lock_60);                        // skin+0x60 CRITICAL_SECTION
if (skin->frameId_38 != dword_14302C8DC)                     // global frame counter → once per frame
{
  skin->frameId_38 = dword_14302C8DC;
  n     = skinData->numBones;                                // *(skin+0x10) + 0x58, uint32
  cur   = skin->boneMatrices_48;  prev = skin->prevBoneMatrices_50;
  if (48*n <= skin->allocatedSize_44)
      memcpy_s(prev, 48*n, cur, 48*n);                       // save last frame's palette FIRST
  else {                                                     // (re)allocate both, 16-aligned, 48*n bytes
      free(cur);  cur  = skin->boneMatrices_48     = alloc(48*n, 16);
      free(prev); prev = skin->prevBoneMatrices_50 = alloc(48*n, 16);
      skin->allocatedSize_44 = 48*n;  reallocated = 1;
  }
  skin->numMatrices_3C = n;  skin->numRegistersPerMatrix_40 = 3;

  // (dead in this build) tmp = Invert(rootParent_20->world_7C);         FUN_14039a980
  //                      tmp = skinData->rootParentToSkin_18 * tmp;     Multiply(a,out,b): out=a*b
  //                      v17 = geomWorld * tmp;   // v17 never read afterwards
  NiTransform **boneWorlds = skin->boneWorldTransforms_30;   // array of NiTransform* (one per bone)
  BoneData *bd = skinData->boneData_50;                      // stride 0x58; +0 = NiTransform skinToBone
  float *dst = cur;
  for (i = 0; i < n; ++i, bd = (char*)bd + 0x58, ++boneWorlds, dst += 12) {
    if (*boneWorlds) {
      NiTransform m;  Multiply_1402AB750(*boneWorlds, &m, &bd->skinToBone);  // m = boneWorld_i * skinToBone_i
      s = m.scale;
      dst[0]=m.rot[0]*s; dst[1]=m.rot[1]*s; dst[2] =m.rot[2]*s; dst[3] =m.pos.x;   // row 0
      dst[4]=m.rot[3]*s; dst[5]=m.rot[4]*s; dst[6] =m.rot[5]*s; dst[7] =m.pos.y;   // row 1
      dst[8]=m.rot[6]*s; dst[9]=m.rot[7]*s; dst[10]=m.rot[8]*s; dst[11]=m.pos.z;   // row 2
    }                                                        // NULL bone => 48 bytes left stale
  }
  if (reallocated) memcpy_s(prev, 48*n, cur, 48*n);          // first frame: prev = cur
}
LeaveCriticalSection(&skin->lock_60);
```

`NiTransform::Multiply_1402AB750(a, out, b)`: `out.rot = a.rot*b.rot` (FUN_140185e80), `out.pos = a.rot*b.pos*a.scale + a.pos`, `out.scale = a.scale*b.scale` — i.e. `out = a ∘ b` (b applied first). `FUN_14039a980(t, out)` = NiTransform inverse (rot transposed, scale reciprocal, pos = -Rᵀ·t/s).

### Helper: 0x140D6FFD0 `GetID3D11Resource(renderer /*ignored*/, rowCount /*ignored*/, void** outMapped, int which)`

```asm
cmp r9d,7 ; jz  → rdx = [0x143027E90]                        ; which==7: dedicated 16-byte CB ([924])
else:       ecx = dword [0x143027A00]                        ; ring cursor 0..3
            rdx = [0x143027A08 + rcx*8]                      ; ring of 4 dynamic CBs ([779..782])
            [0x143027A00] = (ecx+1) & 3
rcx = [0x143027EA0] ; ctx                                    ; qword_14302AC58 = rdx (global scratch)
ctx->Map(rdx, 0, 4 /*WRITE_DISCARD*/, 0, &ms)                ; [vtbl+0x70]
*outMapped = ms.pData ; return &qword_14302AC58              ; caller binds via this global's address
```

`which` (10/9) is otherwise ignored — both bone uploads draw from the **same shared ring**, consuming 2 of the 4 ring slots per skinned upload.

### Ring buffer creation — 0x140D720D0 (renderer init)

The 4 ring CBs: `device(0x143025F08)->CreateBuffer` ([devvtbl+0x18]) with `{ByteWidth=3840, Usage=D3D11_USAGE_DYNAMIC(2), BindFlags=D3D11_BIND_CONSTANT_BUFFER(4), CPUAccessFlags=D3D11_CPU_ACCESS_WRITE(0x10000), MiscFlags=0, StructureByteStride=0}` → stored at 0x143027A08/10/18/20. **3840 bytes = 240 float4 = 80 bones × 3 rows** (matches `Bones[240]`/`PreviousBones[240]` in the shaders). Same function creates the size-16 CB at 0x143027E90 ([924], `which==7`) and the per-size CB pools at [783], [785..], [805..], [815..], [843..], [863..], [883..], plus 576-byte [922] and 720-byte [923].

## D3D11 command sequence

Per skinned pass (when `lastSkinInstance(TLS+0x2A00) != skin` and `args->word_3C != 0`), in exact order, all on `ctx = *(0x143027EA0)`:

1. `Map(ringCB[c], 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)` — vfunc 14 (+0x70); ring cursor c→c+1&3.
2. CPU `memcpy_s(ms.pData, 48*numBones, skin->boneMatrices_48, 48*numBones)`.
3. `Unmap(ringCB[c], 0)` — vfunc 15 (+0x78), skipped only if the buffer ptr is NULL.
4. `VSSetConstantBuffers(StartSlot=10, 1, &ringCB[c])` — vfunc 7 (+0x38). → **b10 = current bones** (`BonesBuffer`).
5. `Map(ringCB[c+1], 0, D3D11_MAP_WRITE_DISCARD, 0, &ms2)`.
6. CPU `memcpy_s(ms2.pData, 48*numBones, skin->prevBoneMatrices_50, 48*numBones)`.
7. `Unmap(ringCB[c+1], 0)`.
8. `VSSetConstantBuffers(StartSlot=9, 1, &ringCB[c+1])` — → **b9 = previous-frame bones** (`PreviousBonesBuffer`).

No PS binds, no other state touched. Bindings persist across subsequent passes; the TLS cache (reset at each dispatcher entry by 0x14131F7C0) prevents duplicate uploads for the same skin instance within one pass. `ppConstantBuffers` for both binds points at the global scratch `qword_14302AC58` (0x14302AC58), overwritten per GetID3D11Resource call.

## Data layout (bone palette format, offsets)

**Palette format**: per bone, **3×float4 rows = 48 bytes**, row-major top-3-rows of the 4×4 world matrix, translation in `.w`:
- row0 = `(R00·s, R01·s, R02·s, T.x)`, row1 = `(R10·s, R11·s, R12·s, T.y)`, row2 = `(R20·s, R21·s, R22·s, T.z)`
- `M_i = boneWorld_i ∘ skinToBone_i` (bind-pose inverse composed into bone world; convention `v_world = M · v_skin` — each output row is dotted with `float4(pos,1)`), uniform scale premultiplied into the rotation, translation NOT scaled (already world). NiMatrix3 `Data[3r+c]` is row-major here (Multiply's position transform uses Data[0..2] as row 0). Full world-space (no eye-relative adjustment at this stage). CB capacity: **80 bones max** (3840-byte CB); memcpy_s dst-size equals src-size, so >80 bones is not guarded by the buffer size.

**NiSkinInstance** (this binary):
- +0x10 `NiPointer<NiSkinData> skinData`
- +0x20 `NiAVObject* rootParent`
- +0x30 `NiTransform** boneWorldTransforms` (array of per-bone NiTransform*, may contain NULLs)
- +0x38 `uint32 frameId` (vs global frame counter `dword_14302C8DC`)
- +0x3C `uint32 numMatrices`, +0x40 `uint32 numRegistersPerMatrix` (=3), +0x44 `uint32 allocatedBytes`
- +0x48 `float* boneMatrices` (current, 16-aligned heap), +0x50 `float* prevBoneMatrices`
- +0x60 `CRITICAL_SECTION lock`

**NiSkinData**: +0x18 `NiTransform rootParentToSkin` (used only in the dead prologue), +0x50 `BoneData* boneData` (stride **0x58**, `NiTransform skinToBone` at +0), +0x58 `uint32 numBones`.

**Geometry/pass** (call-site): skinInstance @ geom+0x130, world NiTransform @ geom+0x7C, LOD byte @ pass+0x1E.

**Globals**: TLS index dword @0x143497408; TLS block: +0x768 marker dword (saved/set 26/restored), +0x2A00 lastSkinInstance. Renderer-data statics: 0x143027A00 ring cursor, 0x143027A08 ring CB[4] (3840B), 0x143027E90 16B CB, 0x143027EA0 ID3D11DeviceContext*, 0x143025F08 ID3D11Device*, 0x14302AC58 scratch "current CB" qword, 0x14302C8DC frame counter, 0x143495D50 BSUtilityShader instance.

## CAVEATS

- The bone setter 0x14131F630 is the **shared BSShader implementation** (100+ vtables point at it, xref list truncated at 100), so BSUtilityShader does not override it; behavior is identical for Lighting etc.
- The `which==7` path of GetID3D11Resource and the meaning of TLS+0x768 = 26 (debug/annotation marker id) were not chased further; the value is saved/restored and has no D3D side effect in this function.
- The prologue composition in 0x140D74F70 (`geomWorld * rootParentToSkin * inv(rootParentWorld)` into a stack temp) is computed but **never consumed** in this build as decompiled — likely vestigial; flagged rather than guessed. Consequently the `geomWorld` (r9 = geom+0x7C) argument has **no effect** on the uploaded palette here.
- `Multiply(a,out,b) = a∘b` operand order was inferred from its position math (out.pos = a.rot·b.pos·a.s + a.pos); the rotation sub-helper FUN_140185E80 was not decompiled (assumed consistent).
- NULL entries in boneWorldTransforms leave that bone's 48 bytes **stale** (unwritten) in the palette.
- CommonLibSSE names `NiSkinInstance::bones` at +0x30 as `NiTransform**`; matches the binary. What sits at +0x28 was not examined.
- Whether geometry vfunc 54 (`[vtbl+0x1B0]`, the HW-skinning gate in the dispatcher) is `GetSkinPartition`-like was not verified; only its gating role is asserted.
- The word gate `args+0x3C` is set to 1 unconditionally by the utility skinned dispatcher; other callers (NiSkinInstance vfunc 37 path) may pass 0 to skip the upload — not traced.

### Machine-extracted caveats
- SetBoneMatrix 0x14131F630 is the shared BSShader-wide implementation (100+ vtables reference it), not a BSUtilityShader-specific override.
- The prologue composition in 0x140D74F70 (geomWorld * rootParentToSkin * inv(rootParentWorld)) is computed into a stack temp that is never read — apparently dead; therefore the geomWorld argument (geom+0x7C) has no effect on the uploaded palette in this build.
- NiTransform::Multiply operand order (out = a applied-after b) inferred from its position math; the 3x3 rotation sub-helper FUN_140185E80 was not decompiled.
- NULL bone pointers in skinInstance+0x30 leave that bone's 48 bytes stale (unwritten) in the palette.
- Meaning of TLS+0x768 marker value 26 and the which==7 GetID3D11Resource path were not chased further.
- Geometry vfunc 54 ([vtbl+0x1B0]) gating the HW-skinned branch was not identified beyond its gating role; the alternate branch (NiSkinInstance vfunc 37, [vtbl+0x128]) software/partition path was not traced.
- memcpy_s destination size equals source size (48*numBones), so bone counts >80 are not guarded against the 3840-byte ring CB capacity by this code.

================================================================================================
## Cluster 3
================================================================================================

# STENCIL_ABOVE_WATER utility technique flow (F = passEnum-0x2B, (F & 0x1200) == 0x1200) — SkyrimSE 1.5.97

Function-role correction first (vtable order matters for hooking): `0x14130DF90` = SetupTechnique (Func2), `0x14130DD80` = **RestoreTechnique** (Func3, takes the technique enum), `0x14130EC70` = SetupGeometry (Func6, takes BSRenderPass*), `0x141310300` = **RestoreGeometry** (Func7, takes BSRenderPass* — the task prompt called this one "RestoreTechnique"; it is the per-pass restore). Both restores participate in the 0x1200 protocol and are documented below.

Struct layouts used throughout (confirmed against `Renderer::SetPixelShader 0x140d6fd60`, `Renderer::SetVertexShader 0x140d6f9b0`, and the Map/Unmap sites):

- `BSGraphics::PixelShader`: `+0` techniqueId(u32), `+8` **ID3D11PixelShader\* m_Shader**, `+0x10` PerTechnique{ID3D11Buffer\* buf, void\* data}, `+0x20` PerMaterial, `+0x30` PerGeometry{buf@0x30, data@0x38}, `+0x40` u8 constantOffsets[].
- `BSGraphics::VertexShader`: `+0` techniqueId, `+8` ID3D11VertexShader\* m_Shader, `+0x18` PerTechnique{buf@0x18, data@0x20}, `+0x38` PerGeometry{buf@0x38, data@0x40}, `+0x50` u8 constantOffsets[].
- Globals: `0x1430281F8` = m_CurrentVertexShader (VertexShader\*), `0x143028200` = m_CurrentPixelShader (PixelShader\*) — both inside the `dword_143028070` render-state block (`0x143028070 + 98*4` / `+100*4`). `0x143027EA0` = stored ID3D11DeviceContext\* used for shader CB Map/Unmap/SetConstantBuffers; `0x1430261B0[926]` = the same immediate context used by SetDirtyStates; `0x1430261B0` = renderer device-objects block (precreated state arrays). `0x143027EB0` = m_StateUpdateFlags (dirty bits). BSUtilityShader stores the current raw technique F at `this+0x90` (`_pad_20[112]`) and `F & 0x7F` at `this+0x94`, written in SetupTechnique at 0x14130e0e4.

## FUN_140d6fcf0 semantics

Full decompilation (entire function; first arg `&renderer 0x143028490` is **unused**):

```c
int32 __fastcall FUN_140d6fcf0(int64 a1_renderer_unused, int64 a2_pixelShader)
{
  if ( a2_pixelShader ) {
    v2 = *(a2_pixelShader + 8);           // ID3D11PixelShader* m_Shader
    if ( v2 ) {
      (*(*v2 + 16LL))(v2);                // vtbl+0x10 = IUnknown::Release  (QI=0, AddRef=8, Release=16)
      *(a2_pixelShader + 8) = 0;          // m_Shader = nullptr
    }
  }
}
```

Disasm confirms: `mov rcx,[rdx+8]; test rcx,rcx; jz; mov rax,[rcx]; call [rax+10h]; mov qword [rbx+8],0`. It does **not** Release-then-swap, does **not** null-bind — it is a **one-shot destructive Release of the D3D pixel-shader COM object owned by the passed PixelShader cache entry, followed by nulling the entry's m_Shader field**. It does NOT touch the context (no `PSSetShader(NULL)` is issued) and does not touch `0x143028200` itself (the PixelShader\* stays current; only its `+8` becomes null).

**Ownership semantics**: the PixelShader table entry (in `BSUtilityShader`'s pixel-shader hash table, buckets at shader+0x80, walked by BeginTechnique) holds exactly one reference to the ID3D11PixelShader, created at shader-load time. FUN_140d6fcf0 consumes that single owned reference. Because the field is nulled in the same guarded block, the function is **idempotent**: a second call on the same entry is a no-op. There is no AddRef anywhere in this flow.

**Only caller in the whole binary**: `BSUtilityShader::SetupGeometry` at 0x14130f3a5 (the other xref, 0x143571200, is just this function's .pdata RUNTIME_FUNCTION entry). The call site: `FUN_140d6fcf0(&MEMORY[0x143028490], v2)` where `v2 = MEMORY[0x143028200]` was **captured at SetupGeometry entry** (0x14130ec86) — i.e. the technique's own PS entry that BeginTechnique just made current.

**Net effect / why the engine does this**: the (F&0x1200)==0x1200 utility technique wants no pixel shader (stencil-only marking with color writes disabled). Instead of binding null, the engine permanently destroys the compiled PS for this technique's psid the first time a 0x1200 pass runs. Timing wrinkle (verified from code order, not runtime): on the **first-ever** execution after shader load, BeginTechnique has already issued `PSSetShader(ps->m_Shader)` (real object; the context's own ref keeps it alive), and the Release happens after that bind — so the first draw executes with the real PS bound; from the next BeginTechnique onward `PSSetShader(*(ps+8)=NULL)` binds null. Steady state = null PS.

### Exact SetupGeometry 0x1200 branch (0x14130f318..0x14130f3a5)

Reached only when `(F & 0x20004000) != 0x4000` (true for F=0x1200) and the grayscale path `(F & 0x100000)` was not taken. `v47` = dirty flags, `v49` = alphaBlendWriteMode (0x143027F60) read earlier, `v41` = depthModePrevious (0x143027F3C) read at 0x14130f1ab:

```c
if ( (F & 0x1200) == 0x1200 ) {
  if ( unk_143027F40 != 0xFF00000001LL ) {          // {stencilMode=1, stencilRef=0xFF}
    v47 |= 8;  unk_143027F40 = 0xFF00000001LL;  dirty = v47;      // DIRTY_DEPTH_STENCILREF_MODE
  }
  if ( v49 ) {                                       // alphaBlendWriteMode != 0
    v47 |= 0x80;  unk_143027F60 = 0;  dirty = v47;   // color writes OFF, DIRTY_ALPHA_BLEND
  }
  if ( unk_143027F38 ) {                             // depthMode != 0
    unk_143027F38 = 0;                               // depth mode 0 (disabled)
    dirty = (unk_143027F3C_prev != 0) ? (v47|4) : (v47 & ~4);     // DIRTY_DEPTH_MODE compare-vs-previous
  }
  *(vsPerGeoMapped + 4 * vs->constantOffsets[7]) = dword_141E0E014;   // raw dword copy
  FUN_140d6fcf0(&renderer_143028490, currentPS_143028200);            // Release + null ps->m_Shader
}
```

`dword_141E0E014` is a float in [0,1] (statically 0), recomputed by `FUN_1404c5660` (the imagespace water/DOF update, which also drives ImageSpaceEffectDepthOfField mode selection and `qword_141E0DFF4+4`): 0 when two water bools at watersystem+0xB8/+0xB9 are clear, else `clamp01((val@+0xC0 − a)/(b − a))`, or 1.0f (0x3F800000) — a camera-vs-water-plane blend factor. It is copied bit-for-bit into the utility VS PerGeometry cbuffer (b2) at float index `constantOffsets[7]` (byte at VertexShader+0x57).

Full per-pass D3D11 call order for a pure-0x1200 pass through SetupGeometry (ctx = `[0x143027EA0]`):
1. `ctx->Map(vs->PerGeometry.buf /*vs+0x38*/, 0, D3D11_MAP_WRITE_DISCARD(4), 0, &p)`; mapped ptr → vs+0x40 (0x14130ecdf)
2. `ctx->Map(ps->PerGeometry.buf /*ps+0x30*/, 0, DISCARD, 0, &p)`; → ps+0x38 (0x14130ed26; executed because currentPS pointer ≠ null — stays true even after m_Shader was nulled)
3. non-skinned (F bit 2 clear): world→view matrix built by FUN_1412c3440 + `D3DXMatrixTranspose` into vsPerGeo at `4*constantOffsets[0]` (0x14130ef1f); skinned bit skips this
4. the 0x1200 shadow-state writes + CB write + FUN_140d6fcf0 (above)
5. tree-anim block skipped (needs F&0x4000000); `BSRenderPass::GetNiProperty(pass)` (0x1412fd8a0) → alpha gate: since currentPS≠null, if (no NiAlphaProperty ∨ !(alphaFlags&1)) ∧ (F&0x80)==0 ∧ (F&0x14000)!=0x10000 → jump straight to the tail. (If the geometry has an alpha-blending NiAlphaProperty or F has AlphaTest 0x80, the PS alpha-ref constants at `4*ps->constantOffsets[0]` get written first — gate at 0x14130f74e/0x14130f764.)
6. tail (LABEL_154, 0x14130f8a6): `ctx->Unmap(vs->PerGeometry.buf, 0)`; `ctx->Unmap(ps->PerGeometry.buf, 0)`; `ctx->VSSetConstantBuffers(2, 1, &vs->PerGeometry.buf)`; `ctx->PSSetConstantBuffers(2, 1, &ps->PerGeometry.buf)`.

The shadow-state writes are materialized later by `BSGraphics::SetDirtyStates (0x140d705b0)` before the DrawIndexed: dirty&0xC → `ctx->OMSetDepthStencilState(deviceBlock_1430261B0[40*depthMode_143027F38 + stencilMode_143027F40], stencilRef_143027F44)`; dirty&0x80 → `ctx->OMSetBlendState(deviceBlock[384 + 52*u143027F58 + 26*u143027F5C + 2*writeMode_143027F60 + dw_143028484], blendFactor_141E07168, 0xFFFFFFFF)`.

## Restore protocol

Two layers, split between per-pass RestoreGeometry and per-technique RestoreTechnique. The 64-bit global `unk_143027F40` is the packed pair `{u32 stencilMode @0x143027F40, u32 stencilRef @0x143027F44}` of RendererShadowState (offsets +0x90/+0x94 from 0x143027EB0); dirty bit 8 forces re-issue of OMSetDepthStencilState.

**RestoreGeometry 0x141310300** (a2 = BSRenderPass\*, `a2+28` = pass+0x1C = accumulationHint; v2 = stored F from shader+0x90):

```c
if ( (v2 & 0x1200) != 0x1200 && pass->accumulationHint == 10 && unk_143027F40 != 0xFF00000000LL ) {
  dirty |= 8;  unk_143027F40 = 0xFF00000000LL;      // {stencilMode=0, ref=0xFF}
}
```

This branch is the undo of a *different* stencil use: in SetupGeometry's shadow-mask path (F&0x1E00000, at 0x14130f126..0x14130f18e) a pass with accumulationHint==10 (LOD-fade dither) sets `{mode=11, ref=(fade*31)}`. RestoreGeometry resets that to `{mode=0, ref=0xFF}` after each such pass. The `(F & 0x1200) != 0x1200` gate deliberately **excludes** the STENCIL_ABOVE_WATER technique from this per-pass reset: its `{1, 0xFF}` must persist across all passes of the technique block (per-pass resetting would thrash dirty-bit 8 / OMSetDepthStencilState between every pass, and the hint==10 case can't co-occur meaningfully). So for 0x1200 passes RestoreGeometry leaves stencil state alone. (Its remaining body: the F&0x100000 grayscale raster/depth restore, and the `dword_141E10660` blend-write-mode stash restore — `dword_141E10660` is statically 13 = sentinel "nothing stashed", set only by the grayscale+blockOutTexture path in SetupGeometry at 0x14130f292 and reset to 13 after restore; a pure 0x1200 pass never touches it, so that block no-ops.)

**RestoreTechnique 0x14130DD80** is where the 0x1200 state is actually undone, once, at technique end (final check reads the stored F at shader+0x90, at 0x14130def2):

```c
if ( (this->rawTechnique_0x90 & 0x1200) == 0x1200 ) {
  if ( unk_143027F40 != 0xFF00000000LL ) { dirty |= 8;    unk_143027F40 = 0xFF00000000LL; }  // stencilMode 0, ref 0xFF
  if ( unk_143027F60 != 1 )              { dirty |= 0x80; unk_143027F60 = 1; }               // alphaBlendWriteMode back to default 1
}
FUN_14131fce0();   // BSShader::EndTechnique — verified EMPTY function (no-op)
```

Note the depth mode (0x143027F38, forced to 0 by SetupGeometry) is **not** restored by either function for the 0x1200 technique — it leaks until the next technique's Setup writes it (every utility/lighting Setup sets it explicitly, so this is benign engine-wide but a replica that owns the pass must reproduce the leak to stay state-identical). The stencilRef protocol overall: ref stays 0xFF at all times for this technique; only the mode toggles 1→0. `pass->accumulationHint == 10` never gates anything in the 0x1200 flow itself — it only gates the *other* (fade-dither, mode 11) protocol which the 0x1200 gate in RestoreGeometry explicitly bypasses.

## Technique-ID impact

`SetupTechnique 0x14130DF90`: `F = passEnum - 43`; `vsid = FUN_141334900(F)`; `psid = FUN_141334970(F)`; then `BeginTechnique(this, vsid, psid, noPS = !v6)` where

```
v6 = (F & 0x14000) == 0x14000
   || ((F & 0x20004000) != 0x4000 && (F & 0x1E02000) != 0x2000)
   || (F & 0x80) != 0
   || (F & 0x14000) == 0x10000;
```

For any F with (F&0x1200)==0x1200 and without 0x4000/0x2000: the second clause is true → **v6 = true → a4(noPS) = false → the PS is looked up in the table and is REQUIRED** (BeginTechnique returns 0 and the pass is skipped if either VS or PS entry is missing). So this technique is *not* dispatched as a null-PS technique — it binds a real PS entry whose D3D object then gets destroyed by SetupGeometry.

**vsid reducer FUN_141334900**: `vsid = F & 0xF7E5FF9F` (clears bits 0x081A0060 = 27,20,19,17,6,5). The shadow-mask override (`byte_141E0DE4C && (F&0x1E00000)`→`(F&0x1E00000)|0x2002`) does not apply (F&0x1E00000==0). The second reduction (`v1 &= 0xDFFFE1E4` — the depth/shadowmap group collapse) requires `((F&0x20004000)==0x4000 || (F&0x1E02000)==0x2000)` which is false for 0x1200. → **vsid keeps both 0x200 and 0x1000 bits intact** (plus vertex-format bits 0..4,7,8): a dedicated RENDER_NORMAL|RENDER_NORMAL_CLEAR vertex shader is selected.

**psid reducer FUN_141334970**: the early `return 0x2000` collapse has the same false precondition. → `psid = F & 0xFFFFFB83` (clears 0x47C = bits 2,3,4,5,6,10 — Skinned/Normals/BinormalTangent/bits5-6 and RenderNormalFalloff), and the `(F&0x1E00000)` shadow-mask override doesn't apply. → **psid keeps 0x1200** (plus bits 0,1,7). So the released pixel shader is the dedicated cache entry `psid = F & 0xFFFFFB83`; all skinned/vertex-format variants of the technique share that one PS entry, and no non-0x1200 technique can map onto it (bits 9 and 12 are never cleared by the reducer for the non-shadow group).

BeginTechnique 0x14131FBD0 itself: hash-walks the VS table (buckets at shader+0x50, size at +0x34) for vsid and the PS table (buckets at +0x80, size at +0x64) for psid, then `Renderer::SetShader(0x140d6f9b0)` — sets dirty 0x400 (input-layout re-resolve), stores VS ptr to 0x1430281F8, **immediately** `ctx->VSSetShader(vs->m_Shader,0,0)` — and `Renderer::SetPixelShader(0x140d6fd60)` — stores PS ptr to 0x143028200, immediately `ctx->PSSetShader(ps ? ps->m_Shader : 0, 0, 0)`. No refcounting on either side. After BeginTechnique succeeds, SetupTechnique maps VS PerTechnique (vs+0x18) and (v6) PS PerTechnique (ps+0x10) with DISCARD, and for pure 0x1200 (F&0x1E00100 != 0x100, F&0x1E00000 == 0, F&0x40000 == 0, F&0x20004000 != 0x4000, F&0x100000 == 0) **writes nothing into either**, then Unmaps both and binds them: `ctx->VSSetConstantBuffers(0,1,&vs->PerTechnique.buf)`, `ctx->PSSetConstantBuffers(0,1,&ps->PerTechnique.buf)` (0x14130e6f4/0x14130e70a). (Per-technique b0 content is therefore DISCARD garbage — engine behavior; the selected shaders evidently don't read it.)

## Double-render hazard + safe compensation

**If both runs execute the engine's own code paths (call-original twice)**: nothing double-Releases. FUN_140d6fcf0 is self-guarded (`if (m_Shader) { Release; m_Shader = null; }`), so the replica's second pass finds `m_Shader == null` and no-ops. All other side effects in Setup/Restore are value-idempotent compare-and-write shadow-state updates plus DISCARD-mapped CB rewrites of identical bytes. The only cross-run asymmetry is the **first-ever execution of the technique per shader-lifetime**: run 1 draws with the real PS bound (bound by BeginTechnique before the Release), run 2's BeginTechnique binds NULL (m_Shader already nulled) — a potential one-frame visual/compare diff, and with F&0x80 (alpha-test, psid keeps bit 7) a real stencil-output diff since the real PS's `clip()` would be lost. From the second execution on, both runs bind null and are byte-identical.

**What actually double-Releases** — the hazard is entirely in a *re-implementation*: the object at risk is the **ID3D11PixelShader** owned by the utility PixelShader cache entry `psid = F & 0xFFFFFB83` (field `+8` of the struct pointed to by `[0x143028200]`). Failure modes:
1. Replica caches `ID3D11PixelShader* s = ps->m_Shader` (e.g. at SetupTechnique time) and later Releases `s` unconditionally, or Releases without also nulling `ps->m_Shader` → engine (or the replica's next frame) Releases again → refcount underflow: the second Release consumes the D3D context's bind-time reference, so the object is destroyed while still recorded in the pipeline binding table → use-after-free on the next PSSetShader/draw (crash in driver/DXVK).
2. Replica (or CS ShaderCache-style hook) **re-populates** `ps->m_Shader` for this psid each frame without an AddRef per store → the engine's SetupGeometry Releases a reference the installer never owned, once per technique execution → underflow of the installer's object.

**Safe compensation for engine-then-replica compare mode** (making both runs identical *including* the first-frame window):

```cpp
// BEFORE the engine executes SetupTechnique/SetupGeometry for the 0x1200 pass:
auto* ps    = *(PixelShader**)0x143028200_after_BeginTechnique;   // or the psid entry from the table
auto* saved = ps->m_Shader;            // ID3D11PixelShader*
if (saved) saved->AddRef();            // take one extra owned reference
// ... engine runs: binds saved (if first time), Releases it, nulls the field ...
ps->m_Shader = saved;                  // transfer YOUR reference back into the entry (null after first frame)
// ... replica runs the identical path: BeginTechnique binds it again, SetupGeometry
//     Releases (consuming your AddRef) and nulls the field again.  Net refcount: balanced.
```

Each of the two Releases consumes exactly one owned reference; behavior (real-PS-then-null timeline) is identical for both runs on every frame. If first-frame identity is not needed, the zero-cost alternative is: replica simply re-runs the original functions (or an exact copy that keeps the guarded release against the **live** `[0x143028200]->m_Shader`, never a cached copy) — no compensation, accepting the one-time null-vs-real PS divergence. If the replica *owns* the pass exclusively (engine path suppressed), it must reproduce all of: stencil {1,0xFF} + writeMode 0 + depthMode 0 with correct dirty bits (8 / 0x80 / 4-with-prev-compare), the `constantOffsets[7] = dword_141E0E014` VS b2 write, the guarded release-and-null, the b2 rebinds, and RestoreTechnique's {0,0xFF} + writeMode 1 — while leaving depthMode 0 un-restored.

### Key addresses

| Item | Address |
|---|---|
| BSUtilityShader::SetupTechnique (Func2) | 0x14130DF90 |
| BSUtilityShader::RestoreTechnique (Func3) | 0x14130DD80 (0x1200 undo at 0x14130def2) |
| BSUtilityShader::SetupGeometry (Func6) | 0x14130EC70 (0x1200 branch 0x14130f318, release call 0x14130f3a5) |
| BSUtilityShader::RestoreGeometry (Func7) | 0x141310300 (0x1200-gated stencil reset 0x14131037e) |
| FUN_140d6fcf0 (release current-PS D3D object) | 0x140d6fcf0 — single code caller |
| BSShader::BeginTechnique | 0x14131FBD0; EndTechnique 0x14131fce0 = empty |
| Renderer::SetVertexShader / SetPixelShader | 0x140d6f9b0 / 0x140d6fd60 |
| vsid / psid reducers | 0x141334900 (`F & 0xF7E5FF9F`) / 0x141334970 (`F & 0xFFFFFB83`) |
| BSGraphics::SetDirtyStates (flush) | 0x140d705b0 (DSS apply 0x140d707c9, blend apply 0x140d70913) |
| m_StateUpdateFlags / depthMode / depthModePrev | 0x143027EB0 / 0x143027F38 / 0x143027F3C |
| stencilMode / stencilRef (packed qword) | 0x143027F40 / 0x143027F44 |
| alphaBlendWriteMode / stash sentinel(=13) | 0x143027F60 / 0x141E10660 |
| currentVS / currentPS / CB-context / device block | 0x1430281F8 / 0x143028200 / 0x143027EA0 / 0x1430261B0 |
| water blend factor (VS b2, constantOffsets[7]) | 0x141E0E014, written by FUN_1404c5660 |

## CAVEATS

- The exact D3D11_DEPTH_STENCIL_DESC behind stencilMode 1 vs 0 (and depthMode 0) was not re-derived — the device-init state-creation loop was not decompiled. The flush indexes precreated states as `deviceBlock_1430261B0[40*depthMode + stencilMode]` with `ref = [0x143027F44]`; a replica should index/bind the engine's own precreated array (or reuse CommonLib's enum mapping) rather than trusting a guessed desc.
- The blend-state index formula `384 + 52*[0x143027F58] + 26*[0x143027F5C] + 2*writeMode + [0x143028484]` implies writeMode range 0..12 (sentinel 13); writeMode 0 = no color writes / 1 = default was inferred from usage (0 set by stencil/grayscale-off paths, 1 restored as default), not from the creation desc.
- The pass-creation site for STENCIL_ABOVE_WATER (passEnum 0x2B + 0x1200 = 0x122B, presumably BSWaterShaderProperty/water-stencil accumulation) was not located — the immediate-search timed out and it wasn't required. Analysis assumes F = 0x1200 plus optional vertex-format bits (0..4) and possibly 0x80 (alpha test); every gate was evaluated symbolically so extra low bits don't change any conclusion except the noted alpha-test first-frame clip() caveat.
- `dword_141E0E014` was characterized from FUN_1404c5660's disasm (clamp01 blend factor derived from water-system fields +0xB8/+0xB9 bools and +0xC0 float; 0 when both bools clear, 1.0 in the far branch); the precise gameplay meaning of those fields (camera above/below water) was not verified further.
- "First draw after shader (re)load executes with the real PS bound, later draws with null PS" is proven from code order (PSSetShader in BeginTechnique precedes the Release in SetupGeometry; D3D bind holds its own reference) but was not runtime-verified with a capture.
- BSUtilityShader member offsets 0x90 (stored raw F) / 0x94 (F&0x7F) derive from the decompiler's `_pad_20[112]/[116]` with pad base 0x20; consistent with BSShader size 0x90 but not independently typed in this idb.

### Machine-extracted caveats
- Exact D3D11_DEPTH_STENCIL_DESC for stencilMode 1/0 and depthMode 0 not re-derived; flush indexes precreated states deviceBlock_1430261B0[40*depthMode + stencilMode] with ref from 0x143027F44 - replica should reuse the engine array or CommonLib enums
- Blend writeMode 0 = no color writes / 1 = default inferred from usage and index formula (weight 2, sentinel 13), not from the creation desc
- Pass-creation site for passEnum 0x122B (STENCIL_ABOVE_WATER) not located (immediate search timed out); F assumed 0x1200 + optional vertex-format bits, gates evaluated symbolically
- dword_141E0E014 characterized as clamp01 water blend factor from FUN_1404c5660 disasm; exact meaning of source fields (+0xB8/+0xB9/+0xC0 of the water object) not fully verified
- First-execution window (real PS bound for the first draw, null thereafter) proven from code order, not runtime-captured; matters for compare mode and for alpha-test (F&0x80) variants whose PS clip() affects stencil output
- BSUtilityShader offsets 0x90/0x94 (stored raw technique F, F&0x7F) derived from decompiler pad offsets, not independently typed

================================================================================================
## Cluster 4
================================================================================================

# RE: Skinned render-pass dispatcher @ 0x141308970 (SkyrimSE.exe 1.5.97)

NOTE ON LABELS: the idb names 0x141308970 `RenderPassImmediately_Standard`, but this is the **SKINNED** dispatcher (reached from BSBatchRenderer::RenderPassImmediately 0x141308440 when `geometry+0x130` (skinInstance) != 0). The idb labels for 0x141308970/0x1413088C0 are swapped vs reality.

## Dispatcher body

Signature (real): `void RenderPassImmediately_Skinned(BSRenderPass* pass /*rcx*/, uint8 alphaTest /*dl*/, uint32 renderFlags /*r8d*/)`. The decompiler's `a4@<xmm0>` is spurious (never read).

Annotated decompile (0x141308970):

```c
v5 = (uint8)a2;                 // dl: alpha-test/technique bool, forwarded to SetupGeometry helper
v7 = *(pass + 0x10);            // rbx = geometry (BSGeometry*)
v8 = *(pass + 0x00);            // rsi = shader   (BSShader*)
v9 = *(geometry + 0x130);       // r15 = skinInstance (NiSkinInstance*) — loaded up-front
FUN_14131f7c0();                // TLS: zero per-thread "last skin instance" cache (see Bracket fns)
if ( geometry->vtbl[54](geometry) )          // call [vtbl+0x1B0] = AsBSSkinnedDecalTriShape()
{
    // ===== TRUE: skinned DECAL path (BSSkinnedDecalTriShape only) =====
    FUN_141309f80(pass, shader, v5, renderFlags);   // alpha setup + shader->SetupGeometry (vtbl+0x30)
    FUN_140c7bad0(a1a);                             // NiSkinPartition::Partition::Partition() on stack (0x50 bytes)
    *(uint16*)((char*)a1a + 0x3C) = 1;              // partition.numBones = 1  (gates SetBoneMatrix)
    // NiBoneMatrixSetterI iface = shader+0x10 (BSShader's 2nd vtable); slot 1 (+8) = SetBoneMatrix
    (*(*(shader+0x10) + 8))( shader+0x10,           // this  = NiBoneMatrixSetterI*
                             *(geometry + 0x130),   // rdx   = NiSkinInstance*
                             a1a,                    // r8    = NiSkinPartition::Partition* (temp)
                             geometry + 0x7C );      // r9    = NiTransform* = &geometry->world
    // NOTE: "-2" in the decompile is NOT an argument — it is the EH-state sentinel
    // `mov [rsp+20h], 0FFFFFFFFFFFFFFFEh` written in the prologue, which happens to sit in the
    // 5th-arg home slot. SetBoneMatrix takes 4 args.
    FUN_141307160(pass);                            // GeometryType draw dispatch (already-REd; draws the decal)
    FUN_140c7bb10(a1a);                             // Partition::~Partition()
}
else
{
    // ===== FALSE: normal skinned path (BSTriShape / BSDynamicTriShape / everything else) =====
    FUN_141309f80(pass, shader, v5, renderFlags);   // same SetupGeometry wrapper
    v10 = (*(uint8*)(pass + 0x1E) >> 7) & 1;        // LODMode bit7  = "exact LOD match" flag
    v11 = *(uint8*)(pass + 0x1E) & 0x7F;            // LODMode bits0-6 = LOD level
    v12 = geometry->vtbl[12](geometry);             // call [vtbl+0x60] = AsBSDynamicTriShape()
    // build 0x28-byte stack struct at a1a:
    a1a[0x00] = shader ? shader+0x10 : 0;           // NiBoneMatrixSetterI*
    a1a[0x08] = geometry;
    a1a[0x10] = 0;                                  // (qword) never read in 1.5.97 skinned path
    a1a[0x18] = v10;                                // dword: exact-match flag
    a1a[0x1C] = v11;                                // dword: LOD level
    a1a[0x20] = 0.0f;                               // float: never read in this path
    a1a[0x24] = -1;                                 // dword: dynamic-VB byte offset (out of FUN_140d6c8a0)
    if ( v12 )                                      // dynamic tri shape: upload CPU dynamic verts
    {
        // ring-allocate *(v12+0x170) bytes from the 4MB dynamic-VB ring; offset -> a1a[0x24]
        v15 = FUN_140d6c8a0(0x143028490 /*Renderer, ignored*/, *(uint32*)(v12+0x170), &a1a[0x24]);
        v16 = *(uint32*)(v12 + 0x170);              // dynamicDataSize
        v17 = BSDynamicTriShape_Lock(v12);          // sub_140C723C0: spinlock, returns *(v12+0x160) = pDynamicData
        memcpy_s(v15, v16, v17, v16);               // FUN_14130a030
        FUN_140d6c9e0();                            // ctx->Unmap(currentRingBuffer, 0)
        BSDynamicTriShape_Unlock(v12);              // sub_140C72420
    }
    (*(*v9 + 0x128))(v9, a1a);                      // skinInstance->vfunc37(renderData*) — renders all partitions
}
return shader->vtbl[7](shader, pass, renderFlags);  // tail jmp [vtbl+0x38] = BSShader::RestoreGeometry
```

Exact D3D11 call order for a full skinned dynamic draw (FALSE branch, dynamic, per partition):
1. `ID3D11DeviceContext::End(query)` — only on ring overflow (vf28, +0xE0)
2. `GetData(query,&u32,4,flags)` loop — only when recycling an un-signaled buffer (vf29, +0xE8; first poll flags=1 D3D11_ASYNC_GETDATA_DONOTFLUSH, then 0, Sleep(1) between)
3. `Map(dynVB[idx], 0, D3D11_MAP_WRITE_NO_OVERWRITE(5), 0, &mapped)` (vf14, +0x70)
4. CPU `memcpy_s(mapped+offset, size, shape->pDynamicData, size)`
5. `Unmap(dynVB[idx], 0)` (vf15, +0x78)
6. per partition (inside SetBoneMatrix): `Unmap(boneCB10,0)`; `VSSetConstantBuffers(10,1,&boneCB)` (vf7, +0x38); `Unmap(boneCB9,0)`; `VSSetConstantBuffers(9,1,&prevBoneCB)`
7. manager draw vfunc → IASetVertexBuffers/DrawIndexed (inside geometry-manager, singleton ptr @0x1430136C0; +0x30 = DrawDynamicTriShape, +0x38 = DrawTriShape)

## vfunc54 semantics

`[geometry_vtbl + 0x1B0]` (index 54) is **`AsBSSkinnedDecalTriShape()`** — a Gamebryo-style RTTI cast returning `this` or null.

Vtables (slot0 = dtor xref):
- BSTriShape vtable = 0x141766FF8 → [+0x1B0] = **0x140218960**: `return 0;`
- BSDynamicTriShape vtable = 0x141768A50 → [+0x1B0] = **0x140218960** (same shared stub): `return 0;`
- BSSkinnedDecalTriShape vtable = 0x1417671C0 → [+0x1B0] = **0x140C678E0**: `return a1;` (returns `this`)

So **only BSSkinnedDecalTriShape returns non-zero** (its own pointer); the TRUE branch is the skinned-decal path. Sanity cross-check: `[vtbl+0x60]` (index 12) = `AsBSDynamicTriShape()`: base stub 0x140165910 `return 0`, BSDynamicTriShape override 0x140218930 `return this` (BSTriShape and BSSkinnedDecalTriShape both use the null stub — a skinned decal never takes the dynamic-upload sub-block).

## Bracket fns

**FUN_14131f7c0 — TLS skin-cache invalidate:**
```c
tls = *(TEB->ThreadLocalStoragePointer /*TEB+0x58*/ + 8 * MEMORY[0x143497408] /*TLS index*/);
*(QWORD*)(tls + 10752 /*0x2A00*/) = 0;   // per-thread "last NiSkinInstance uploaded" cache
```
This slot is the dedupe key inside SetBoneMatrix (`if (tls+0x2A00 != skin …)`), so clearing it at pass entry forces bone-CB re-upload once per pass even if the same skin rendered last.

**FUN_140c7bad0 = `NiSkinPartition::Partition::Partition()`** (proved: used as the element ctor with elemsize 80 in NiSkinPartition::LoadBinary_140C7B340 via `fForeachMemSetFunctor(partArray, 80, n, FUN_140c7bad0)`, dtor = FUN_140c7bb10, vector-deleting dtor FUN_140c7d4f0 uses elemsize 80). Zeroes: +0x08,+0x10,+0x18,+0x20,+0x28,+0x30 (six array ptrs), +0x38 (dword: numVertices|numTriangles), +0x3C (dword: numBones|numStrips), +0x48 (rendererData handle).

**FUN_140c7bb10 = `Partition::~Partition()`**: frees the six arrays (+0x08..+0x30) via `dtor_140c249b0` → `(*(**0x142F77208 + 0x10))(*0x142F77208, ptr, 9)` (global heap-interface pointer @0x142F77208; vf1(+8)=alloc(size,align) — see FUN_140c24750 — vf2(+0x10)=free(ptr, 9)); then if +0x48 (rendererData) non-null, releases it via geometry-manager `(*mgr_vtbl + 0x28)(mgr, handle)` (mgr = singleton ptr @**0x1430136C0**). Companion setter FUN_140c7bb90(part, h): release old via mgr vf+0x28, store, addref via mgr vf+0x20.

**Partition layout (0x50 bytes; from stream loader FUN_140c7bc30):**
| off | field |
|---|---|
| +0x00 | BSVertexDesc (u64, SSE stream ver ≥ 0x64) |
| +0x08 | uint16* bones (numBones entries) |
| +0x10 | float* weights (numVertices*numWeightsPerVertex) |
| +0x18 | uint16* vertexMap (numVertices) |
| +0x20 | uint8* boneIndices/palette (numVertices*numWeightsPerVertex) |
| +0x28 | uint16* triList / strip indices (3*numTriangles or Σstrips) |
| +0x30 | uint16* stripLengths (numStrips) |
| +0x38 | u16 numVertices |
| +0x3A | u16 numTriangles |
| +0x3C | u16 numBones |
| +0x3E | u16 numStrips |
| +0x40 | u16 numWeightsPerVertex |
| +0x42 | u8 LOD slot (used in visibility LUT) |
| +0x43 | u8 (ver ≥ 0x4F flag byte) |
| +0x48 | rendererData handle (BSGraphics tri-shape data, managed via mgr @0x1430136C0) |

## True-branch call contract

Call site 0x141308AB3-0x141308ACA:
```
lea rcx,[rsi+10h]      ; this = &shader->NiBoneMatrixSetterI (BSShader 2nd vtable @ +0x10)
mov rdx,[rbx+130h]     ; NiSkinInstance* = geometry+0x130  (v7[38] — qword index 38 = byte 0x130)
lea r8,[rsp+..a1a]     ; NiSkinPartition::Partition* (stack temp; ctor'd, then numBones(word @+0x3C)=1)
lea r9,[rbx+7Ch]       ; NiTransform*  — "v7+124" is BYTE offset 0x7C = NiAVObject::world transform
call [rax+8]           ; NiBoneMatrixSetterI vfunc 1 = SetBoneMatrix
```
- `v7[38]` = **geometry+0x130 = skinInstance** (same value cached in r15/v9).
- `v7 + 124` as printed by Hex-Rays is misleading: the machine code is `lea r9,[rbx+7Ch]` — raw byte offset **0x7C = NiAVObject::world (NiTransform: rot 0x7C..0x9F, pos 0xA0..0xAB, scale 0xAC)** (local is 0x48..0x7B).
- `-2` is **not an argument**: it is the `mov qword [rsp+20h], -2` EH-unwind-state sentinel from the prologue that aliases the 5th-arg home slot. The callee takes 4 args.

**The single shared implementation** (all shader NiBoneMatrixSetterI vtables sampled — 0x141850948, 0x141856540, 0x1418511B8, 0x141851310 — point at it): `BSShader::SetBoneMatrix @ 0x14131F630`:
```c
tls+0x760 saved, set to 26 (perf/context marker); 
if (tls+0x2A00 != skin && partition && *(dword*)(partition+0x3C) /*numBones|strips*/ != 0) {
  tls+0x2A00 = skin;
  FUN_140D74F70(skin);                         // update skin->BoneMatrices under skin->Lock(+0x60):
                                               //   once per frame (skin+0x38 vs frame counter @0x14302C8DC),
                                               //   composes rootParent->world(+0x7C) x skinData(+0x10)->rootToSkin(+0x18)
                                               //   then per-bone boneWorld(+0x30[i]) x skinToBone(skinData+0x50, stride 88),
                                               //   stores transposed scaled 4x3 into skin+0x48 (cur) / skin+0x50 (prev)
  n = 3 * *(dword*)(skin->skinData(+0x10) + 0x58);   // 3 float4 rows per bone
  buf10 = GetID3D11Resource(Renderer@0x143028490, n, &mapped, 10);  // per-frame CB alloc (0x140D6FFD0)
  memcpy_s(mapped, 16*n, skin+0x48, 16*n);           // current bone matrices
  ctx->Unmap(buf10,0); ctx->VSSetConstantBuffers(10,1,&buf10);      // b10 = bones
  buf9 = GetID3D11Resource(..., n, &mapped, 9);
  memcpy_s(mapped, 16*n, skin+0x50, 16*n);           // previous-frame bone matrices
  ctx->Unmap(buf9,0);  ctx->VSSetConstantBuffers(9,1,&buf9);        // b9 = previous bones (motion vectors)
}
```
This is exactly why the dispatcher sets the temp partition's numBones word to 1: it satisfies the `*(partition+0x3C) != 0` gate. **The 4th arg (&geometry->world) is ignored by this implementation.** The bone data comes from the skin instance, not the partition. After SetBoneMatrix, the TRUE branch draws through the ordinary GeometryType dispatch FUN_141307160(pass) (the already-REd draw leaf), then destroys the temp Partition.

## False-branch struct + dynamic sub-block

**Stack struct (0x28 bytes) passed to `skinInstance->vfunc[37]` ([vtbl+0x128]):**
| off | value | source | consumer |
|---|---|---|---|
| +0x00 | shader ? shader+0x10 : 0 | NiBoneMatrixSetterI* | NiSkinPartition::Func37 → SetBoneMatrix this |
| +0x08 | geometry | pass+0x10 | Func37: reads geom+0x130 (skin) and geom+0x7C (world) and calls geom vf12 (AsBSDynamicTriShape) |
| +0x10 | 0 (qword) | const | **never read** in the 1.5.97 skinned chain |
| +0x18 | (pass+0x1E)>>7 & 1 (dword) | LODMode bit7 | LUT row select ("exact match" mode) |
| +0x1C | (pass+0x1E)&0x7F (dword) | LODMode bits0-6 | LOD level for LUT |
| +0x20 | 0.0f (movss) | const | **never read** in this chain |
| +0x24 | -1, overwritten by ring offset | out-param of FUN_140d6c8a0 | dynamic-draw vertex-buffer byte offset |

**vfunc37 implementations** (NiSkinInstance vtable index 37, offset 0x128):
- `NiSkinInstance::Func37_140C7E170(this, data)` → `FUN_140c7ca10(this->skinPartition /*+0x18*/, data)`: loops `for i in 0..skinPartition->numPartitions(+0x10)` calling `skinPartition->vfunc[37](data, i)`.
- `BSDismemberSkinInstance::Func37_140C6B9F0`: if per-partition enable array (this+0x90) exists, calls `skinPartition->vfunc[37](data, i)` only for partitions whose enable dword is set; else falls back to NiSkinInstance::Func37.

**Per-partition renderer `NiSkinPartition::Func37_140C7CA70(this, data, i)`:**
```c
part = this->partitions(+0x18) + 0x50*i;
if (!byte_141E06650[12*data->exactFlag(+0x18) + 3*data->lodLevel(+0x1C) + part->lodSlot(+0x42)])
    return 0;                                             // LOD visibility LUT (24 bytes, see below)
data->setter(+0x00)->vfunc1( setter,                      // SetBoneMatrix(
        *(data->geometry(+0x08) + 0x130),                 //   skin,
        part,                                             //   partition,     — real partition, numBones gate real
        data->geometry + 0x7C );                          //   &geom->world)  — ignored by impl
if (dyn = geometry->vfunc12() /* AsBSDynamicTriShape, called twice */)
    mgr(@0x1430136C0)->vfunc[6](+0x30)(mgr, part->rendererData(+0x48), dyn+0x178, 0,
                                       part->numTriangles(+0x3A), data->dynOffset(+0x24));
                                                          // DrawDynamicTriShape(handle, &dyn+0x178, start=0, numTris, vbByteOffset)
else
    mgr->vfunc[7](+0x38)(mgr, part->rendererData(+0x48), 0, part->numTriangles(+0x3A));
                                                          // DrawTriShape(handle, start=0, numTris)
```
LUT `byte_141E06650` (24 bytes = [exactFlag][lodLevel 0..3][lodSlot 0..2]):
- exactFlag=0: lvl0:{0,0,0} lvl1:{1,0,0} lvl2:{1,1,0} lvl3:{1,1,1} → draw partitions with lodSlot < lodLevel
- exactFlag=1: lvl0:{1,0,0} lvl1:{0,1,0} lvl2:{0,0,1} lvl3:{0,0,0} → draw only lodSlot == lodLevel

**Dynamic-tri-shape sub-block (dispatcher-side, before vf37):**
- geometry vfunc at +0x60 (index 12) = `AsBSDynamicTriShape()`; non-null only for BSDynamicTriShape (and subclasses e.g. BSParticleShaderGeometry / faces).
- BSDynamicTriShape fields: +0x148 vertexDesc, +0x15A vertexCount(u16), **+0x160 pDynamicData (CPU copy), +0x168 lock ownerTID, +0x16C lock recursion count, +0x170 dynamicDataSize (u32), +0x178 opaque per-shape slot passed by address to the manager's dynamic draw**.
- `sub_140C723C0` (lock/get): `Mutex::Lock1_140132BD0(&shape+0x168, 0); return *(shape+0x160);` — recursive owner-TID spinlock, returns the CPU dynamic vertex data.
- `sub_140C72420` (unlock): lfence; if owner==GetCurrentThreadId(): if count==1 { owner=0; mfence; ICX(count,1→0) } else InterlockedDecrement(count).
- `FUN_14130a030` = memcpy_s clone (dest,destSize,src,count; errno 22/34 + memset-zero on failure).

**FUN_140d6c8a0 — dynamic-VB ring allocator (globals @ 0x143025F18; rcx arg ignored):**
- 3 × ID3D11Buffer* dynamic VBs at **0x143025F18 + 8*idx** (idx 0..2), each **0x400000 (4 MiB)**.
- current index: dword @ **0x143025F30**; current byte offset: dword @ **0x143025F34**.
- 3 × ID3D11Query* (event) at **0x143026168 + 8*idx**; 3 × u8 "GPU-done" flags at **0x143026164 + idx**.
- device context fetched from cached ptr @ **0x143027EA0** (same object as MEMORY[0x1430261B0][926]).
- Algorithm: if offset+size > 4MiB → clear flag[idx], `ctx->End(query[idx])` (vf28), idx=(idx+1)%3, offset=0. If flag[newIdx]==0 → loop `ctx->GetData(query[newIdx], &u32, 4, flags)` (vf29; first poll flags=1 DONOTFLUSH, then 0; Sleep(1) between) until S_OK && data!=0; flag = (data==1). Then `ctx->Map(buf[idx], 0, D3D11_MAP_WRITE_NO_OVERWRITE /*5*/, 0, &mapped)` (vf14); store idx/newOffset back; `*out = oldOffset`; return mapped.pData + oldOffset.
- `FUN_140d6c9e0` = `ctx->Unmap(buf[currentIdx], 0)` (vf15).
- So protocol per dynamic shape: ring-alloc → Map(NO_OVERWRITE) → memcpy_s of shape->pDynamicData under the shape spinlock → Unmap → offset recorded in struct+0x24 → consumed as the VB byte offset by the manager's DrawDynamicTriShape (vfunc +0x30 of singleton @0x1430136C0).

## CAVEATS
- The geometry-data manager singleton (pointer @ 0x1430136C0; vf3(+0x18)=create tri-shape data, vf4(+0x20)=addref, vf5(+0x28)=release, vf6(+0x30)=DrawDynamicTriShape, vf7(+0x38)=DrawTriShape, vf15(+0x78)=write dynamic vertex component, vf30(+0xF0)/vf31(+0xF8)=create/release shared vertex block): I did not locate its concrete class/vtable (no write xref surfaced; only reads). Its draw vfuncs were not decompiled here — they are the layer that issues IASetVertexBuffers/DrawIndexed (the already-REd DrawTriShape leaf @0x140D6BFE0 family).
- BSDynamicTriShape+0x178: passed by address to the dynamic draw; exact meaning (cached binding record vs. second size field) not determined.
- False-branch struct fields +0x10 (qword 0) and +0x20 (float 0.0) are written but never read anywhere in the NiSkinInstance/BSDismemberSkinInstance → NiSkinPartition::Func37 chain; other vfunc37 overrides (if any exist beyond the two found) were not exhaustively enumerated.
- The heap interface @0x142F77208 (vf+8 alloc(size,align=4/16), vf+0x10 free(ptr, 9), vf+0x18 realloc) used for partition arrays and skin bone-matrix arrays: concrete class not identified; the constant 9 in free() is unexplained (pool/area id?).
- Partition ctor does NOT zero +0x00 (vertexDesc), +0x40 (numWeightsPerVertex), +0x42/+0x43 — in the TRUE branch's stack temp these hold garbage; they are provably unread by SetBoneMatrix (only +0x3C dword gates), but any hook replicating this should still zero-init the full 0x50 for safety.
- LUT byte_141E06650 read as 24 bytes; lodLevel values > 3 would index past it (engine presumably never produces them, bits0-6 of pass+0x1E notwithstanding). Not verified.
- NiAVObject layout used (local@0x48, world@0x7C) matches CommonLibSSE and the rootParent->WorldTransform_7C field name in the idb, but was not independently re-derived here.

### Machine-extracted caveats
- Geometry-data manager singleton (ptr @0x1430136C0) concrete class/vtable not located; its DrawTriShape/DrawDynamicTriShape vfuncs (+0x38/+0x30) not decompiled in this pass.
- BSDynamicTriShape+0x178 (passed by address to the dynamic draw) purpose undetermined.
- False-branch struct fields +0x10 and +0x20 are written but never read in the discovered consumer chain; other vfunc37 overrides not exhaustively enumerated.
- Heap interface @0x142F77208 class unidentified; the constant 9 passed to its free() unexplained.
- Partition stack temp in the TRUE branch leaves +0x00/+0x40/+0x42/+0x43 uninitialized (provably unread by SetBoneMatrix, but replicas should zero the full 0x50).
- byte_141E06650 LUT verified only for lodLevel 0..3; larger values would index out of the 24-byte table.
- NiAVObject world-transform offset 0x7C taken from CommonLibSSE/idb field names, not independently re-derived.

================================================================================================
## Cluster 5
================================================================================================

# RE report — non-TRISHAPE cases of the geometry Draw dispatch `FUN_141307160` (SkyrimSE 1.5.97)

`FUN_141307160(BSRenderPass* pass@rcx, int64 a2@rbx, float a3@xmm0)`:
`geom = *(pass+0x10)`, `switch(byte geom+0x150 − 1)` (13 cases, `ja` default = return). Case N handles GeometryType N+1. All draws go through `BSGraphics::SetDirtyStates_140d705b0(0)` before any IA/Draw call, so full pipeline state (shaders, CBs, RTs, samplers, textures, input layout, raster/blend/DS state) is flushed from the RendererShadowState first; the leaf functions only touch IA-stage state + Draw.

## Shared infrastructure (used by all cases)

### Globals
| Address | Meaning |
|---|---|
| `0x143028490` | BSGraphics::Renderer singleton (`flt_143028470+0x20`), passed as rcx to all draw leaves (never dereferenced in them — vestigial `this`) |
| `0x143027EA0` | ID3D11DeviceContext* (immediate). Identical to `(qword*)0x1430261B0)[926]` (0x1430261B0+0x1CF0) |
| `0x143025F08` | ID3D11Device* |
| `0x143027EB0` | RendererShadowState stateUpdateFlags (dword). Bit 0x400 = vertex-desc/input-layout dirty, bit 0x800 = topology dirty |
| `0x143027EB4` | PSResourceModifiedBits (dword) — consumed by SetDirtyStates at 0x140d70bec..0x140d70c2b |
| `0x143027FF0` | `qword_143027FF0[]` = shadow-state PSTexture SRV array (`&PSTexture[0]`; RendererShadowState+0x140), flushed via PSSetShaderResources in SetDirtyStates (0x140d70c07) |
| `0x1430281F0` | shadow-state current vertexDesc (qword; `dword_143028070[96]`) |
| `0x1430281F8` | shadow-state current vertex shader object (qword; `dword_143028070[98]`). VS+0x10 = bytecode length, VS+0x48 = vertex-input mask, VS+0x68 = bytecode start |
| `0x143028208` | shadow-state current primitive topology (dword; `dword_143028070[102]`) |
| `0x143025F18` | Dynamic-geometry VB ring (see below) |
| `0x14302AE50` | Static lazily-initialized "particle quad" geometry descriptor (case 0) |
| `0x143283BB0` / `0x143477BB0` | CPU staging: strip-particle vertex / index build buffers (case 1) |
| `0x14302C8DC` | BSGraphics::State::uiFrameCount — `++` once per frame in the end-frame/Present function `FUN_140d6a2b0` (0x140d6a300) |
| `0x14302C8E8` | NiSourceTexture* default/fallback texture (BSGraphics::State block; also used by `BSLightingShaderProperty::sub_1412C56D0` as texture fallback, written at shader-manager init `FUN_1412eeb90`). Used SRV = `*(*(tex+0x48)+0x10)` (rendererTexture→SRV) |
| `0x1430243B0` | byte flag "map render / draw-all-segments": set to 1 in `FUN_1404b35e0` and cleared in `FUN_1404b2960` (local/world-map rendering system; MapMenu/TESWorldSpace involved) |
| `0x1434963C8` | byte flag read ONLY by case 8; **no writer exists in the binary** → always 0 (dead debug toggle) |
| `0x141E07140` / `0x141E07144` / `0x141E07160` | global input-layout hash cache (lock/size/table). Key = CRC (`sub_140C06570`) of `(curVertexDesc & VS->inputMask@+0x48)`; insert via `FUN_140d71830` |

### The two static draw leaves
`FUN_140d6bfe0(renderer, rendererData a2, uint32 startIndex a3, int triCount a4)` — the known DrawTriShape:
```c
if (qword[0x1430281F0] != *(a2+0x10)) { qword[0x1430281F0] = *(a2+0x10); dirty|=0x400; }
if (dword[0x143028208] != 4)         { dword[0x143028208] = 4; dirty|=0x800; }   // TRIANGLELIST
SetDirtyStates(0);
ctx->IASetIndexBuffer(*(a2+8), DXGI_FORMAT_R16_UINT /*57*/, 0);                   // vfunc 19, r9d=0
stride = (4 * *(a2+0x10)) & 0x3C;  offset = 0;
ctx->IASetVertexBuffers(0, 1, (ID3D11Buffer**)a2 /* [a2+0]=VB */, &stride, &offset); // vfunc 18
ctx->DrawIndexed(3*a4, a3, 0);                                                     // vfunc 12
```
`FUN_140d6c0e0(renderer, rendererData, startIndex, triCount, void** a5)` — byte-identical **except** `IASetIndexBuffer(*a5, 57, 0)`: the index buffer comes from an alternate IB slot pointer instead of `rendererData+8`. Used by cases 5/6 when `byte(pass+0x1C) == 12`.

### Dynamic-geometry VB ring (`qword_143025F18`)
- `[0..2]` (0x143025F18/20/28): three 4 MiB `ID3D11Buffer` dynamic VBs; `[3].lo` (dword 0x143025F30) = current ring index, `[3].hi` (0x143025F34) = current byte offset; `[4]` (0x143025F38) = shared particle-quad index buffer (R16, 6 indices/4 verts); `[5]` (0x143025F40) = shared particle "corner" VB (slot-0 stream, stride 4); `[6]` (0x143025F48) = cached ID3D11InputLayout for particle-shader dynamic geometry; `[7]` (0x143025F50) = cached ID3D11InputLayout for strip particles; `[74+i]` = per-buffer ID3D11Query (event); `[77+i]` = query-satisfied flag.
- **Allocator `FUN_140d6c8a0(renderer, int size, uint32* outOffset)`**:
```c
if (curOffset + size > 0x400000) {              // 4 MiB wrap
    ctx->End(query[curIdx]);                    // vfunc 28
    readyFlag[curIdx] = 0; curIdx = (curIdx+1)%3; curOffset = 0;
}
if (!readyFlag[curIdx])                          // busy-wait GPU done with that third
    while (ctx->GetData(query[curIdx], &r, 4, flags 1 then 0) < 0 || !r) Sleep(1);  // vfunc 29
ctx->Map(buf[curIdx], 0, D3D11_MAP_WRITE_NO_OVERWRITE /*5*/, 0, &m);                // vfunc 14
*outOffset = oldOffset;  curOffset = oldOffset + size;  return m.pData + oldOffset;
```
- `FUN_140d6ca10(renderer, unused, desc, unusedOutPtr, size)` = `FUN_140d6c8a0(renderer, size ? size : *(desc+0x1C), desc+0x18)` — i.e. **the allocation offset is stored into `desc+0x18`** (the rendererData/descriptor itself); the r9 out-pointer argument the dispatch passes (e.g. `shape+0x178`, `&stackvar`) is **ignored/vestigial**. `FUN_140d6d5d0` is an identical clone.
- `FUN_140d6ca30()` / `FUN_140d6d5f0()` = `ctx->Unmap(dynVB[curIdx], 0)` (vfunc 15).
- **Draw `FUN_140d6ca60(renderer, desc, ignoredPtr, uint32 startIndex@r9d, int triCount stack)`** copies `desc[0]`,`desc[8]`,`desc[0x10]` and `desc[0x18]` then tail-calls `FUN_140d6cab0`:
```c
// FUN_140d6cab0: vd = desc[0x10]
vertexDesc-dirty check (0x400) + topology=4 check (0x800);  SetDirtyStates(0);
ctx->IASetIndexBuffer(desc[8], 57, 0);
bufs    = { desc[0],          dynVB[curIdx] };
strides = { (4*vd)&0x3C,      (vd>>2)&0x3C  };
offsets = { 0,                desc[0x18]    };   // <- allocator-written offset
ctx->IASetVertexBuffers(0, 2, bufs, strides, offsets);
ctx->DrawIndexed(3*triCount, startIndex, 0);
```
`BSGraphics::Renderer::FUN_140d6d620` is the LINELIST twin (topology 2, `DrawIndexed(2*count, start, 0)`, then `desc[0x18] = -1`).

### `byte(pass+0x1C) == 12` gate (cases 5 and 6)
Disasm: `cmp byte ptr [rcx+1Ch], 0Ch` — a **byte** compare on BSRenderPass+0x1C (the project's "accumulationHint" slot). When equal, the multi-index shapes draw with their **alternate index set** (`geom+0x160` IB slot + alternate counts) via `FUN_140d6c0e0`; otherwise the normal IB via `FUN_140d6bfe0`. No other case reads pass fields besides `pass+0x10`.

---

## Case 0 (GeometryType 1 — NiParticles quad particles)
Geometry fields read: `geom+0x158` (**qword** = NiParticlesData* here, not triCount), `geom+0x148` (vertexDesc dword), world transform @0x7C, ModelBound @0x110 (inside PackParticleData).
```c
count = particleData->vfunc[+0x130]();  count = min(count & 0xFFFF, 2048);
stride = (dword(geom+0x148) >> 2) & 0x3C;                 // 20 bytes for desc 0x…51
if (!count) return;
desc = FUN_140d6c7e0();   // static 0x14302AE50, lazy init (thread-safe static guard):
                          //   [0]=cornerVB(0x143025F40) [8]=quadIB(0x143025F38)
                          //   [0x10]=vertexDesc 0x0840200004000051  [0x18]=-1  [0x20]=1
ptr = FUN_140d6ca10(renderer, 0, desc, &dummy, 4*count*stride);   // offset -> desc+0x18
if (ptr) { PackParticleData_140d76080(count, geom, ptr); FUN_140d6ca30(); /*Unmap*/ }
FUN_140d6ca60(renderer, desc, &dummy, 0, 2*count);        // called even if Map failed
```
`PackParticleData_140d76080` CPU-expands each particle into a camera-facing quad: 4 vertices × 20 B `{float3 corner position (world, relative to bound center); float texIndex/subtexture (from speedToAspect array, 0 if absent); uint32 RGBA color (alpha zeroed when a·fade ≤ 0.05)}`, using view axes from `qword_143028230` (shadow-state camera rows) and sorted indices from `FUN_140c88190/140c88200` (radix sort by depth into scrap arrays).
D3D11 sequence (inside cab0): dirty checks → `SetDirtyStates(0)` → `IASetIndexBuffer(quadIB, R16_UINT, 0)` → `IASetVertexBuffers(0, 2, {cornerVB, dynVB}, {4, 20}, {0, allocOffset})` → `DrawIndexed(6*count, 0, 0)`.

## Case 1 (GeometryType 2 — BSStripParticleSystem)
```c
vertCount = 0; indexCount = 0;
BSStripParticleSystem::sub_140D76D80(vtxStaging 0x143283BB0, psys, idxStaging 0x143477BB0,
                                     &vertCount, a3 /*float in xmm0 forwarded from caller*/, &indexCount);
if (vertCount && indexCount)
    FUN_140d6ce60(vtx@rdx, renderer@rcx, a2@rbx(unused), vertCount@r8d, idx@r9, indexCount stack);
```
`sub_140D76D80`: iterates strips (`psys+0x158` → BSStripPSysData; strip count @data+0x7C, per-strip ring descriptors @data+0xA8, stride 40). Per point emits **4 vertices × 40 B** `{float3 pos@0, float u@12 (accumulates by a3 per step), float v@16 (0.0/1.0), float subtexIndex@20 (-1 if none), float3 normal/binormal@24, uint32 RGBA@36}` and strip indices (u16) with degenerate stitching (`prev-1, prev` pair) between strips. Outputs: `*a4 = 4*totalPoints`, `*a6 = index count incl. degenerates`.
`FUN_140d6ce60` D3D11 sequence:
1. `p = FUN_140d6c8a0(renderer, 40*vertCount, &off1)`; `memcpy_s(p, …, vtxStaging, 40*vertCount)` (`FUN_140d74a60` = memcpy_s); `ctx->Unmap(dynVB[cur], 0)`.
2. `ctx->IASetVertexBuffers(0, 1, &dynVB[cur], {40}, {off1})`.
3. `p2 = FUN_140d6c8a0(renderer, 2*indexCount, &off2)`; memcpy indices; `ctx->Unmap(dynVB[cur], 0)`.
4. `ctx->IASetIndexBuffer(dynVB[cur], R16_UINT/*57*/, off2)` — the **index buffer is the dynamic VB at a byte offset**.
5. topology: `dword[0x143028208]=5` (TRIANGLESTRIP, dirty|=0x800); **clears** dirty bit 0x400 (`v & 0xFFFFFBFF`) because it manages the input layout manually; `SetDirtyStates(0)`.
6. If `qword_143025F18[7]==0`: `device->CreateInputLayout` (vfunc 11) with 4 elements stride 40 — `POSITION0 R32G32B32_FLOAT@0`, `TEXCOORD0 R32G32B32_FLOAT@12`, `NORMAL0 R32G32B32_FLOAT@24`, `COLOR0 R8G8B8A8_UNORM(28)@36` — against current VS bytecode (`VS=qword[0x1430281F8]`, bytecode `VS+0x68`, length `*(VS+0x10)`), out → `0x143025F50`.
7. `ctx->IASetInputLayout(layout)` (vfunc 17); then key = CRC(`qword[0x1430281F0] & *(VS+0x48)`); if not in the global layout cache (`0x141E07160`), `FUN_140d71830(layout, key)` inserts it; `dirty|=0x400` (forces proper rebind on next normal draw).
8. `ctx->DrawIndexed(indexCount, 0, 0)` (strip: raw index count, no ×3).

## Case 3 (GeometryType 4 — BSDynamicTriShape)
Fields: shape via vfunc `geom+0x60` (AsDynamicTriShape); `shape+0x138` rendererData; `shape+0x160` dynamic-data CPU ptr; `shape+0x168/0x16C` spinlock tid/recursion; `shape+0x170` dynamic data size (dword); `shape+0x174` last-upload frame stamp; `shape+0x158` triCount (word). rendererData: `+0` VB, `+8` IB, `+0x10` vertexDesc, `+0x18` dynamic offset, `+0x1C` default alloc size.
```c
shape = geom->vfunc60();  rd = *(shape+0x138);
if (*(u32*)(shape+0x174) != frameCount@0x14302C8DC) {        // once per frame
    *(shape+0x174) = frameCount;
    dst = FUN_140d6ca10(renderer, shape, rd, shape+0x178 /*ignored*/, 0);  // size=*(rd+0x1C), offset->rd+0x18
    size = *(u32*)(shape+0x170);
    src = BSParticleShaderGeometry::sub_140C723F0(shape);     // spinlock(shape+0x168); return *(shape+0x160)
    FUN_14130a030(dst, size, src, size);                      // memcpy_s
    BSDynamicTriShape::sub_140C72420(shape);                  // unlock
    FUN_140d6ca30();                                          // Unmap(dynVB[cur], 0)
}
FUN_140d6ca60(renderer, rd, shape+0x178 /*ignored*/, 0, (u16)*(shape+0x158));
```
D3D11 (cab0): dirty checks (vertexDesc `rd+0x10`, topology 4) → SetDirtyStates(0) → `IASetIndexBuffer(rd[8], R16, 0)` → `IASetVertexBuffers(0,2,{rd[0],dynVB},{(4vd)&0x3C,(vd>>2)&0x3C},{0, rd[0x18]})` → `DrawIndexed(3*triCount, 0, 0)`.

## Case 4 (GeometryType 5 — BSMeshLODTriShape)
Fields: `geom+0x108` current LOD byte (bit7 = "draw single level"), `geom+0x160` = `uint32 lodSize[3]` (triangles per LOD level), `geom+0x138` rendererData.
```c
lod = (u8)geom[0x108];
count = FUN_141330240(geom, lod);
//   bit7 set:   return lodSize[lod & 0x7F];
//   bit7 clear: return Σ lodSize[0 .. lod-1];             (pairwise-unrolled sum)
if (!count) return;
start = FUN_1413300c0(geom);          // re-reads lod dword @geom+0x108
//   bit7 set:   return 3 * Σ lodSize[0 .. (lod&0x7F)-1];   else 0
FUN_140d6bfe0(renderer, *(geom+0x138), start, count);
```
D3D11: exactly the standard TRISHAPE sequence with computed `StartIndexLocation`/`IndexCount=3*count`. Index buffer is laid out LOD0|LOD1|LOD2 consecutively.

## Case 5 (GeometryType 6 — LOD multi-index tri shape)
Fields: `geom+0x108` LOD dword (bit7 as above), `geom+0x1D8` = per-LOD `uint32 pair[ ][2] {cnt_main, cnt_alt}`, `geom+0x160` = alternate index-buffer slot (qword; `*(geom+0x160)` = ID3D11Buffer*), `geom+0x138` rendererData.
```c
lod = (u8)geom[0x108];
if (byte(pass+0x1C) == 12) {          // alternate index set
    count = FUN_1413308f0(geom, lod, 1);   // bit7 ? pair[lod&0x7F][1] : Σ pair[i][1], i<lod  (base geom+0x1D8, element = +4*(which+2*i))
    start = FUN_141330850(geom, 1);        // 3 * (bit7 ? Σ pair[i][1], i<(lod&0x7F) : 0)
    FUN_140d6c0e0(renderer, rd, start, count, (void**)(geom+0x160));  // IASetIndexBuffer(*(geom+0x160))
} else {
    count = FUN_1413308f0(geom, lod, 0);
    start = FUN_141330850(geom, 0);
    FUN_140d6bfe0(renderer, rd, start, count);                        // IASetIndexBuffer(rd[8])
}
```
D3D11: standard sequence; only IB source and start/count differ.

## Case 6 (GeometryType 7 — BSMultiIndexTriShape)
Fields: `geom+0x160` alternate IB slot (qword), `geom+0x168` alternate triCount (dword), `geom+0x158` triCount (word), `geom+0x138` rendererData.
```c
if (byte(pass+0x1C) == 12)
    FUN_140d6c0e0(renderer, rd, 0, *(u32*)(geom+0x168), (void**)(geom+0x160));
else
    FUN_140d6bfe0(renderer, rd, 0, (u16)*(geom+0x158));
```

## Case 7 (GeometryType 8 — BSSubIndexTriShape)
Fields: `geom+0x138` rendererData; segment system: `geom+0x160` = segment-record array ptr, `geom+0x168` = numSegments (dword), `geom+0x16C` = active-range count, `geom+0x170` = dirty byte, `geom+0x171` = "draw whole object" byte, `geom+0x158` triCount (word). Segment record stride 20 (0x14): `+0` StartIndexLocation (u32, index units), `+4` ownTriCount, `+8` enabled byte, `+0xC` mergedTriCount, `+0x10` rangeActive byte.
```c
rd = *(geom+0x138);
BSSubIndexTriShape::sub_140D59430(geom);
//   if dirty(+0x170): walk segments last→first, coalescing consecutive enabled
//   segments into contiguous draw ranges: head seg gets rangeActive=1 and
//   mergedTriCount = own + next.merged (start inherited from next when own==0);
//   absorbed segs get rangeActive=0; +0x16C = number of active ranges; clears +0x170.
if (byte[0x1430243B0]) {                                  // map rendering: draw everything
    FUN_140d6bfe0(renderer, rd, 0, (u16)*(geom+0x158));  return;
}
n = geom[0x171] ? 1 : *(u32*)(geom+0x168);   if (!n) return;
for (i = 0, off = 0; i < n; i++, off += 0x14) {           // ASCENDING
    seg = *(geom+0x160) + off;
    if (!seg[0x10]) continue;                             // rangeActive
    count = geom[0x171] ? (u16)*(geom+0x158) : *(u32*)(seg+0xC);
    start = geom[0x171] ? 0                  : *(u32*)(seg+0);
    FUN_140d6bfe0(renderer, rd, start, count);            // full standard sequence per range
}
```
D3D11: one standard TRISHAPE sequence (incl. SetDirtyStates + IASetIndexBuffer + IASetVertexBuffers) **per active merged range** — not batched.

## Case 8 (GeometryType 9 — BSSubIndexLandTriShape, landscape)
Fields: as case 7 plus `geom+0x178` = ptr to per-segment `uint32 textureMask[]` (bit b set = land-texture layer b unused/invalid from this segment). Segment records identical to case 7.
```c
rd = *(geom+0x138);
if (byte[0x1434963C8]) {          // DEAD: never written, always 0
    seg0 = *(geom+0x160);
    FUN_140d6bfe0(renderer, rd, geom[0x171]?0:*(u32*)seg0, geom[0x171]?(u16)*(geom+0x158):*(u32*)(seg0+0xC));
    return;
}
// live path: iterate segments DESCENDING from numSegments-1 down to 1 (segment 0 is NEVER drawn here;
// if geom[0x171] is set n=1 so NOTHING is drawn)
bit = 5; bitMask = 0x20;
for (i = (geom[0x171] ? 1 : *(u32*)(geom+0x168)) - 1, off = 20*i;  i >= 1;  i--, off -= 20) {
    mask = *(u32*)(*(geom+0x178) + 4*i);                  // FUN_141334070(geom, i)
    if ((mask & bitMask) == 0) {
        // demote unused landscape texture slots to the default texture:
        do {
            srv = tex0x14302C8E8 ? *(*(tex+0x48)+0x10) : 0;    // NiSourceTexture->rendererTexture->SRV
            if (qword_143027FF0[bit]   != srv) { qword_143027FF0[bit]   = srv; dword[0x143027EB4] |= 1<<bit; }     // PS t[bit]   (diffuse layer)
            if (qword_143027FF0[bit+7] != srv) { qword_143027FF0[bit+7] = srv; dword[0x143027EB4] |= 1<<(bit+7); } // PS t[bit+7] (normal layer)
            if (bit == 0) break;
            bitMask >>= 1; bit--;
        } while ((mask & bitMask) == 0);
    }
    seg = *(geom+0x160) + off;
    if (seg[0x10]) {                                       // rangeActive
        count = geom[0x171] ? (u16)*(geom+0x158) : *(u32*)(seg+0xC);
        start = geom[0x171] ? 0                  : *(u32*)(seg+0);
        FUN_140d6bfe0(renderer, rd, start, count);         // SetDirtyStates inside flushes the SRV rebinds
    }
}
```
The SRV writes are shadow-state only (`qword_143027FF0[slot]` + dirty bits in `0x143027EB4`); the actual `PSSetShaderResources` happens inside `SetDirtyStates` in the subsequent `FUN_140d6bfe0`. `bit`/`bitMask` persist across segments (monotonically decreasing 5→0), so slots are demoted at most once per shape.

## Case 9 (GeometryType 10 — grass, multi-stream instancing)
Fields: `geom+0x170` = batch count (dword), `geom+0x160` = array of instance-group ptrs (8 B each), `geom+0x158` triCount (word), `geom+0x148` vertexDesc (qword), `geom+0x138` rendererData. Instance group: `+0x40` = ID3D11Buffer* instance-data VB, `+0x4C` = instance count (dword), `+0x50` = active byte.
```c
for (i = 0; i < *(u32*)(geom+0x170); i++) {
    inst = ((void**)*(geom+0x160))[i];
    if (!inst || !*(u8*)(inst+0x50)) continue;
    // per-batch VS constant buffer (batch index):
    holder = GetID3D11Resource(renderer, 1, &cpu, 7);
    //   a4==7 → cb = qword[0x143027E90] ( = (qword*)0x1430261B0)[924] );  (a4!=7 would use a
    //   round-robin ring of 4 buffers @0x1430261B0[779..782] with counter dword @[778])
    //   ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD /*4*/, 0, &m);  *cpu = m.pData;  returns &qword_14302AC58 (holds cb)
    *(u32*)cpu = i;                                   // write batch index into the cbuffer
    if (*holder) ctx->Unmap(*holder, 0);              // vfunc 15
    ctx->VSSetConstantBuffers(7, 1, holder);          // vfunc 7 — VS b7 = grass batch index
    fDrawGrass_140D6C1E0(renderer, rd, 0, (u16)*(geom+0x158),
                         *(u32*)(inst+0x4C), *(u64*)(geom+0x148), (int64**)(inst+0x40));
}
```
`fDrawGrass_140D6C1E0` D3D11 sequence: vertexDesc dirty check vs `geom` vertexDesc (a6) → topology 4 → `SetDirtyStates(0)` → `IASetIndexBuffer(rd[8], R16, 0)` → `IASetVertexBuffers(0, 2, {rd[0], *(inst+0x40)}, {(4*vd)&0x3C, (vd>>2)&0x3C}, {0, 0})` → `DrawIndexedInstanced(3*triCount, instCount, 0, 0, 0)` (vfunc 20, ctx+0xA0).

## Case 10 (GeometryType 11 — particle-shader dynamic tri shape)
Fields: shape via vfunc `geom+0x60`; `shape+0x15A` vertexCount (word); `shape+0x160` dynamic data ptr; `shape+0x168/0x16C` spinlock.
```c
shape = geom->vfunc60();
n = (u16)*(shape+0x15A);
data = sub_140C723F0(shape);         // lock + return *(shape+0x160)
FUN_140d6cbe0(data@rdx, renderer@rcx, n@r8d);
sub_140C72420(shape);                // unlock
```
`FUN_140d6cbe0` D3D11 sequence (stride 48):
1. `p = FUN_140d6c8a0(renderer, 48*n, &off)`; `memcpy_s(p, …, data, 48*n)`; `ctx->Unmap(dynVB[cur], 0)`.
2. topology 4 (dirty 0x800 if changed); **clears** dirty 0x400; `SetDirtyStates(0)`.
3. If `qword_143025F18[6]==0`: `device->CreateInputLayout` — `POSITION0 R32G32B32A32_FLOAT(2)@0`, `NORMAL0 R32G32B32A32_FLOAT(2)@16`, `TEXCOORD0 R32G32B32_FLOAT(6)@32`, `TEXCOORD1 R8G8B8A8_SINT(32)@44`, stride 48, vs current VS bytecode → cached @0x143025F48.
4. Layout-cache lookup (key CRC(curVertexDesc & VS mask)); insert via `FUN_140d71830` if absent; `ctx->IASetInputLayout(qword_143025F18[6])`; `dirty|=0x400`.
5. `ctx->IASetIndexBuffer(qword_143025F18[4] /*shared quad IB*/, R16, 0)`.
6. `ctx->IASetVertexBuffers(0, 1, &dynVB[cur], {48}, {off})`.
7. `ctx->DrawIndexed(6*(n>>2), 0, 0)` — quads: 6 indices per 4 vertices.

## Case 11 (GeometryType 12 — BSLines)
`FUN_140d6d310(renderer, rd = *(geom+0x138), count = (u16)*(geom+0x158))`:
vertexDesc dirty check on `rd+0x10` → topology `2` (LINELIST, dirty 0x800) → `SetDirtyStates(0)` → `IASetIndexBuffer(rd[8], R16, 0)` → `IASetVertexBuffers(0,1,rd,{(4*vd)&0x3C},{0})` → `DrawIndexed(2*count, 0, 0)`.

## Case 12 (GeometryType 13 — BSDynamicLines)
Fields: `geom+0x138` rendererData; `geom+0x160` dynamic data size (dword); `geom+0x164/0x168` spinlock tid/recursion; `geom+0x170` dynamic data ptr (qword, returned by `NiParticleSystem::GetModifierList` @0x141317e50 = lock(+0x164)+return *(+0x170)); `geom+0x158` lineCount (word).
```c
rd = *(geom+0x138);
dst  = FUN_140d6d5d0(renderer, rd, 0);        // alloc *(rd+0x1C) bytes, offset -> rd+0x18, Map NO_OVERWRITE
size = *(u32*)(geom+0x160);
src  = GetModifierList(geom);                  // lock, *(geom+0x170)
FUN_14130a030(dst, size, src, size);           // memcpy_s
FUN_140d6d5f0();                               // ctx->Unmap(dynVB[cur], 0)
FUN_140d6d620(renderer, rd, 0, (u16)*(geom+0x158));
FUN_141317e80(geom);                           // unlock (+0x164/+0x168)
```
`FUN_140d6d620` D3D11 sequence: vertexDesc dirty check (`rd+0x10`) → topology `2` → `SetDirtyStates(0)` → `IASetIndexBuffer(rd[8], R16, 0)` → `IASetVertexBuffers(0, 2, {rd[0], dynVB[cur]}, {(4*vd)&0x3C, (vd>>2)&0x3C}, {0, rd[0x18]})` → `DrawIndexed(2*count, 0, 0)` → `*(u32*)(rd+0x18) = -1` (offset reset; unlike case 3 the dynamic-lines upload is NOT frame-stamped — it re-uploads every draw).

---

## Utility-pass relevance ranking (as requested)
- Cases 4/5/6/7/8 all bottom out in the **already-REd `FUN_140d6bfe0` sequence** (or its `c0e0` twin differing only in IB source). The only new state they introduce is: computed start/count (4/5), the `pass+0x1C == 12` alternate-IB select (5/6), per-range multi-draw (7/8), and case 8's shadow-state PSTexture demotion writes (slots b and b+7, b=5..0) which flush through `SetDirtyStates`.
- Case 3/12/10/0/1 add the dynamic-VB ring allocator (Map WRITE_NO_OVERWRITE + event-query fencing at 4 MiB granularity, ring of 3) and, for 10/1, a manual `IASetInputLayout` + dirty-bit 0x400 repair.
- Case 9 is the only path that touches a constant buffer directly (Map DISCARD + `VSSetConstantBuffers(7,…)`) and the only `DrawIndexedInstanced` user in the dispatch.

## CAVEATS
- Geometry-type names (BSMeshLODTriShape, BSSubIndexLandTriShape, etc.) are inferred from the CommonLib/SSE GeometryType enum and behavior; no RTTI was consulted in this session. Offsets/behavior are ground truth; class names are best-effort labels.
- The `byte(pass+0x1C)==12` gate: field identity ("accumulationHint" per the project's BSRenderPass convention) and the meaning of value 12 were not traced to pass-creation writers; only the compare width (byte) and effect (alternate index set) are verified.
- `unk_14302C8E8`: verified as a NiSourceTexture* fallback in the BSGraphics::State block (SRV path `+0x48`→`+0x10` confirmed); which specific default texture it is (white/black/land default) was not traced into its initializer `FUN_1412eeb90`.
- `unk_1430243B0` semantics ("map-render draw-all") inferred from its writers `FUN_1404b35e0`/`FUN_1404b2960` (MapMenu / TESWorldSpace context); not exhaustively confirmed.
- `unk_1434963C8` has exactly one xref (the read in case 8) and no writer — stated as dead/always-0 on that basis; a runtime patch could theoretically set it.
- `PackParticleData_140d76080` and `sub_140D76D80` vertex-field semantics (which float is U vs subtexture index, normal vs binormal) are interpreted from layout/usage, not shader-side confirmation. The full math of PackParticleData (billboard axes, speed-to-aspect stretching path) is summarized, not exhaustively documented.
- `BSSubIndexTriShape::sub_140D59430` merge algorithm is summarized at the invariant level (coalesce enabled runs, head range carries summed count); the exact handling of the `enabled && next.active` break path was read but not re-derived case-by-case.
- In `FUN_140d6c8a0`, the per-buffer query/flag slots were decompiled as `qword_143025F18[74+i]` / `[77+i]`; the +73/+4 pointer arithmetic in the pseudocode makes the exact flag byte width (byte vs dword at those qword slots) slightly ambiguous, though functionally it is a boolean per ring buffer.
- Decompiler signatures for `FUN_140d6ca60`/`FUN_140d6cab0` were broken ("local allocation failed"); argument routing was reconstructed from disassembly (verified: offset from `desc+0x18`, start index in r9d, count on stack) — the r8 "out offset" pointer arguments passed by the dispatch (`&stackvar`, `shape+0x178`) are ignored by all callees and documented as vestigial.

### Machine-extracted caveats
- Geometry-type class names are inferred from the CommonLib GeometryType enum and observed behavior, not RTTI; offsets and D3D11 sequences are ground truth.
- byte(pass+0x1C)==12 gate: compare width (byte) and effect (alternate index set via FUN_140d6c0e0) are verified, but the field's identity as 'accumulationHint' and the semantics of value 12 were not traced to pass writers.
- unk_14302C8E8 is a fallback NiSourceTexture* in the BSGraphics::State block; which specific default texture it is was not traced into its initializer FUN_1412eeb90.
- unk_1430243B0 = map-render draw-all flag is inferred from writers FUN_1404b35e0/FUN_1404b2960 (MapMenu/TESWorldSpace context).
- unk_1434963C8 (case 8 single-draw path) has no writer in the binary and is treated as always 0 (dead path).
- PackParticleData_140d76080 and sub_140D76D80 vertex field semantics (U vs subtexture index etc.) are interpreted from layout, not confirmed against shader inputs; the billboard/aspect math is summarized rather than exhaustively documented.
- BSSubIndexTriShape::sub_140D59430 segment-coalescing is documented at invariant level; the merge break path was read but not re-derived edge-case by edge-case.
- FUN_140d6ca60/FUN_140d6cab0 decompiled with broken signatures; argument routing was reconstructed from disassembly (offset comes from desc+0x18; the out-offset pointers the dispatch passes, e.g. shape+0x178, are ignored/vestigial).
- In FUN_140d6c8a0 the per-ring-buffer query-ready flag slot width (byte vs dword at qword_143025F18[77+i]) is slightly ambiguous in the pseudocode; functionally one boolean per buffer.

================================================================================================
## Cluster 6
================================================================================================

# NiSkinInstance vfunc37 (Render, vtable +0x128) — skinned utility path FALSE branch — SkyrimSE 1.5.97

## Vtable + implementations found (addresses)

| Class | vtable (.rdata) | slot 37 addr | vfunc37 impl |
|---|---|---|---|
| NiSkinInstance | `0x141767CF0` (COL ptr at 0x141767CE8; 38 slots total, vfunc37 is the LAST) | `0x141767E18` | **`0x140C7E170`** |
| BSDismemberSkinInstance | `0x141767E28` (COL ptr at 0x141767E20) | `0x141767F50` | **`0x140C6B9F0`** |
| NiSkinPartition | `0x14176A0A0` (COL ptr at 0x14176A098) | `0x14176A1C8` | **`0x140C7CA70`** (the real per-partition draw; same slot index 37) |

Support functions:
- `0x140C7CA10` — partition loop helper (called by NiSkinInstance impl)
- `0x14131F630` — NiBoneMatrixSetterI vfunc1 = **SetBoneMatrix / bone-palette upload** (single shared impl referenced by 100+ shader vtables; BSUtilityShader's NiBoneMatrixSetterI vtable = `0x141868608`, slot1 at 0x141868610)
- `0x140D74F70` — NiSkinInstance::UpdateBoneMatrices (frame-gated CPU bone matrix build)
- `0x140D6FFD0` (`GetID3D11Resource`) — map a per-frame dynamic constant buffer from a ring
- `0x14131F7C0` — clears the per-thread "last skin instance" TLS cache (called at entry of skinned dispatcher 0x141308970)
- Geometry-data manager singleton: global ptr **`0x1430136C0`** → static object **`0x141E10A50`**, vtable **`0x14186BF80`**. Relevant entries: vfunc3(+0x18)=`0x141327F70`→`0x140D6BE60` (CreateTriShape), vfunc6(+0x30)=`0x141327FF0`→`0x140D6CAB0` (DrawDynamicTriShape), vfunc7(+0x38)=`0x141327FD0`→**`0x140D6BFE0`** (DrawTriShape — the already-REd standard leaf). All wrappers pass `&Renderer` = `0x143028490`. Set in shader-system init `FUN_141294060` at 0x1412940F3 (`unk_1430136C0 = &off_141E10A50`).
- Dynamic VB ring: map `0x140D6C8A0`, unmap `0x140D6C9E0`; `memcpy_s` = `0x14130A030`; SIMD copy = `0x14131FF40`.

## Call chain

`RenderPassImmediately_Skinned (0x141308970)` FALSE branch (`geometry->vfunc54(+0x1B0) == 0`):
1. `FUN_14131f7c0()` — TLS[0x2A00] (last-skin-instance cache) = 0 (done at function entry, both branches).
2. `BSRenderPass::FUN_141309f80(pass, shader, techniqueFlag, a3)` — shader technique setup (SetupTechnique/SetupGeometry, pre-existing RE).
3. Builds `drawStruct` on stack (layout below).
4. **If geometry is dynamic** (`v12 = geometry->vfunc12(+0x60)` ≠ 0): maps the 4MB dynamic VB ring (`FUN_140d6c8a0(&Renderer, size=*(v12+0x170), &drawStruct+0x24)` — returns dest ptr, writes ring offset into drawStruct+0x24), locks BSDynamicTriShape data (`0x140C723C0`: spinlock at +0x168, returns ptr at +0x160), `memcpy_s(dst, size, src, size)`, unmap (`0x140D6C9E0` = `ctx->Unmap(dynVB[idx],0)`), unlock (`0x140C72420`).
5. `skinInstance->vfunc37(skinInstance, &drawStruct)` where `skinInstance = *(geometry+0x130)`.
6. `shader->vfunc7(+0x38)(shader, pass, a3)` — RestoreTechnique/cleanup.

## NiSkinInstance::Render pseudocode

`0x140C7E170`:
```c
int32 NiSkinInstance::Func37(NiSkinInstance *this, DrawData *a2) {
  return FUN_140c7ca10(this->SkinPartition_18, a2);   // this+0x18 = NiSkinPartition*
}
```
`0x140C7CA10` (partition loop):
```c
int32 FUN_140c7ca10(NiSkinPartition *sp, DrawData *a2) {
  uint32 n = *(uint32*)(sp + 0x10);                    // partition count
  for (uint32 i = 0; i < n; ++i)
    result = (*(*sp + 0x128))(sp, a2, i);              // NiSkinPartition::vfunc37
  return result;
}
```

`NiSkinPartition::Func37 (0x140C7CA70)` — verified against disassembly:
```c
char NiSkinPartition::Func37(NiSkinPartition *this, DrawData *a2, uint32 idx) {
  Partition *p = *(this + 0x18) + 0x50 * idx;          // partition array @+0x18, stride 0x50
  // LOD gate:  byte_141E06650[ 12*a2->singleLevelLOD@0x18 + 3*a2->lodIndex@0x1C + p->LODLevel@0x42 ]
  if (!byte_141E06650[12 * *(dword*)(a2+0x18) + 3 * *(dword*)(a2+0x1C) + *(byte*)(p+0x42)])
    return 0;                                           // partition culled for this LOD
  // bone-palette upload callback (NiBoneMatrixSetterI vfunc1):
  //   rcx = *(a2+0)  iface,  rdx = *( *(a2+8) + 0x130 ) skinInstance,
  //   r8  = p (Partition*),  r9  = *(a2+8) + 0x7C (&geometry->worldTransform)
  (*(**(a2+0) + 8))(iface, skinInstance, p, &geom->world);
  dyn = geometry->vfunc12(+0x60)(geometry);             // geometry = *(a2+8)
  if (dyn) {  // dynamic (BSDynamicTriShape) skinned draw
    // vfunc6(+0x30) of *0x1430136C0: (mgr, p->rendererData@0x48, dyn+0x178 (UNUSED),
    //                                 startIndex=0, numTris=*(u16*)(p+0x3A), dynVBOffset=*(dword*)(a2+0x24))
    (*(*mgr + 0x30))(mgr, *(p+0x48), dyn+0x178, 0, *(u16*)(p+0x3A), *(dword*)(a2+0x24));
  } else {    // static skinned draw
    // vfunc7(+0x38): (mgr, p->rendererData@0x48, startIndex=0, numTris=*(u16*)(p+0x3A))
    (*(*mgr + 0x38))(mgr, *(p+0x48), 0, *(u16*)(p+0x3A));
  }
  return 1;
}
```

**LOD gate LUT** `byte_141E06650` (24 bytes = `[flag][lodIndex 0..3][partitionLOD 0..2]`):
```
flag=0 (multi-LOD):  lod0:{0,0,0}  lod1:{1,0,0}  lod2:{1,1,0}  lod3:{1,1,1}   // draw partitions with LODLevel < lodIndex
flag=1 (single-LOD): lod0:{1,0,0}  lod1:{0,1,0}  lod2:{0,0,1}  lod3:{0,0,0}   // draw only LODLevel == lodIndex
```

## BSDismember override delta

`BSDismemberSkinInstance::Func37 (0x140C6B9F0)`:
```c
if (*(qword*)(this+0x90) == 0)          // partition-data array (BSDismemberSkinInstance::Data*, 4-byte entries)
    return NiSkinInstance::Func37(this, a2);   // tail-jump to base
NiSkinPartition *sp = *(this+0x18);
uint32 n = *(dword*)(sp+0x10);
for (i = 0; i < n; ++i)
    if (*(BYTE*)(*(this+0x90) + 4*i))   // first byte of Data[i] (partFlag low byte; bit0=editor-visible) nonzero
        (*(*sp + 0x128))(sp, a2, i);    // same NiSkinPartition::vfunc37
```
Delta = per-partition visibility mask only (byte test on each 4-byte `{partFlag u16, bodyPart u16}` entry at this+0x90); base path when the array is null. No other behavior change.

## drawStruct layout (built at 0x141308A05..0x141308A29, stack rsp+0x30; consumed offsets confirmed in 0x140C7CA70 disasm)

| Offset | Type | Value / meaning |
|---|---|---|
| +0x00 | `NiBoneMatrixSetterI*` | `&shader->vftable_NiBoneMatrixSetterI` = **BSShader + 0x10** (BSUtilityShader: vtable 0x141868608); callee invokes its vfunc1 (+8) |
| +0x08 | `BSGeometry*` | pass->geometry (`*(pass+0x10)`); callee reads geom+0x130 (skinInstance), geom+0x7C (world NiTransform), geom vfunc12 |
| +0x10 | qword | 0 (not read by this path) |
| +0x18 | dword | `(pass->LODMode@0x1E >> 7) & 1` — single-level-LOD flag |
| +0x1C | dword | `pass->LODMode@0x1E & 0x7F` — LOD index |
| +0x20 | dword | 0 (not read by this path) |
| +0x24 | dword | dynamic-VB byte offset; **init -1**, overwritten by `FUN_140d6c8a0`'s out-param when geometry is dynamic; passed to DrawDynamicTriShape as slot-1 IASetVertexBuffers offset |

Bone-setter callback signature (vfunc1 at iface_vtbl+8): `void SetBoneMatrices(NiBoneMatrixSetterI* this, NiSkinInstance* skin /*rdx*/, NiSkinPartition::Partition* part /*r8*/, NiTransform* geomWorld /*r9*/)`.

## NiSkinPartition::Partition layout (stride 0x50; from Partition::LoadBinary FUN_140c7bc30 + draw usage)

| Offset | Field |
|---|---|
| +0x00 | u64 vertexDesc (NIF ver >= 100) |
| +0x08 | u16* bones (indices into skin bone palette) |
| +0x10 | float* weights |
| +0x18 | u16* vertexMap |
| +0x20 | u8* boneIndices (numVerts*weightsPerVert) |
| +0x28 | u16* triList indices |
| +0x30 | u16* stripLengths |
| +0x38 | u16 numVertices |
| +0x3A | u16 **numTriangles** (draw count source) |
| +0x3C | u16 **numBones** (bone-setter gate: `cmp word [r8+3Ch],0`) |
| +0x3E | u16 numStrips |
| +0x40 | u16 numWeightsPerVertex |
| +0x42 | u8 **LODLevel** (LUT gate byte) |
| +0x43 | u8 globalVB flag (ver >= 0x4F) |
| +0x48 | **rendererData** (BSGraphics::TriShape*, created at load via mgr vfunc3(+0x18)(mgr, vertexData, vertexDesc, triIndices, 3*numTris) = 0x140D6BE60) |

rendererData (48-byte alloc, 0x140D6BE60): **+0x00 ID3D11Buffer\* VB** (filled by FUN_140d4e060), **+0x08 ID3D11Buffer\* IB** (device->CreateBuffer: ByteWidth=2*numIndices, Usage=DEFAULT, BindFlags=INDEX_BUFFER, init=triList; device at Renderer+0x48, vfunc+0x18), **+0x10 u64 vertexDesc**, +0x28 CPU copy of u16 indices, +0x30 refcount=1. Per-partition IB ⇒ DrawIndexed always starts at 0.

## Partition draw D3D11 sequence (in order, per partition)

Context = qword at **0x143027EA0** (= `*(0x1430261B0 base)[926]`, same global block).

**A. Bone palette upload** — SetBoneMatrices `0x14131F630`, skipped when `TLS[0x2A00] == skinInstance` (cache; cleared once per pass-dispatch by 0x14131F7C0 ⇒ executes once per skin instance per pass, not per partition) or `part==NULL` or `part->numBones@0x3C == 0`:
1. `TLS[0x2A00] = skinInstance`; TLS[0x768] arena id set to 26 (restored at exit).
2. `UpdateBoneMatrices(skinInstance, geomWorld)` `0x140D74F70`: frame-gated (`skin->FrameId@0x38 != *(dword*)0x14302C8DC`), under `EnterCriticalSection(skin+0x60)`. Copies current→prev (`skin+0x48` → `skin+0x50`, 48 bytes/bone) or reallocs both (16-aligned, size@+0x44, count@+0x3C, numRegisters=3@+0x40); composes `xform = geomWorld * skinData->rootTransform(skinData+0x18) …` then per bone i: `m = boneWorld[i](skin+0x30 array) * skinData->boneData[i].skinToBone` (boneData at skinData+0x50, stride 0x58, bone count at skinData+0x58); stores 3×float4 rows `{R[r][0..2]*scale, pos[r]}` into `skin->BoneMatrices@0x48`.
3. rows = `3 * *(dword*)(skinData+0x58)` (skinData = skin+0x10).
4. `GetID3D11Resource(&Renderer 0x143028490, rows, &mapped, 10)` `0x140D6FFD0`: picks CB from **4-entry round-robin ring** at block+0x1858 (`0x143027A08 + 8*idx`), counter dword at `0x143027A00` (`(c+1)&3`); *size arg ignored*; stores chosen buffer in scratch `0x14302AC58`; **`ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD(4), 0, &mapped)`** (vfunc14, +0x70). (`kind==7` would use fixed buffer at 0x143027E90 — not this path; kinds 9/10 share the ring.)
5. copy `16*rows` bytes from `skin->BoneMatrices@0x48` (current) into mapped.
6. **`ctx->Unmap(cb, 0)`** (vfunc15, +0x78).
7. **`ctx->VSSetConstantBuffers(10, 1, &cb)`** (vfunc7, +0x38) — **b10 = current bone palette**.
8. Repeat 4–6 with kind 9 and `skin->PrevBoneMatrices@0x50`, then **`ctx->VSSetConstantBuffers(9, 1, &cb2)`** — **b9 = previous-frame bone palette**.

**B. Static draw leaf** — DrawTriShape `0x140D6BFE0` (mgr vfunc7 wrapper 0x141327FD0), args (rendererData, startIndex=0, numTris=part+0x3A):
1. If `RSS.vertexDesc@0x1430281F0 != rendererData->vertexDesc@0x10`: store it, `RSS.dirty@0x143027EB0 |= 0x400`.
2. If `RSS.topology@0x143028208 != 4` (TRIANGLELIST): store 4, `dirty |= 0x800`.
3. `BSGraphics::SetDirtyStates(0)` `0x140D705B0` — flushes all dirty state (input layout from vertexDesc, topology, CBs, etc. — pre-existing RE).
4. **`ctx->IASetIndexBuffer(rendererData->IB@0x08, DXGI_FORMAT_R16_UINT(57), 0)`** (vfunc19, +0x98).
5. **`ctx->IASetVertexBuffers(0, 1, &rendererData->VB@0x00, stride = (vertexDesc<<2)&0x3C, offset = 0)`** (vfunc18, +0x90).
6. **`ctx->DrawIndexed(3*numTris, 0, 0)`** (vfunc12, +0x60).

**B'. Dynamic draw leaf** — DrawDynamicTriShape `0x140D6CAB0` (mgr vfunc6 wrapper 0x141327FF0), args (rendererData, dyn+0x178 UNUSED, startIndex=0, numTris, dynOffset=drawStruct+0x24):
1–3. identical vertexDesc/topology dirty handling + `SetDirtyStates(0)`.
4. `ctx->IASetIndexBuffer(rendererData->IB, R16_UINT, 0)`.
5. **`ctx->IASetVertexBuffers(0, 2, {rendererData->VB, dynVBRing[idx]}, strides {(desc<<2)&0x3C, (desc>>2)&0x3C}, offsets {0, dynOffset})`** — dynVBRing = `qword[0x143025F18 + 8*idx]`, idx = dword@`0x143025F30`.
6. `ctx->DrawIndexed(3*numTris, startIndex(=0), 0)`.

**Dynamic VB ring map (dispatcher pre-step, 0x140D6C8A0)**: ring of 3 VBs at `0x143025F18[0..2]`, current index dword @`0x143025F30`, running offset dword @`0x143025F34`, cap **0x400000**; on overflow: `ctx->End(query[idx])` (vfunc28, +0xE0, queries at `0x143026168+8*idx`), advance idx mod 3, offset=0, clear signaled flag (bytes at `0x143026164+idx`); if flag clear: spin `ctx->GetData(query, &v,4, flags)` (vfunc29, +0xE8) with `Sleep(1)`; then `ctx->Map(dynVB[idx], 0, D3D11_MAP_WRITE_NO_OVERWRITE(5), 0, &m)`; returns `m.pData+offset`, out-param = offset (→ drawStruct+0x24), commits idx/new offset.

## Global state writes (replica must reproduce)

- `TLS[0x2A00]` (tls index at `0x143497408`): last-bound skin instance; **cleared at every skinned-pass dispatch entry**, set in bone setter. Governs bone-CB re-upload.
- `TLS[0x768]`: memory arena id save/set(26)/restore — allocation context only.
- RendererShadowState dirty dword `0x143027EB0`: `|=0x400` (vertexDesc), `|=0x800` (topology); consumed/cleared by SetDirtyStates.
- `0x1430281F0` (u64): current vertexDesc ← rendererData+0x10.
- `0x143028208` (dword): topology ← 4 (TRIANGLELIST).
- VS constant buffer bindings **b9 (prev bones)** and **b10 (current bones)** changed on ctx.
- Bone-CB ring counter `0x143027A00` (&3); scratch `0x14302AC58`.
- Dynamic-VB ring state `0x143025F30/0x143025F34`, signaled flags `0x143026164+i` (dynamic geometry only).
- `skin->FrameId@0x38` ← frame counter `*(dword*)0x14302C8DC`; `skin+0x48/+0x50` bone matrix arrays rewritten (CPU side, under skin+0x60 critsec).
- IA state on ctx: index buffer (R16_UINT), vertex stream 0 (+ stream 1 for dynamic).

## CAVEATS

- The class name of the manager object at `*0x1430136C0` (static instance 0x141E10A50, vtable 0x14186BF80) could not be resolved: its RTTI TypeDescriptor (RVA 0x1EBD968) is zero-filled in this unpacked dump. Functionally it is the BSGraphics geometry-data create/draw interface (CreateTriShape/DrawTriShape/DrawDynamicTriShape wrappers over Renderer 0x143028490).
- b10=current / b9=previous register assignment is inferred from copy order (skin+0x48 written by this frame's UpdateBoneMatrices goes to slot 10; skin+0x50, which is copied from the old +0x48 before recompute, goes to slot 9). Not cross-checked against shader disassembly here.
- `GetID3D11Resource` ignores its size argument; the ring CBs are preallocated at unknown size (creation site not chased). Kinds 9 and 10 draw from the SAME 4-buffer ring — the kind value only distinguishes 7.
- `FUN_140d4e060` (vertex-buffer creation inside CreateTriShape) was not decompiled; VB contents = per-partition SSE vertex data passed into Partition::LoadBinary.
- In the dynamic path, the `dyn+0x178` argument passed to DrawDynamicTriShape is dead (never read by 0x140D6CAB0).
- geometry vfunc12 (+0x60) "returns dynamic-shape data object" is derived from usage (+0x160 data ptr, +0x168 spinlock, +0x170 size dword); exact class (BSDynamicTriShape) inferred from idb names.
- LOD LUT is 24 bytes; a lodIndex > 3 (pass byte@0x1E & 0x7F) would index out of bounds — no clamp exists in 0x140C7CA70; game data presumably guarantees 0..3.
- The TRUE branch of the skinned dispatcher (geometry vfunc54/+0x1B0 ≠ 0, e.g. face-gen custom path) was out of scope and is not covered.
- UpdateBoneMatrices' exact skinData member offsets (rootTransform at skinData+0x18, boneData array at +0x50 stride 0x58, bone count at +0x58) are read from the decompile of 0x140D74F70; the two intermediate NiTransform::Multiply argument orders were not instruction-verified.

### Machine-extracted caveats
- Class name of the draw-manager object at *0x1430136C0 unresolved: RTTI TypeDescriptor (0x141EBD968) is zero-filled in the unpacked dump; identified functionally as the BSGraphics geometry create/draw interface over Renderer 0x143028490.
- b10=current / b9=previous bone-palette register assignment inferred from copy order in SetBoneMatrices; not cross-checked against VS shader disassembly.
- GetID3D11Resource ignores its size argument; kinds 9 and 10 share one 4-buffer round-robin CB ring whose creation site/size was not chased.
- FUN_140d4e060 (vertex-buffer creation inside CreateTriShape) not decompiled; VB contents assumed per-partition SSE vertex data.
- Dynamic path: the dyn+0x178 argument passed to DrawDynamicTriShape is dead (unused by 0x140D6CAB0).
- geometry vfunc12 (+0x60) semantics (returns dynamic-shape data with data@+0x160, spinlock@+0x168, size@+0x170) derived from usage and idb names (BSDynamicTriShape).
- LOD LUT is only 24 bytes; lodIndex (pass byte@0x1E & 0x7F) > 3 would read out of bounds - no clamp in code, assumed guaranteed 0..3 by game data.
- TRUE branch of the skinned dispatcher (geometry vfunc54 +0x1B0 != 0) out of scope, not analyzed.
- UpdateBoneMatrices skinData offsets (root xform +0x18, boneData +0x50 stride 0x58, count +0x58) taken from decompile; NiTransform::Multiply argument order not instruction-verified.


================================================================================================
## Verification corrections (apply these over the cluster text above)
================================================================================================

- **SetupAlphaBlendState (0x14131F440) opaque-branch pseudocode is wrong for `testEnable == false`.** The report claims the opaque path (`materialAlpha >= 1.0 && !blendEnable`) ends with `if (testEnable != alphaTestEnabled@F64) { F64 = testEnable; flags |= 0x100 }`. The actual code (disasm 0x14131F48E–0x14131F4FE) splits on `testEnable` (r10b): if **true**, it clears F58 (|=0x80 if changed) and sets F64 **to 1** (|=0x100) if not already 1; if **false**, it jumps to 0x14131F4D8 which **only clears F58 and returns — F64 is never touched**. So an opaque pass with test disabled leaves `alphaTestEnabled` stale (e.g. still true from a previous pass) instead of syncing it to false as the report states. The bidirectional sync exists only in the translucent tail (LABEL_39 @0x14131F601). This is load-bearing for exact replication: in the custom path `useAlphaTestBit` is hardcoded 1, so `testEnable=false` occurs for any geometry whose NiAlphaProperty lacks flag 0x200 (or is NULL), and a replica following the report would emit a spurious alpha-test disable + dirty 0x100 where the game leaves prior state in effect.
- Minor (address label only, not behavior): the `test byte [rsi+109h], 8` instruction is at **0x1413084EF** (jz at 0x1413084F6, call at 0x1413084F8), not "at 0x1413084F6/0x1413084F8" as the report's call-site line reads. Operands and semantics are as claimed.

Everything else re-derived and confirmed exactly: all cited addresses (0x141308440/B20/8C0/970/6C0, 0x141309F80, 0x141307160, 0x1412FD8A0, 0x14131F440/F2A0, 0x1412CCE20, ctors 0x140C79F70/9BB0/95C0/9CA0, clearers in 0x140C711A0/0x140C71A80/0x140C80AA0/0x140C80B80), all struct offsets (pass +0/+8/+0x10/+0x18/+0x1E; geom +0x108/+0x109/+0x120/+0x130/+0x148/+0x150; shaderProp +0x30/+0x78; alphaProp +0x30/+0x32), all globals (0x143283BA4/BA8, 0x141E0DF8C, 0x143490BB0, 0x143027EB0/F58/F64/F68, 0x143497408 TLS +0x768 marker 26, 0x14302C8E5, 0x1432336C0), dirty bits 0x80/0x100/0x200 (bts 7/8 + or 0x200), vtbl slots +0x10/+0x18/+0x20/+0x30/+0x38, register-level facts of the custom body (rdx clobber @0x141308B35, r8d live into vfunc+0x30, r9b=1, tail jmp [rax+38h]), call order, all translucent blend-mode mappings, ShaderSetup gates ((RenderFlags&4), passEnum 0x5C000058..0x5C00005B via `[rcx+18h]`, sky singleton), and the delta-vs-standard claims (no TLS in custom, order inversion, AlphaTest ignored, gates skipped).

- **Blend-state index formula, last term address is off by 4.** The report writes `deviceBlock[384 + 52*[0x143027F58] + 26*[0x143027F5C] + 2*writeMode_143027F60 + dw_143028484]` (cited both in the SetupGeometry flush paragraph and in the CAVEATS). The actual instruction at 0x140d708fa is `mov eax, cs:flt_143028470+10h`, i.e. the term is the dword at **0x143028480** (`flt_143028470[4]`), not 0x143028484. Corrected formula: `deviceBlock_1430261B0[384 + 52*[0x143027F58] + 26*[0x143027F5C] + 2*[0x143027F60] + dword[0x143028480]]`, applied via vtbl+0x118 (OMSetBlendState) with blendFactor &0x141E07168 and sample mask 0xFFFFFFFF at 0x140d70913. Everything else in that formula (coefficients, base 384, factor/mask, call site) verified correct.

All other load-bearing claims were re-derived from the binary and confirmed exactly: FUN_140d6fcf0 semantics (guarded Release via vtbl+0x10 + null of field +8; sole code caller 0x14130f3a5; idempotent; no null PSSetShader), vtable slot roles (base 0x1418685b0: slot2=0x14130DF90 Setup, slot3=0x14130DD80 RestoreTechnique, slot6=0x14130EC70 SetupGeometry, slot7=0x141310300 RestoreGeometry), the entire 0x1200 SetupGeometry branch (gate 0x14130f318, {1,0xFF} packed qword at 0x143027F40 with dirty|8, writeMode→0 with dirty|0x80, depthMode→0 with prev-compare bit 4 vs 0x143027F3C, VS b2 write of dword_141E0E014 at constantOffsets[7]=vs+0x57, release at 0x14130f3a5), the full D3D11 call order (Map vs+0x38 / ps+0x30 DISCARD at 0x14130ecdf/0x14130ed26; matrix transpose 0x14130ef1f skipped when F&4; alpha gate at 0x14130f74e/0x14130f764 with (F&0x80)==0 && (F&0x14000)!=0x10000; tail Unmap/Unmap/VSSetConstantBuffers(2,1)/PSSetConstantBuffers(2,1) at 0x14130f8a6..0x14130f90b), RestoreTechnique's 0x1200 undo at 0x14130def2 ({0,0xFF}, writeMode→1) + empty EndTechnique 0x14131fce0, RestoreGeometry's exclusion gate at 0x14131037e ((F&0x1200)!=0x1200 && pass+0x1C==10) and sentinel-13 stash at 0x141E10660 (static value 13, xrefs only 0x14130f292 + RestoreGeometry), the depth-mode leak (neither restore touches 0x143027F38 for pure 0x1200), SetupTechnique's F=passEnum−43, exact v6 expression, F stored at this+0x90/0x94 at 0x14130e0e4, nothing written to PerTechnique for pure 0x1200, b0 binds at 0x14130e6f4/0x14130e70a, both reducers (0x141334900: F&0xF7E5FF9F with the 0xDFFFE1E4 collapse gated exactly as claimed; 0x141334970: early 0x2000 collapse gate + F&0xFFFFFB83; shadow overrides never set bits 9|12 so the psid entry is exclusive), BeginTechnique table walks (VS buckets +0x50/size +0x34, PS +0x80/+0x64, returns 0 on miss), SetShader/SetPixelShader (stores to 0x1430281F8/0x143028200 = dword_143028070[98]/[100], immediate VSSetShader vtbl+88 / PSSetShader vtbl+72, dirty|0x400, no refcounting), DSS flush deviceBlock[40*depthMode+stencilMode] with ref 0x143027F44 at 0x140d707c9, dword_141E0E014 static 0 and FUN_1404c5660's clamp01 water-blend computation (bools +0xB8/+0xB9, float +0xC0, 1.0 branch at 0x1404c5932), and both statics' xref sets.

- No load-bearing errors found. Every cited address, struct offset, vtable index, D3D11 call order, and branch gate was re-derived from the binary and matches: the 13-case switch on byte geom+0x150−1 at 0x141307160; both static leaves (0x140d6bfe0 / 0x140d6c0e0 differing only in IB source); the ring allocator 0x140d6c8a0 (>0x400000 wrap, End idx 28, GetData idx 29, Map idx 14 mode 5, ring of 3, query slots [74+i]/flags [77+i]); ca10/d5d0 writing the offset to desc+0x18 with the r9 arg ignored; cab0/d620 two-stream binds with offsets {0, desc[0x18]}; the byte compares `cmp byte ptr [rcx+1Ch], 0Ch` at 0x1413074c3 and 0x141307559; the LOD helpers (lodSize@+0x160, pair array@+0x1D8, lod byte@+0x108 bit7, exact sum/3x formulas); case 7's ascending vs case 8's descending i=n−1..1 loop (segment 0 never drawn, geom[0x171]→nothing drawn), the bit 5→0 SRV demotion into qword_143027FF0[bit]/[bit+7] with dirty bits in 0x143027EB4; case 9's cb selection (a4==7 → 0x143027E90 = 0x1430261B0[924], else ring [779..782]/counter [778]), Map DISCARD, VSSetConstantBuffers(7,1) idx 7, DrawIndexedInstanced idx 20; the input-layout descriptors of ce60 (fmt 6@0/12/24, fmt 28@36, stride 40) and cbe0 (fmt 2@0/16, fmt 6@32, fmt 32@44 semidx 1, stride 48, DrawIndexed 6*(n>>2)); the layout cache 0x141E07140/44/60 + CRC sub_140C06570 over (curVertexDesc & VS+0x48); the static quad desc 0x14302AE50 (VB=0x143025F40, IB=0x143025F38, vertexDesc 0x0840200004000051); frame counter inc at 0x140d6a300 in FUN_140d6a2b0; 0x1434963C8 having exactly one xref (the case-8 read, no writer); and the SetDirtyStates PS-SRV flush region (0x143027EB4 read @0x140d70bec, qword_143027FF0 lea @0x140d70c07, PSSetShaderResources call @0x140d70c27).
- Two immaterial nuances, already covered by the report's caveats: (1) FUN_1404b35e0 both sets AND clears 0x1430243B0 (dl-dependent), and FUN_1408e8830/FUN_1408e89e0 also reference the flag — "set in 35e0 / cleared in 2960" is a simplification; (2) in case 8 the null-guard in the SRV fallback chain is on *(tex+0x48) (the rendererTexture), not on the 0x14302C8E8 texture pointer itself as the report's ternary implies (a null 0x14302C8E8 would fault).



================================================================================================
# PART III: shadow-map render loop → deferred context (Stage D)
# Own IDA analysis, adversarially cross-verified. APPLY the corrections block at the end over the reports.
================================================================================================


================================================================================================
## Shadow cluster 1
================================================================================================

# Shadow-map RT / viewport / depth-clear setup — SkyrimSE 1.5.97 (unpacked, base 0x140000000)

## Call chain (context — where the per-shadow-map D3D11 setup lives)

```
Main::DrawWorld 0x1405B1860
 ├─ NiCamera::CalculateAndDrawShadowCasterLights 0x1412E2660   (cull/accumulate ONLY, no D3D:
 │    ├─ per-list accumulation jobs 0x1412E2C50 / 0x1412E2DE0 on job threads
 │    └─ CalculateActiveShadowCasterLights 0x1412E2F60         (picks ≤4 lights/frame, calls light vfunc +0x48
 │                                                              = calc matrices; sets light+0x548 flags, unk_1432334D0=count)
 ├─ DrawWorld::MainAccum 0x1412E2A60
 │    └─ 0x1412e2ba6 → ShadowSceneNode_RenderAllShadowmaps 0x1412E3480   ★ THE SHADOW-MAP D3D LOOP (render thread)
 │         for each entry from ShadowSceneNode::GetShadowCasterLightArrayEntry 0x1412BC7D0:
 │             light->vfunc[+0x50] (RenderShadowmaps)          (TLS+1896 memory tag saved/set to 29 around loop)
 │               BSShadowDirectionalLight::Func10 0x141324C30  (cascade loop)
 │               BSShadowFrustumLight::Func10     0x14132D040
 │               BSShadowParabolicLight::Func10   0x14132E4E0  (2 paraboloid halves; ShadowSign 0x141E10B7C = ±1.0f,
 │                                                              ShadowRadius 0x141E10B78 = light->Radius)
 │                 └─ ALL call the shared per-map body BSShadowLight::RenderShadowmap 0x141305610
 ├─ Main::RenderDepth 0x1412E3520        (z-prepass: DS target 0 mode 0, then CopyResource depth→copy)
 └─ Main::RenderShadowmasks 0x1412E3AC0 → 0x1412E3B80 (mask passes into RT 18, BSUtilityShader 0x143495D50
                                                       via RenderPassImmediately 0x141308440 — reads the maps)
```

`Main::Swap 0x1405B1020` separately runs the precipitation-occlusion queue via `FUN_1405b29f0 0x1405B29F0` (same RT/DSV pattern, DS target **5**, ortho camera).

## Per-shadow-map body — BSShadowLight::RenderShadowmap 0x141305610

Args: `(this, shadowMapDesc a2, counter a3, renderFlags a4)`. `a2` = 240-byte shadow-map descriptor
(directional: array at `this+0x148`, count `this+0x140`; focus-shadow descriptors embedded at `this+0x160`, stride 240).
Descriptor fields used here: `+64` NiCamera*, `+72` BSShaderAccumulator*, `+84` (int) depth-stencil target index,
`+88` (int) slice, `+208/+212/+216/+220` sub-rect floats, `+232` byte "bind depth target" flag, `+233` byte.

```c
// 0x1413056c5: lazy slot allocation for point/spot lights
if (*(a2+84) == -1) {
    *(a2+84) = 4;                              // DS target 4 = the shadow-map slice pool
    if (dword_141E10538) {                     // free-slice bitmask @0x141E10538
        for (i=1, v9=0; (i & dword_141E10538)==0; ++v9) i*=2;
        dword_141E10538 &= ~i;
    }
    *(a2+88) = v9;                             // slice = lowest free bit
}
BSGraphics_Renderer_SetClearColors_140d6a6d0(0x143028490, ..., 1.0f,1.0f,1.0f,1.0f); // 0x141305711
if (*(a2+232)) {                                                        // 0x14130571d
    RenderTargetManager::SetDepthStencilRenderTarget(0x14302BB20, *(a2+84), 1 /*SRTM_CLEAR_DEPTH*/, *(a2+88)); // 0x14130573b
    RenderTargetManager::SetRenderTarget(0x14302BB20, 0, -1, 3 /*SRTM_RESTORE*/, 1 /*updateViewport*/);        // 0x141305754
    FUN_140d6a330(0x143028490, 0);             // 0x141305762 = Renderer::FlushOMAndClear (immediate flush, see below)
}
NiCamera::Render_1412C15C0(*(a2+64), *(a2+72), a4 | 0x400);             // 0x141305777 — draws the geometry
BSGraphics__Renderer_SetClearColorFromArray(0x143028490);               // 0x141305783 restore clear color
// ...then frustum/sub-rect math (+208..220 vs DS dims) feeding FUN_140d7bbc0(&0x14302C890, cam+160, rotCols, frustum, port)
// = BSGraphics::State shadow-camera data used later by the shadow-MASK projection (NOT by the map render itself)
```

Per-map-type target selection (writers of desc+84/+88 before calling 0x141305610):

- **BSShadowDirectionalLight::Func10 0x141324C30** — sun cascades:
  ```c
  if (dword_141E0E2E8 == 2)                    // volumetric-lighting quality (console TVL toggles it)
      for (i=0; i<cascadeCount; ++i) { desc[i]+84 = 3; desc[i]+88 = i; RenderShadowmap(this, desc[i], &local0, 0x100); }
  for (j=0; j<cascadeCount; ++j)     { desc[j]+84 = 2; desc[j]+88 = j; RenderShadowmap(this, desc[j], a2, 0); }
  if (this+0x550 byte)                          // focus shadows
      for (k=0; k<unk_1431D0FB8._used; ++k) { fdesc[k]+84 = 4; fdesc[k]+232 = 1; fdesc[k]+88 = k+4; RenderShadowmap(this, fdesc[k], &local0, 0); }
  ```
  So: cascade = **slice of DS target 2** (one full slice per cascade, NO atlas sub-rect); VL pre-pass = slices of DS target 3; focus shadows = DS target 4 slices 4+.
- **Frustum light 0x14132D040**: main desc (idx already set / lazily allocated to 4), plus focus descs (`word +232 = 0x0101`, idx 4, slice k+4).
- **Parabolic 0x14132E4E0**: two halves, same desc each time (idx 4 + pool slice).
- **Precipitation occlusion (FUN_1405b29f0 @0x1405b2ca2)**: `SetDepthStencilRenderTarget(5, mode 0-or-3, 0)`, RT0=-1.
- Z-prepass (`Main::RenderDepth` @0x1412e358f): `SetDepthStencilRenderTarget(GetDepthStencilTarget_MAIN()=0, 0 /*SRTM_CLEAR*/, 0)`.
  `GetDepthStencilTarget_MAIN 0x140D74E50` is literally `xor eax,eax; ret`.

## RT set

**RenderTargetManager::SetDepthStencilRenderTarget 0x140D74D10** `(mgr, targetIdx, mode, slice)` — pure S-block write:
```c
if (S.depthStencil != idx || S.setDepthStencilMode != mode || mode != 3 || S.depthStencilSlice != slice) {
    *(DWORD*)0x143027EB0 |= 1;            // stateUpdateFlags: DIRTY_RENDERTARGET
    S.depthStencil      = idx;            // 0x143027EE8
    S.depthStencilSlice = slice;          // 0x143027EEC
    S.setDepthStencilMode = mode;         // 0x143027F18
}
```
(Note: any mode != 3 re-dirties even if unchanged — guarantees the pending clear fires.)

**BSGraphics::Renderer::SetRenderTarget 0x140D74EC0** `(this=0x143028490, slot, targetIdx, mode, updateViewport)`
(thin wrapper `RenderTargetManager::SetRenderTarget 0x140D74CF0` forwards):
```c
if (mode == 5 /*SRTM_FORCE_COPY_RESTORE*/) { mode = 3;
    ctx->CopyResource(*(this+48*idx+2656), *(this+48*idx+2648)); }   // vfunc +0x178
if (S.renderTargets[slot] != idx || S.setRenderTargetMode[slot] != mode || mode != 3) {
    S.renderTargets[slot]       = idx;    // 0x143027EC8 + 4*slot
    S.cubeMapRenderTarget       = -1;     // 0x143027EF0
    S.setRenderTargetMode[slot] = mode;   // 0x143027EF8 + 4*slot
    *(DWORD*)0x143027EB0 |= 1;
}
if (updateViewport) Renderer::UpdateViewPort(this, 0, 0, 0);
```
The shadow body sets **RT0 = -1** (no color target); OMSetRenderTargets is then issued with **NumViews=0** and only the DSV.

**Actual OM bind + pending clears** happen in either flush:
- **FUN_140d6a330 0x140D6A330** (`Renderer::FlushOMAndClear(this, onlyRenderTarget)`) — called explicitly at 0x141305762 with a2=0, so it flushes OM **and** depth-stencil state, raster state, viewport, blend, immediately, on the immediate context `*(ID3D11DeviceContext**)0x143027EA0`;
- **BSGraphics::SetDirtyStates 0x140D705B0** — same logic, run before every subsequent draw of the accumulated passes.

The shared bit-1 (render target) logic, with `data = *(void**)0x143025F00` (= 0x1430284A0 = Renderer+0x10):
```c
if (S.stateUpdateFlags & 1) {
  if (S.cubeMapRenderTarget == -1) {
    for (i = 0; i < 8; ++i) {
      idx = S.renderTargets[i]; if (idx == -1) break;
      rtv[i] = *(data + 48*idx + 2648);                       // RTV array
      if (S.setRenderTargetMode[i] == 0 /*SRTM_CLEAR*/) {
        ctx->ClearRenderTargetView(rtv[i], data+10088);       // vfunc +0x190; clear color @0x14302AC08
        S.setRenderTargetMode[i] = 4; } }
  } else { i = 1; rtv[0] = *(data + 8*(S.cubeMapRenderTargetView + 8*S.cubeMapRenderTarget) + 9936); ... }
  if (S.setDepthStencilMode <= 2 || == 6) *(byte*)(data+34) = 0;   // clear the "read-only depth" flag
  dsv = 0;
  if (S.depthStencil != -1) {
    rec = S.depthStencilSlice + 19*S.depthStencil;
    dsv = *(byte*)(data+34) ? *(data + 8*rec + 8176)          // read-only DSV
                            : *(data + 8*rec + 8112);         // normal DSV  (abs: 0x14302A450 + 8*rec)
    if (dsv) switch (S.setDepthStencilMode) {
      case 0: case 6: flags = 3; goto clear;                  // DEPTH|STENCIL
      case 1:         flags = 1; goto clear;                  // DEPTH
      case 2:         flags = 2; goto clear;                  // STENCIL
      clear: ctx->ClearDepthStencilView(dsv, flags, 1.0f, 0); // vfunc +0x1A8 — see ## Depth clear
             S.setDepthStencilMode = 4; } }
  ctx->OMSetRenderTargets(i, rtv, dsv);                       // vfunc +0x108
}
```

## Viewport set

**Renderer::UpdateViewPort 0x140D69D00** `(this, width, height, ForceMatchRenderTarget)` — the ONLY writer of the S viewport (write @0x140d69de4):
```c
sx = sy = 1.0f;
if (width) { W = width; H = height; }
else {
  if (!ForceMatchRenderTarget && !*(byte*)0x14302C9A0)      // dynamic-resolution enabled
      { sx = *(float*)0x14302C98C; sy = *(float*)0x14302C990; }   // DRS width/height scale
  if (S.cubeMapRenderTarget == -1) { W = GetCurrentRTWidth(); H = GetCurrentRTHeight(); }   // 0x140D74C20/0x140D74C60
  else { W/H from cubemap target (0x140D74CB0/0x140D74CD0); }
}
*(DWORD*)0x143027EB0 |= 2;                                   // DIRTY_VIEWPORT
S.vp.TopLeftX = port.x0 * W;                                 // 0x143027F20 = [0x143028460] * W
S.vp.TopLeftY = (1.0f - port.y1) * H;                        // 0x143027F24 = (1 - [0x143028468]) * H
S.vp.Width    = (port.x1 - port.x0) * W * sx;                // 0x143027F28
S.vp.Height   = (port.y1 - port.y0) * H * sy;                // 0x143027F2C  ([0x143028468]-[0x14302846C])
```
Dimension source (**this is how the shadow map size gets in**), `FUN_140d74c20/0x140D74C60`:
```c
if (S.renderTargets[0] != -1) return *(mgr + 28*S.renderTargets[0] + 0 /*or +4*/);       // RT props, 28B/rec @0x14302BB20
if (S.depthStencil > 12) return 0;
return *(mgr + 16*S.depthStencil + 3192 /*or +3196*/);       // DS target props, 16B/rec @0x14302C798: {width,height,...}
```
Since the shadow body sets RT0 = -1, the viewport becomes the **full depth-target slice**: `(0, 0, W, H)` for the default full port `{0,1,1,0}`. There is **no per-cascade TopLeftX/Y offset** — cascade/face selection is entirely via the **DSV slice index** (`S.depthStencilSlice → dsv = data + 8*(slice + 19*idx) + 8112`).

Two viewport recomputations happen per shadow map:
1. inside `SetRenderTarget(0,-1,3,1)` → `UpdateViewPort(0,0,force=0)` — **DRS scale applied**, immediately flushed by FUN_140d6a330's bit-2 branch (`ctx->RSSetViewports(1, &S.vp @0x143027F20)`, vfunc +0x160, @0x140d6a63f);
2. inside `NiCamera::Render 0x1412C15C0 → sub_1412C1600 0x1412C1600`: `SetCameraData(0x14302C890, cam, flags)` then, because the shadow path passes `flags|0x400`, `UpdateViewPort(0x143028490, 0, 0, ForceMatchRenderTarget=1)` — **no DRS scale**; flushed by SetDirtyStates before the first accumulated draw (`RSSetViewports` @0x140d708bc).

So a deferred replica must set: `D3D11_VIEWPORT{0, 0, dsWidth, dsHeight, MinDepth, MaxDepth}` per map (dims from mgr+3192 table), where **MinDepth/MaxDepth** come from the camera depth-range globals: in both flush paths (raster branch, bits 0x1070 incl 0x40) `S.vp.MinDepth(0x143027F30) = [0x143028470]`, `S.vp.MaxDepth(0x143027F34) = [0x143028474]`, then if `S.depthBiasMode(0x143027F50) != 0`: `MaxDepth -= *(float*)(0x143026180 + 4*mode)` (per-mode depth-bias table) and dirty|=2. `BSGraphics::Renderer::SetMinandMaxViewportDepth 0x140D69E50` does the same copy standalone. The port rect + depth range live in the Renderer camera-state block **0x143028230 (0x250 bytes)** at +0x230..+0x244; they are rewritten per camera by `State::SetCameraData 0x140D7BAB0 → FUN_140d7d430 0x140D7D430 → FUN_140d7bef0 0x140D7BEF0` (copies `NiCamera::Port_174` into block+0x230 via `FUN_140d7d8c0` compare-and-update).

## Depth clear

There is **no direct engine wrapper call** for the shadow clear — it is a *pending-clear* encoded in `setDepthStencilMode` and executed by the next flush:
- `SetDepthStencilRenderTarget(idx, **1**, slice)` = SRTM_CLEAR_DEPTH → on flush: `ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH(1), **1.0f**, **0**)` then mode←4 (consumed).
- Verified args at the callsites (vfunc **+0x1A8** = index 53):
  - FUN_140d6a330 @**0x140d6a4b4**: `rdx=dsv, r8d=flags(3/1/2 from mode 0|6 /1 /2), xmm3 = dword[0x1415232D8] = 0x3F800000 (1.0f), stack arg5 = 0` (disasm 0x140d6a4a4-0x140d6a4b1).
  - SetDirtyStates @**0x140d70765**: identical (`movss xmm3, cs:0x1415232D8; mov byte [rsp+a5], r15b(=0)`).
- For the shadow maps the clear is issued **inside RenderShadowmap by the explicit `FUN_140d6a330(0x143028490, 0)` call at 0x141305762** (i.e., before NiCamera::Render), together with `OMSetRenderTargets(0, {}, dsv)`, `OMSetDepthStencilState` (+0x120), `RSSetState` (+0x158), `RSSetViewports` (+0x160), `OMSetBlendState` (+0x118).
- The white `SetClearColors(1,1,1,1)` @0x141305711 (writes data+10088 = 0x14302AC08, saving old to 0x143025EF0..FC; restored by `SetClearColorFromArray 0x140D6A740`) only matters if a color RT with SRTM_CLEAR were bound — none is for shadow maps.
- Z-prepass uses mode 0 → flags 3 (DEPTH|STENCIL), same 1.0f/0 values.

## State globals (offsets)

S-block = RendererShadowState dirty region, base **S = 0x143027EB0**:
| Abs addr | S ofs | Field |
|---|---|---|
| 0x143027EB0 | +0x00 | stateUpdateFlags (bit0 RT/DSV+pending clears, bit1 viewport, bits2-3 depth-stencil state, bit4 (0x10) depth-enable part of DSS, bit5 (0x20) cull, bit6 (0x40) depth-bias→viewport, bit7 (0x80) blend, 0x100/0x200 alpha-test CB, 0x400 VS/IL, 0x800 topology, 0x1000 raster) |
| 0x143027EB4 | +0x04 | PSResourceModifiedBits (PSSetShaderResources, srvs @0x143027FF0[16]) |
| 0x143027EB8 | +0x08 | PSSamplerModifiedBits (indices 0x143027F6C[16]/0x143027FAC[16]) |
| 0x143027EBC..C4 | +0x0C..+0x14 | CS resource/sampler/UAV modified bits |
| 0x143027EC8 | +0x18 | renderTargets[8] (int, -1 = none) |
| 0x143027EE8 | +0x38 | **depthStencil** (DS target index) |
| 0x143027EEC | +0x3C | **depthStencilSlice** |
| 0x143027EF0 / F4 | +0x40/+0x44 | cubeMapRenderTarget / cubeMapRenderTargetView |
| 0x143027EF8 | +0x48 | setRenderTargetMode[8] (0=CLEAR,1=CLEAR_DEPTH,2=CLEAR_STENCIL,3=RESTORE,4=NO_CLEAR/consumed,5=FORCE_COPY_RESTORE,6=INIT) |
| 0x143027F18 | +0x68 | **setDepthStencilMode** |
| 0x143027F1C | +0x6C | setCubeMapRenderTargetMode |
| 0x143027F20 | +0x70 | D3D11_VIEWPORT {X, Y @+0x74, W @+0x78, H @+0x7C, MinDepth @+0x80, MaxDepth @+0x84} |
| 0x143027F38/3C/40/44 | +0x88.. | depth-stencil-state indices (a/b/stencilRef) |
| 0x143027F48/4C/50/54 | +0x98.. | raster indices: scissor?, cullMode, **depthBiasMode** (0x143027F50, also offsets vp.MaxDepth), fillMode |
| 0x143027F58/5C/60 | +0xA8.. | blend indices (alphaBlendMode/AlphaBlendModeExtra/WriteMode; 0x143027F60 heavily toggled around shadow/mask passes) |

Renderer / data globals:
- Renderer singleton `this` = **0x143028490**; RendererData pointer `*(void**)0x143025F00` = **0x1430284A0** (= this+0x10).
- Immediate context: `*(ID3D11DeviceContext**)0x143027EA0` (== qword index 926 of 0x1430261B0).
- RT records: stride 48, for idx: texture `data+2632+48i`, copy-texture `data+2640+48i`, **RTV `data+2648+48i`** (abs 0x143028F38+48i); clear color float4 @ `data+10088` = **0x14302AC08** (saved copy 0x143025EF0).
- **Depth-stencil records**: base `data+8104` = **0x14302A448**, stride 152 (19 qwords): `+0` ID3D11Texture2D*, `+8` **DSV[slice 0..7]** (abs 0x14302A450 + 152*idx + 8*slice), `+72` read-only DSV[0..7], remainder SRVs. Read-only selector byte `data+34`.
- Cubemap RTVs: `data + 9936 + 8*(view + 8*idx)` (abs 0x14302AB70).
- RenderTargetManager = **0x14302BB20**: RT props 28 B/rec at +0 {width,height,...}; **DS target props 16 B/rec at +3192** (abs 0x14302C798): {width, height, ...}, max index 12.
- Camera-state block **0x143028230** (0x250 B): port rect `+0x230` = 0x143028460 (x0), 0x143028464 (x1), 0x143028468 (y1/top), 0x14302846C (y0/bottom); depth range `+0x240/0x244` = **0x143028470 / 0x143028474** (→ vp.MinDepth/MaxDepth). Depth-bias table (floats, indexed by 0x143027F50): **0x143026180**.
- DRS: scale x/y **0x14302C98C/0x14302C990**, disable flag byte **0x14302C9A0** (`flags&0x400` shadow path bypasses DRS via ForceMatchRenderTarget anyway).
- Shadow slice pool free-mask (DS target 4): **dword_141E10538**; focus-shadow set: unk_1431D0FB8 (+enable byte 0x141E0DE43); VL quality driving the target-3 pre-pass: **dword_141E0E2E8** (==2); active-caster count 0x1432334D0; per-frame single-caster flags 0x1432334E1/E2, last light 0x143283B80; parabolic globals ShadowRadius 0x141E10B78 / ShadowSign 0x141E10B7C; mask-slice global 0x143283B78 (written in the mask pass from desc+88).

Key vfunc offsets on the context (for the deferred replica): OMSetRenderTargets +0x108, OMSetBlendState +0x118, OMSetDepthStencilState +0x120, RSSetState +0x158, RSSetViewports +0x160, ClearRenderTargetView +0x190, ClearDepthStencilView +0x1A8, CopyResource +0x178.

Deferred-replica contract per shadow map/cascade: `OMSetRenderTargets(0, nullptr, DSV(idx,slice))` → `ClearDepthStencilView(DSV, D3D11_CLEAR_DEPTH, 1.0f, 0)` (only when desc+232 set; mode-1 semantics) → `RSSetViewports(1, {0,0,dsW,dsH, minD=[0x143028470], maxD=[0x143028474]−biasTable[depthBiasMode]})` → depth-stencil/raster/blend states per S indices → then the accumulated pass walk (BeginPass 0x141308030 → RenderPassImmediately 0x141308440) with SetDirtyStates-equivalent per-pass flushing. After the loop, nothing restores the DSV inside 0x1412E3480 — the next consumer (Main::RenderDepth) re-sets DS target 0 itself.

## CAVEATS

- `dword_141E0E2E8` is confirmed volumetric-lighting-related (console `FunctionToggleVolumetricLighting` and VolumetricLightingParams touch it), so the `==2` cascade pre-pass into DS target 3 is the VL half-res shadow copy — but I did not trace which INI setting feeds it, and the exact meaning of renderFlags 0x100 passed to those RenderShadowmap calls (it propagates into NiCamera::Render flags as 0x500) was not followed into the accumulator.
- DS-target index names (2 = cascades/SHADOWMAPS_ESRAM-like, 3 = VL half-res, 4 = shadow-map slice pool, 5 = precipitation occlusion, 0 = main) are inferred from usage, not from strings; the CommonLib enum mapping was not re-verified against the target-creation site.
- Descriptor fields +208..+220 (sub-rect floats) feed the post-render frustum-scaling math (0x1413057e6-0x141305898) and `FUN_140d7bbc0` (shadow camera data at 0x14302C890 for mask projection); they do NOT affect the RT/viewport of the map render. The gating virtual `Func7_38` (skips the adjust) and the exact semantics of `*(cam+336/352/360)` (NiCamera frustum fields) were not fully decoded.
- The DS record tail (data+8104 record qwords 17-18, presumed depth/stencil SRVs) and the RT record fields beyond {tex, copyTex, RTV} were not individually verified.
- Freeing of pool slices in dword_141E10538 (presumably light vfunc +0x60 called after the mask pass at 0x1412e3e0e) was not traced.
- `flt_143028470[0]/[1]` (vp Min/MaxDepth source) read 0/0 statically; they are runtime-written via the camera-state update (block 0x143028230+0x240 path through FUN_140d7d8c0). I did not enumerate every writer; shadow passes rely on whatever SetCameraData installed for the shadow camera (normally 0.0/1.0).
- Whether cascadeCount (`dirLight+0x140`) is 2 in practice (iShadowSplitCount) was not checked; the code loops generically.


### Caveats
- dword_141E0E2E8==2 pre-pass into DS target 3 is volumetric-lighting-related (console toggle + VL params reference it), but the feeding INI setting and the meaning of renderFlags 0x100 were not traced further.
- DS-target index naming (2 cascades, 3 VL half-res, 4 slice pool, 5 precip occlusion, 0 main) inferred from usage, not verified at the target-creation site.
- Descriptor fields +208..+220 affect only the post-render shadow-camera data (mask projection), not the map's RT/viewport; the gating virtual Func7_38 and NiCamera +336/352/360 fields were not fully decoded.
- DS record qwords 17-18 (presumed depth/stencil SRVs) and RT record fields beyond tex/copyTex/RTV unverified.
- Pool-slice freeing in dword_141E10538 (likely light vfunc +0x60 after the mask pass) not traced.
- 0x143028470/74 (viewport depth range) are runtime-written through the camera-state block update; not every writer enumerated — shadow passes use whatever SetCameraData installed (normally 0.0/1.0).
- Actual cascade count at dirLight+0x140 not checked against INI; code loops generically.

================================================================================================
## Shadow cluster 2
================================================================================================

# Shadow-map deferred-context integration surface — SkyrimSE 1.5.97 (base 0x140000000)

## Detour point (addr, once-per-frame proof)

**Detour target: `FUN_1412E3480` @ 0x1412E3480** — "render all shadow maps for this frame". Sole content is the shadow-caster-light loop:

```c
BSShadowParabolicLight *FUN_1412e3480()
{
  v0 = shadowSceneNode;                                   // qword ptr global 0x141E0DED0
  v1 = (TLS[MEMORY[0x143497408]] + 1896);                 // memory-context tag
  v2 = *v1; *v1 = 29;                                     // tag = 29 around the loop
  a2 = 0;
  for ( result = ShadowSceneNode::GetShadowCasterLightArrayEntry_1412BC7D0(v0, 0);
        result;
        result = ShadowSceneNode::GetShadowCasterLightArrayEntry_1412BC7D0(v0, a2) )
  {
    (result->vftable->Func10_50)(result, &a2);            // vfunc 0xA = BSShadowLight::RenderShadowmaps
  }
  *v1 = v2;
  return result;
}
```

`GetShadowCasterLightArrayEntry_1412BC7D0` is trivial: `return *(a1->shadowCasterLights.Data_0 + a2);` (BSTArray on the ShadowSceneNode; the per-light `RenderShadowmaps` advances `a2`).

**vfunc 0xA (vtable +0x50) implementations** (resolved from vtables + AddressLibrary):
- `BSShadowDirectionalLight::RenderShadowmaps` = **0x141324C30** (AddrLib id 101495; vtable 0x14186BBD8 entry 10). CS's ShadowmapCascadeRasterizerFix hooks its call at +0xC6 = 0x141324CF6.
- `BSShadowFrustumLight::RenderShadowmaps` = **0x14132D040** (vtable 0x14186C690 entry 10).
- `BSShadowParabolicLight::RenderShadowmaps` = **0x14132E4E0** (vtable 0x14186C7A0 entry 10).
- None of the three has any code xref — vtable/data refs only. The loop's virtual dispatch is the only static call path.

Directional body (0x141324C30): three sub-loops, all funneling into the shared per-shadowmap renderer `sub_141305610` @ 0x141305610:
```c
if ( dword_141E0E2E8 == 2 )                 // ESM mode: extra pre-pass, type 3, flags 256
  for ( i = 0; i < numCascades /* +0x140 */; ++i ) sub_141305610(this, cascade_i, &tmp, 256);
for ( j = 0; j < numCascades; ++j )         // main cascades: type 2, id j       <- CS hook +0xC6
  sub_141305610(this, cascade_j /* 240B descriptors @ +0x148 */, a2, 0);
if ( this->_pad_8[1360] )                   // focus shadows: type 4, count unk_1431D0FB8._used
  for ( k ... ) sub_141305610(this, focus_k, &tmp, 0);
```
`sub_141305610` per shadow map: releases the light's pass-array refs, **allocates a depth-slice from the global bitmask `dword_141E10538`**, `Renderer::SetClearColors(0x143028490)`, `RenderTargetManager::SetDepthStencilRenderTarget(0x14302BB20, target=*(a2+84), 1, slice=*(a2+88))` + `SetRenderTarget(0, -1, 3, 1)` + clear, then **`NiCamera::RenderPreAndPostResolveDepth_1412C15C0(*(a2+64) /*shadow cam*/, *(a2+72) /*accum list*/, a4|0x400 /*RENDER_SHADOWMAP*/)`** — which walks batches through the known BeginPass 0x141308030 / RenderPassImmediately 0x141308440 machinery — then writes shadow camera data via `FUN_140D7BBC0(&0x14302C890, ...)` (eye-pos mirrors 0x143028260..0x1430282D0) and stores the ShadowMapProj matrix back into the cascade descriptor (`*a2..*(a2+48)`).

**Once-per-frame proof (single-xref chain at every level):**
- `FUN_1412E3480` ← one code xref: 0x1412E2BA6 inside `DrawWorld::MainAccum` 0x1412E2A60.
- `MainAccum` ← one code xref: **0x1405B1B4C = `Main::Draw_1405B1860` + 0x2EC** (this is exactly CS's `Main_RenderShadowMaps` write_thunk_call site, RelocationID(35560)+0x2EC; AddrLib 35560 → 0x1405B1860 confirmed).
- `Main::Draw_1405B1860` ← one code xref: 0x1405B182D in `Main::sub_1405B1710` (sets 0x1430243E0=1, caches camera, calls Draw).
- `sub_1405B1710` ← two code xrefs, both inside the per-frame render entry `Main::Swap_1405B1020`: the normal in-game path at 0x1405B12FC, and the menu-world-snapshot path via `Main::sub_1405B1650` (0x1405B16C8), which is in the mutually-exclusive `if (menuMode)` branch and latched by byte 0x142F26B75 (runs the world draw once when a menu needs it). So the shadow loop runs **at most once per frame, always on the render thread**.

**What surrounds it in `MainAccum` (0x1412E2A60):**
- Before: main accumulator 0x143233400 gets flag+camera; two `BSAccumProcess::DoSceneListAccumRegisterJob_1412D6FB0` jobs queued on JobList 0x143233208 (accum 0x1432333F8 bucket 0, accum 0x143233400 bucket 1) and submitted. **The shadow loop then runs on the render thread concurrently with those accumulation jobs** (already-parallel by design; the per-light pass lists were built earlier by `NiCamera::CalculateAndDrawShadowCasterLights_1412E2660` = AddrLib 100414, one culling job per light).
- After: `JobList::Finish(0x143233208)`, then an optional front-to-back z-prepass block gated by `bEnableFrontToBackPrepass:Display` (data byte 0x141E0E818, default 0; sole xref 0x1412E2BF0) calling `FUN_1412CD0E0(accum 0x1432333F8, 17)` and `(…, 9)`. Not shadow work.

**What surrounds the `MainAccum` call in `Draw_1405B1860`:** before = `BSGraphics::State::SetCameraData(0x140D7BAB0)` at 0x1405B1B47; after = volumetric-lighting render (if 0x143232EF0), `Main::RenderDepth` 0x1412E3520, `Main::RenderShadowmasks` 0x1412E3AC0 (AddrLib 100422; consumes the shadow maps), `Precipitation::RenderOcclusion` thunk 0x1412E43F0.

**Suppress/replace semantics:** detouring 0x1412E3480 removes all shadow-map D3D11 work (no other producer exists); its non-D3D side effects that downstream depends on are (a) the produced depth textures, (b) depth-slice allocations consumed from `dword_141E10538`, (c) ShadowMapProj matrices written into the cascade descriptors (read later by the shadow-mask constant setup), (d) per-light pass-array release. Re-running the original function with the context global redirected preserves all of these exactly.

## Device + deferred-context feasibility

Device creation: `FUN_140D718D0` (import thunk `D3D11CreateDeviceAndSwapChain` 0x14135BB22, .idata 0x14150A0A0):
```c
Flags v7 = 0;  if ( dword_141E072B8 /*debug ini*/ ) v7 = 2;   // D3D11_CREATE_DEVICE_DEBUG only
D3D11CreateDeviceAndSwapChain(pAdapter, D3D_DRIVER_TYPE_UNKNOWN, 0, v7,
    /*pFeatureLevels*/0, /*count*/0, /*SDK*/7, &scDesc, base+96, base+56, &pFeatureLevel, base+64);
...
MEMORY[0x143025F08]      = *(base+56);   // ID3D11Device*        @ 0x140D71CC8
MEMORY[0x1430261B0][926] = *(base+64);   // ID3D11DeviceContext* @ 0x140D71CD8  (== 0x143027EA0)
```
`D3D11_CREATE_DEVICE_SINGLETHREADED` (0x1) is **never set** → `ID3D11Device::CreateDeferredContext` (device vfunc 27, +0xD8) is legal on this device. Feature levels default (11_0 first). `base` = the struct pointed to by 0x143025F00 (adapter also mirrored to 0x143027E98 = 0x1430261B0[925]; hwnd-block ptr to 0x143025F10). Under CS the d3d11 implementation is DXVK, which reports full command-list support (driver command lists native), so FinishCommandList/ExecuteCommandList are first-class; on stock runtimes they'd be emulated if the driver lacks support (correct, slower).

## Immediate-context pointer storage (redirect target)

The immediate context lives in exactly one engine global: **qword 0x143027EA0** (aka `MEMORY[0x1430261B0][926]`, i.e. 0x1430261B0+0x1CF0). 355 xrefs total; the only store is the init at **0x140D71CD8**, plus teardown-path refs in `Renderer::Shutdown` 0x140D69140 / `KillWindow` 0x140D696B0 / `WindowSizeChanged` 0x140D698A0. Every use site re-loads it fresh per call — verified pattern in `BSGraphics::SetDirtyStates` 0x140D705B0 and the query helpers:
```asm
mov rcx, cs:143027EA0h
mov rax, [rcx]
call qword ptr [rax+190h]   ; etc.
```
Therefore swapping the qword at 0x143027EA0 to a deferred context for the duration of the detoured shadow scope (render thread only) redirects **all** engine D3D11 calls issued inside that scope — SetDirtyStates, Renderer::SetVertexShader/SetPixelShader, Map/Unmap of the bone-CB ring 0x143027A00 and dynamic-VB ring 0x143025F30, RT/DS binds, clears, DrawIndexed — with no other engine-side cached copy observed. Restore the qword before `Main::RenderDepth`, then `ExecuteCommandList` on the immediate context. NOTE: CS's own `globals::d3d::context` is a separate cached pointer that stays immediate — see gate section.

## ExecuteCommandList/FinishCommandList indices

**The expected "133 / 0x428 and 134 / 0x430" is wrong for `ID3D11DeviceContext`** (those indices land inside the `ID3D11DeviceContext1` extension block; 133 = DiscardView1). The binary provably uses the canonical d3d11.h vtable: offsets observed in engine code map exactly to `ClearRenderTargetView` +0x190 (idx 50), `ClearDepthStencilView` +0x1A8 (53), `OMSetRenderTargets` +0x108 (33), `OMSetDepthStencilState` +0x120 (36), `RSSetState` +0x158 (43), `Begin` +0xD8 (27), `End` +0xE0 (28). Under that layout:
- **`ExecuteCommandList` = vfunc 58, offset +0x1D0** (between ResolveSubresource 57 and HSSetShaderResources 59) — call on the immediate context.
- **`FinishCommandList` = vfunc 114, offset +0x390** (last ID3D11DeviceContext method) — call on the deferred context.
- `ID3D11Device::CreateDeferredContext` = vfunc 27, +0xD8.
The engine itself never calls either (no deferred-context usage anywhere). CS code should just call the COM methods via headers; raw indices only matter if patching engine-side dispatch.

## CS-substitution bypass gate

Engine `BSShader::BeginTechnique` = **0x14131FBD0** (AddrLib 101341, confirmed). Body: looks up vertex shader (hash map at BSShader+0x50) and pixel shader (+0x80), then `Renderer::SetVertexShader(0x140D6F9B0)` at **+0xC3** (0x14131FC93) and `Renderer::SetPixelShader(0x140D6FD60)` at **+0xD7** (0x14131FCA7/B2). Both renderer setters read the context global 0x143027EA0 directly (xrefs 0x140D6F9BD, 0x140D6FD70) → the engine-original path is automatically redirect-safe.

CS substitutes in exactly three thunks (src/Hooks.cpp):
1. `Hooks::BSShader_BeginTechnique::thunk` (line 123; `stl::detour_thunk` on 101341, install line 1047) — on `!shaderFound` binds cache shaders via `globals::d3d::context->VS/PSSetShader`.
2. `BSShader__BeginTechnique_SetVertexShader` (line 773; write_thunk_call at 101341+0xC3) — substitutes cache VS via `globals::d3d::context->VSSetShader`.
3. `BSShader__BeginTechnique_SetPixelShader` (line 806; +0xD7) — same for PS.

**Cleanest gate:** a new flag on `State` (make it `thread_local`-backed for the later multithreaded phase), e.g. `state->engineShaderPassthrough`, set/cleared by the shadow detour around its call into the original 0x1412E3480, and checked FIRST in all three thunks → fall through to `func(...)`/engine path untouched. Precedent for this exact pattern already exists: `state->settingCustomShader` short-circuits thunks 2/3. The gate is **mandatory for correctness, not just shader policy**: the thunks bind through CS's cached `globals::d3d::context` (immediate context), which would issue VS/PSSetShader on the wrong context while the engine global is redirected — the engine-original path instead re-reads 0x143027EA0 and records correctly on the deferred context. Bonus: bypassing substitution automatically yields engine-ORIGINAL shaders for every pass rendered inside the shadow scope (Utility/RENDER_SHADOWMAP techniques), as required.

## CAVEATS

- "No other producer of shadow-map D3D work" rests on single-xref chains plus zero direct xrefs to the three `RenderShadowmaps` implementations; another *virtual* dispatch of vfunc 0xA elsewhere cannot be 100% excluded statically. CS's FrameAnnotations already wraps vfunc 0xA on all three vtables — one instrumented session can verify the sole call site at runtime.
- `Precipitation::RenderOcclusion` (0x1412E43F0, runs after RenderShadowmasks) and `BSCubeMapCamera` renders are separate utility/depth D3D work intentionally NOT covered by this detour; they are not shadow maps.
- The 355 reads of 0x143027EA0 were pattern-verified on a sample (SetDirtyStates, query helpers, Renderer setters, shader code at 0x1412c–0x14133x), not exhaustively decompiled; no memory-stored copy was seen, but a full audit of all 355 sites was not performed. CS-side code that caches `globals::d3d::context` (and any feature hooks firing during shadow passes — grep `globals::d3d::context`) is a known separate surface that must be gated or redirected.
- `ExecuteCommandList(RestoreContextState=FALSE)` resets the immediate context to defaults, and `FinishCommandList(FALSE)` resets the deferred one — the engine's shadow-state mirror at 0x143027EB0 will then disagree with actual pipeline state. After replay, either pass TRUE or force-set all dirty bits (the 0x143027EB0 dword) and re-issue lazily; the same applies to the deferred context at recording start (record from a known-clean state; the engine assumes state persistence across passes within the scope).
- The dynamic rings Map with DISCARD/NO_OVERWRITE; on a deferred context NO_OVERWRITE for VB/IB/CB requires D3D11.1 behavior. DXVK (the actual runtime here) supports it; stock-driver deployments would need a `CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS)` guard.
- The menu-snapshot path (`Main::sub_1405B1650`) runs the same full Draw once when entering certain menus — "once per frame" means at most once, via exactly one of two mutually exclusive branches in `Main::Swap_1405B1020`.
- `sub_141305610` internals partially uninterpreted: first arg `v20` of `FUN_14044B4A0` decompiles as uninitialized (register-carried; likely the camera/state block), and the ESM (`dword_141E0E2E8==2`) pre-pass semantics (flags 256) were not traced further.
- Address-library-derived addresses (101495/101498/101499, vtables) come from a self-written parser validated against three known ground-truth IDs (35560, 100422, 101341 all matched); residual parser-bug risk is negligible but nonzero.

### Caveats
- ExecuteCommandList/FinishCommandList are vfunc 58 (+0x1D0) / 114 (+0x390), NOT 133/134 as the task expected — 133/134 belong to the ID3D11DeviceContext1 extension block; binary-verified against seven engine call sites using the canonical layout
- No-other-shadow-producer claim is static (single-xref chains + no direct xrefs to the three vfunc-0xA impls); a second virtual dispatch elsewhere can't be fully excluded — runtime-verify via CS FrameAnnotations vfunc wrappers
- 355 reads of 0x143027EA0 sampled, not exhaustively audited; no memory-cached copy observed, but CS's own globals::d3d::context cache is a separate immediate-context copy that must be gated during redirect
- ExecuteCommandList/FinishCommandList with RestoreContextState=FALSE reset pipeline state; the engine mirror at 0x143027EB0 must have all dirty bits forced (or pass TRUE) after replay and at recording start
- Dynamic-ring Map(NO_OVERWRITE) on a deferred context needs D3D11.1 behavior — fine on DXVK (the actual runtime), needs a feature check on stock drivers
- Precipitation occlusion and cubemap renders are separate depth/utility D3D work outside the detour by design
- AddressLibrary-derived addresses come from a self-written v1 parser validated on 3 ground-truth IDs (35560, 100422, 101341 all matched)

================================================================================================
## Shadow cluster 3
================================================================================================

# Shadow-map D3D11 call surface — SkyrimSE 1.5.97 (base 0x140000000)

## Context vs wrapper — THERE IS NO WRAPPER

The two spellings in the decompiler are the **same 8 bytes**:

- `MEMORY[0x1430261B0][926]` = `0x1430261B0 + 926*8` = `0x1430261B0 + 0x1CF0` = **`0x143027EA0`**.
- `*(ID3D11DeviceContext**)0x143027EA0` and `MEMORY[0x1430261B0][926]` are one global slot; Hex-Rays renders the absolute-address load as the former and the base+index form as the latter. IDA resolves both as data xrefs to 0x143027EA0 (verified: the xref list for 0x143027EA0 contains both styles, e.g. `SetDirtyStates` 0x140d7062e uses the `[926]` form).
- It holds the **genuine `ID3D11DeviceContext*` (immediate context)**, written once at device creation: `FUN_140d718d0+0x408` = **0x140d71cd8**: `mov cs:143027EA0h, rax`. Released/nulled in `Renderer::Shutdown` (0x140d69140) and `Renderer::WindowSizeChanged` (0x140d698a0).
- All engine calls are direct COM vcalls: `(*(*ctx + OFFSET))(ctx, ...)` with OFFSET/8 = ID3D11DeviceContext vtable index (IUnknown 0–2, ID3D11DeviceChild 3–6, then VSSetConstantBuffers=7...). No BSGraphics-side vtable indirection exists on this path.
- Neighbors in the renderer data block `0x1430261B0` (all `[i]` = 0x1430261B0+8i): `[922]`=0x143027E80 misc CB (0x240B, FUN_140d70050), `[923]`=0x143027E88 **PerFrame CB (b12)**, `[924]`=0x143027E90 grass per-instance CB (b7), `[926]`=0x143027EA0 **context**. Bone-CB ring: cursor `[778]`=0x143027A00, 4 CBs `[779..782]`=0x143027A08..0x143027A20, alpha/tint CB `[783]`=0x143027A28 (b11). Device `ID3D11Device*` = **0x143025F08**. RT/DSV/clear-color tables live behind the pointer at **0x143025F00** (points 16 below 0x143028490; RTVs at +2648/+2664, DSVs at +8112 (+8176 stencil-side / +0x2000 read-only variants), depth-slice DSVs at +9936, clear color at +10088/+10104). Dynamic-VB ring block **0x143025F18**: `[0..2]` 3 ring VBs (4 MB), `[3]` (=0x143025F30, matches known ground truth) packed cursor(lo)/offset(hi), event queries at `[74..76]`, ready flags after.

**Redirect rule: swap the single pointer at `0x143027EA0` and every D3D11 call on the shadow path follows it — with the caveats below.**

## Shadow-map loop structure (call chain)

1. **`FUN_1412e3480` = Main::RenderShadowmaps (0x1412E3480)** — iterates `ShadowSceneNode::GetShadowCasterLightArrayEntry` (0x1412BC7D0) on `shadowSceneNode` (ptr at **0x141E0DED0**), calling each light's **vfunc Func10 (vtable +0x50)**, advancing by the per-light shadow-map count (`light+0x140` region). TLS memory-context tag (TLS idx dword 0x143497408, block +1896) set to 29 around the loop.
2. **`BSShadowDirectionalLight::Func10_141324C30`** — per-cascade loop: if `dword_141E0E2E8 == 2` (VL shadows) first loop sets desc+84=3 (depth target idx) and calls the shared render with flags 0x100; then main loop sets desc+84=**2** (= sun shadowmap depth target), desc+88=cascade index (texture-array slice), calls shared render with caller flags; then focus shadow maps (`unk_1431D0FB8._used` count) with desc+84=4, slice k+4. Frustum/parabolic lights have their own Func10 (0x14132D040 / 0x14132E4E0) into the same shared body.
3. **`BSShadowParabolicLight::sub_141305610` (0x141305610) = BSShadowLight::RenderShadowmap — the per-shadowmap body**:
```c
BSGraphics_Renderer_SetClearColors_140d6a6d0(&Renderer, ..., 1.0,1.0,1.0,1.0);
if (*(a2+232)) {   // needsClear
  RenderTargetManager::SetDepthStencilRenderTarget(0x14302BB20, *(a2+84)/*target*/, 1/*SRTM_CLEAR*/, *(a2+88)/*slice*/);  // 0x140d74d10
  RenderTargetManager::SetRenderTarget(0x14302BB20, 0, -1, 3/*SRTM_NO_CLEAR*/, 1);                                        // 0x140d74cf0
  FUN_140d6a330(&Renderer(0x143028490), 0);   // flush OM binds + issue ClearDepthStencilView NOW
}
NiCamera::RenderPreAndPostResolveDepth(*(a2+64)/*shadow cam*/, *(a2+72)/*shadow accumulator*/, a4|0x400);  // 0x1412C15C0
BSGraphics__Renderer_SetClearColorFromArray(&Renderer);  // restore clear color
FUN_140d7bbc0(&0x14302C890, cam+160, ...);   // BSGraphics::State camera-data update (CPU) + view/proj matrix block 0x143028260..0x143028417 swap
```
4. **`NiCamera::Render` 0x1412C15C0** → `sub_1412C1600` (cull/accumulate) → accumulator vfunc **Func43 (+0x158)** → for the shadow accumulator (render mode 13, set at ctor time `*(accum+336)=13`, created via `FUN_1412c9c90(mem, 0x63)`): **`BSShaderAccumulator::sub_1412CC3C0` = FinishAccumulating_ShadowMapOrMask (0x1412CC3C0)**:
   - flags&0x100 (VL): group 15 only.
   - else: `RenderBatches(0x2B, 0x4000002B)` (utility RenderDepth techniques), `RenderBatches(0x5C000030, 0x5C00005C)` (grass), group 1 (LOD, tech 0x5C006074), group 9 (LowAniso), `sub_1412CBCC0` (decals).
5. **`BSShaderAccumulator::RenderBatches` 0x1412CCE40** → `BSBatchRenderer::sub_141307DD0` + **BeginPass walk 0x141308030** (ground truth) → **`RenderPassImmediately` 0x141308440** → `BeginPass(tech, shader)` 0x1413086C0 (current-technique globals **0x143283BA4** tech / **0x143283BA8** shader / dword_141E0DF8C; current material **0x143490BB0**) → shader vfuncs Func2/Func4/Func6 (BSUtilityShader = 0x14130DF90 / 0x14130E890 / 0x14130EC70) → geometry dispatch `BSRenderPass::FUN_141307160` (0x141307160, switch on `BSGeometry+0x150` type) → Renderer draw helpers.
   - Skinned path: `RenderPassImmediately_Skinned` 0x1413088C0 / `_Standard` 0x141308970 → bone setter **`NiBoneMatrixSetterI::Func1` = 0x14131F630** (dedup via TLS+10752, reset by `FUN_14131f7c0`).
6. Shadow **masks** (screen-space, for completeness): `Main::RenderShadowmasks` 0x1412E3AC0 → `FUN_1412e3b80` — binds RT 18, builds one `BSShader::MakeRenderPass` per light (utility shader 0x143495D50, techniques (flags|0x2002/0x200002)+0x2B) and calls `RenderPassImmediately` directly; `unk_143283B78` = shadowmap slice for the pass.

## Full vfunc table (every distinct ID3D11DeviceContext call reachable on the shadow-map path)

All are issued on the ONE context pointer 0x143027EA0 (both decompiler spellings). "SDS" = `BSGraphics::SetDirtyStates` 0x140D705B0; "ClearFlush" = `FUN_140d6a330` 0x140D6A330 (its clone: same OM/RS flush logic + immediate lazy clears; used by the shadow body for the DSV clear).

| idx | offset | D3D11 name | Where issued (addresses) | Purpose on shadow path |
|----|--------|-----------|--------------------------|------------------------|
| 7 | 0x38 | VSSetConstantBuffers | BSUtilityShader Func2 0x14130e6f4/0x14130e861 (slot 0); Func4 0x14130ebfa/0x14130ec51 (slot 1); Func6 tail (slot 2); bone setter 0x14131f729 (slot 10, previous bones) / 0x14131f78b (slot 9, bones); grass instance 0x1412cecbc (slot 8); grass per-instance 0x14130786d (slot 7, CB=[924]); `ResetState` 0x140d702e3 (slot 12 = PerFrame b12) | per-technique/material/geometry CBs, bone ring, grass |
| 8 | 0x40 | PSSetShaderResources | SDS 0x140d70c27 (loop over PS-SRV dirty bits `unk_143027EB4`, table `qword_143027FF0[16+]`) | texture binds (utility PS: diffuse for alpha-test @ slot 0) |
| 9 | 0x48 | PSSetShader | `Renderer_SetPixelShader` 0x140d6fd60 (from `BSShader::BeginTechnique` 0x14131FBD0; stores current PS obj at 0x143028200 = dword_143028070[100]) | pixel shader (NULL for depth-only techniques) |
| 10 | 0x50 | PSSetSamplers | SDS 0x140d70bde (dirty bits `unk_143027EB8`, sampler indices `dword_143027F6C[i]`/`dword_143027FAC[i]`, sampler objects in renderer block at `0x1430261B0[748 + 5*mode + addr]`) | samplers |
| 11 | 0x58 | VSSetShader | `Renderer_SetShader` 0x140d6f9b0 (stores current VS obj at 0x1430281F8 = dword_143028070[98]; sets input-layout dirty 0x400) | vertex shader |
| 12 | 0x60 | DrawIndexed | `DrawTriShape` FUN_140d6bfe0 @0x140d6c0ba; `DrawDynamicTriShape` FUN_140d6cab0 @0x140d6cbc7; custom-IB FUN_140d6c0e0 @0x140d6c1c1 | the draws (IndexCount = 3*tris) |
| 14 | 0x70 | Map | shader Setup CB maps (WRITE_DISCARD=4): Func2 0x14130e085/0x14130e0d1, Func4 0x14130e8ee/0x14130e92f, Func6 head ×2, grass Func6 0x1412ce2a0; `GetID3D11Resource` 0x140d70031 (bone-ring/grass CBs; **static scratch `qword_14302AC58`!**); SDS alpha-test CB 0x140d70951 ([783]); PerFrame b12 upload `FUN_140d6b210` (Map [923], gated on `unk_14302C8E0`); FUN_140d70050 ([922]); dynamic-VB ring `FUN_140d6c8a0` @0x140d6c9a5 (**NO_OVERWRITE=5**) | all CB updates + dynamic vertex data |
| 15 | 0x78 | Unmap | matching unmaps: 0x14130e6c6/0x14130e6de/0x14130e84b (Func2), 0x14130ebca/0x14130ebe2/0x14130ec32 (Func4), Func6 tail, 0x14131f70f/0x14131f771 (bones), 0x1412ceca0 (grass), 0x140d70987 (SDS), FUN_140d6c9e0 & FUN_140d6ca30 (dyn-VB ring), 0x141307854 (grass instance CB), FUN_140d6b210 tail | |
| 16 | 0x80 | PSSetConstantBuffers | Func2 0x14130e70a (slot 0), Func4 0x14130ec19 (slot 1), Func6 tail (slot 2); `ResetState` 0x140d702c3 (slot 11 = [783]) / 0x140d702f8-ish (slot 12) | PS CBs (only when PS bound) |
| 17 | 0x88 | IASetInputLayout | SDS 0x140d70ae2 (dirty 0x400; layout = hash(vertexdesc `dword_143028070[96]` & VS `[98]`->desc) via map `unk_141E07140`/`qword_141E07160`, miss → `FUN_140d70f90` device->CreateInputLayout, guarded by lock FUN_140d730e0/FUN_140d73f70) | input layout |
| 18 | 0x90 | IASetVertexBuffers | 0x140d6c0a1 (slot 0, 1 VB, stride=(4*desc)&0x3C); 0x140d6cba3 (2 VBs: static + **dyn ring `qword_143025F18[cursor]`**); grass 0x140d6c2c3 (geom VB + instance VB); 0x140d6c1a5 | |
| 19 | 0x98 | IASetIndexBuffer | 0x140d6c05d, 0x140d6cb25, 0x140d6c25b, 0x140d6c161 — format **57 = DXGI_FORMAT_R16_UINT**, offset 0 | |
| 20 | 0xA0 | DrawIndexedInstanced | grass `fDrawGrass` 0x140d6c2ec (3*tris, instanceCount, startIndex, 0, 0) | grass (only if grass passes accumulated into shadow) |
| 24 | 0xC0 | IASetPrimitiveTopology | SDS 0x140d70b14 (dirty 0x800; topo `dword_143028070[102]` @0x143028208, draws force 4=TRIANGLELIST) | |
| 28 | 0xE0 | End | dyn-VB ring wrap `FUN_140d6c8a0` @0x140d6c8f9 (event query `qword_143025F18[74+i]` when >4 MB used) | ring fence |
| 29 | 0xE8 | GetData | 0x140d6c944 spin-wait (flags 4=DONOTFLUSH first try, then 0 + Sleep(1)); also `FUN_140d701d0` (8-byte query, not shadow) | ring fence wait — **blocks; on a deferred ctx GetData is illegal → must be rerouted to immediate ctx or privatized** |
| 33 | 0x108 | OMSetRenderTargets | SDS 0x140d70775; ClearFlush 0x140d6a4c4 | binds shadowmap DSV (per-slice DSV from data+9936 `[8*(slice + 8*targetIdx)]`), RTV=none |
| 35 | 0x118 | OMSetBlendState | SDS 0x140d70913 (blendFactor const @0x141E07168); ClearFlush 0x140d6a698 (@0x141E07178) | blend (indices unk_143027F58/F5C/F60 + alpha-to-coverage flt_143028470[4]) |
| 36 | 0x120 | OMSetDepthStencilState | SDS 0x140d707c9; ClearFlush 0x140d6a544 (state idx unk_143027F38 (depth) / unk_143027F44 stencilRef, table `0x1430261B0[40*a+...]`) | |
| 43 | 0x158 | RSSetState | SDS 0x140d70823; ClearFlush 0x140d6a5a0 (indices unk_143027F48/F4C/dword_143027F50/F54; **F50=1 selects the depth-bias rasterizer for shadow render**, bias values dword_143027EBC[29..30] vs table 0x143026180) | |
| 44 | 0x160 | RSSetViewports | SDS 0x140d708bc; ClearFlush 0x140d6a63f (1 viewport, float4 @dword_143027EBC[25..28], filled by `Renderer::UpdateViewPort` 0x140d69d00 — dirty-bit only, sized from RenderTargetManager 0x14302BB20; dyn-res scale 0x14302C98C skipped when depth-target bound) | shadowmap viewport |
| 45 | 0x168 | RSSetScissorRects | `FUN_140d70100` @0x140d7013a (1 rect) — called from BSUtilityShader::Func6 non-directional branch with the light's projected bounding box | per-light scissor (frustum/omni shadow) |
| 47 | 0x178 | CopyResource | `Renderer::SetRenderTarget` 0x140d74f0a (only mode a4==5 SRTM_RESTORE — copies RT texture; not hit with mode 3/1 used on shadow path); main-view depth copies 0x1412cb07e / Main::RenderDepth tail | not expected during shadowmaps, listed for completeness |
| 50 | 0x190 | ClearRenderTargetView | SDS 0x140d706a1/0x140d7063f; ClearFlush 0x140d6a411/0x140d6a3b5 (lazy clear-on-bind: per-RT mode words `dword_143027EBC[15+i]`/[24], mode 0=clear→issues then sets 4) | shadow path: RT list empty → no-op |
| 53 | 0x1A8 | ClearDepthStencilView | SDS 0x140d70765 (flags by depth mode word `dword_143027EBC[23]`: 0/6→3, 1→1, 2→2, depth=1.0 stencil=0 implied); ClearFlush 0x140d6a4b4 — **this is the shadow-map clear**, DSV chosen by `dword_143027EBC[11]` (target) `[12]` (slice) | |
| 67 | 0x218 | CSSetShaderResources | SDS loop 0x140d70cc7 (bits dword_143027EBC[0]) | normally no-op on shadow path |
| 68 | 0x220 | CSSetUnorderedAccessViews | SDS loop 0x140d70b7c (bits [2]) | no-op |
| 70 | 0x230 | CSSetSamplers | SDS loop 0x140d70c7e (bits [1]) | no-op |

Not used on the shadow path: Draw, DrawInstanced, Begin, UpdateSubresource, any GS/HS/DS setter, Dispatch, ResolveSubresource, ExecuteCommandList.

Key body quotes (load-bearing):
- Draw helper `FUN_140d6bfe0`: `SetDirtyStates(0); ctx->IASetIndexBuffer(*(a2+8),57); ctx->IASetVertexBuffers(0,1,a2,&stride,&0); ctx->DrawIndexed(3*a4, a3, 0);`
- Bone setter 0x14131F630: `GetID3D11Resource(&renderer, 3*boneCount, &mapped, 10)` → memcpy 48B/bone ×2 → `Unmap; VSSetConstantBuffers(10,1,&cb); ... VSSetConstantBuffers(9,1,&cb2);`
- `GetID3D11Resource` 0x140d6ffd0: `cb = (a4==7) ? [924] : [779 + cursor[778]++&3]; qword_14302AC58[0]=cb; ctx->Map(cb,0,DISCARD,0,&m); return &qword_14302AC58;` — **one static slot shared by bones/grass; not thread-safe, must be privatized.**
- `ResetState` 0x140d70290: `RendererShadowState::InitFlags(0x143027EB0); PSSetConstantBuffers(11,1,&[783]); VSSetConstantBuffers(12,1,&[923]); PSSetConstantBuffers(12,1,&[923]);`

## Redirect surface (what to swap / replicate)

1. **The pointer**: `0x143027EA0` (== `0x1430261B0[926]`). Every listed call dereferences it fresh at call time — pointing it at a deferred context redirects 100% of the D3D calls above. There is no second cached copy on this path.
2. **Blocking calls that are illegal/degenerate on a deferred context** — must NOT be recorded:
   - `GetData` spin (idx 29) in the dyn-VB ring allocator `FUN_140d6c8a0` and its `End` (28). Either give the shadow thread a private VB ring (replicate `0x143025F18` block: 3 VBs + 3 event queries + cursor 0x143025F30) with fences issued on the immediate context, or pre-reserve.
   - `Map(NO_OVERWRITE)` on the ring VB is legal on deferred contexts only on 11.1 hardware; `Map(DISCARD)` on the CBs is fine but each first-per-commandlist DISCARD is required. Bone ring `[778..782]`, alpha CB `[783]`, PerFrame `[923]`, grass `[924]`, per-VS-object CBs (VS obj+24/+40/+56, mapped ptrs +32/+48/+64) all get mapped through the swapped pointer — DISCARD semantics survive, but the **ring cursor 0x143027A00 and `qword_14302AC58` scratch are shared globals** → privatize for multithreading.
3. **Deferred contexts start with clean state**: the engine assumes persistent bindings that are set elsewhere on the immediate context — re-issue at the start of every command list: `VS/PSSetConstantBuffers(12, &[923])`, `PSSetConstantBuffers(11, &[783])` (i.e. replay `ResetState` 0x140D70290), plus current VS/PS, topology, and the OM/RS state. Cheapest correct approach: memset the dirty word **0x143027EB0** to "everything dirty" and reset the cached-binding shadows (`dword_143028070[96..102]`, `qword_143027FF0[]`, `dword_143027F6C/FAC`, `unk_143028000/008/010/018/020/028`, `0x1430281F0/1F8/200/208`, `unk_1430284C2`, `dword_143027EBC[13]=-1`) so SDS/ClearFlush re-emit on first use — these caches otherwise suppress the D3D calls because they think state is already bound.
4. **The whole mutable state block to replicate for a private (multithreaded) copy** is the RendererShadowState "S" block **0x143027EB0 .. ~0x143028488** (dirty word; CS dirty bits EBC[0..2]; RT/DSV selection EBC[11..24]; viewport EBC[25..28]; depth-bias EBC[29..30]; DS/raster/blend indices F38..F64; alpha-test F64/F68; PS sampler modes F6C[16]/FAC[16]; PS SRVs FF0[16+]; CS SRV/UAV/sampler tables 0x143028030..; vertexdesc/VS/PS/topology 0x1430281F0..0x143028208; pos-adjust 0x14302820C..220; view/proj matrix block 0x143028260..0x143028417; clear-color save flt_143028470[0..]; misc flags 0x1430284C2/C5) **plus** bone-ring cursor 0x143027A00, dyn-VB ring 0x143025F18/0x143025F30, `qword_14302AC58`, current-technique globals 0x143283BA4/0x143283BA8/0x141E0DF8C/0x143490BB0, `unk_143283B78` (shadowmap slice), camera block 0x14302C890 (+ dirty flag 0x14302C8E0, frame counter 0x14302C8DC) and TLS slots +1896/+10752.
5. **`ExecuteCommandList` insertion point**: end of `FUN_1412e3480` (0x1412E3480) after the light loop, or per-light after each `Func10` return, on the real immediate context (keep a saved copy of the original pointer from 0x140d71cd8-time). Restore-state=FALSE and then mark the S block fully dirty (see #3) so the following engine pass rebinds.
6. **Engine-original shaders**: shader binds go through the VS/PS objects' `+8` `ID3D11VertexShader*/ID3D11PixelShader*` (`Renderer_SetShader`/`_SetPixelShader`) — untouched by the redirect; input layouts come from the shared cache 0x141E07140/0x141E07160 whose **miss path creates layouts on the device (thread-safe) but under a renderer lock (FUN_140d730e0/FUN_140d73f70)** — safe to call from the recording thread.

## CAVEATS

- ID3D11DeviceContext vfunc indices are mapped from the standard Windows SDK vtable order; every offset seen was consistent with it (arg shapes verified for Map/Unmap/OMSetRenderTargets/ClearDepthStencilView/DrawIndexed/IASet*), but I did not diff against d3d11.h mechanically.
- BSDistantTreeShader (Func2 0x1413014C0 / Func6 0x141301990) and BSGrassShader Func2 were not fully decompiled; their ctx xrefs (Map/Unmap/VS-PSSetConstantBuffers at 0x14130156f/5a9/917-955, 0x1413019d6/ad1/ae1) match the same Map→fill→Unmap→SetConstantBuffers quartet as BSUtilityShader, and trees render in shadow maps (LOD group / technique range). Slot numbers for distant-tree assumed 0/2 by pattern, not read.
- `BSShaderAccumulator::sub_1412CC3C0` was identified as the mode-13 (shadowmap) FinishAccumulating by its technique ranges (0x2B..0x4000002B RenderDepth, grass, decals) and by the shadow accumulator ctor setting `*(accum+336)=13`; I did not decompile the Func43 dispatcher switch itself.
- Decals in shadow (`sub_1412CBCC0` → `sub_1412CBB20`) were not decompiled; they route through the same RenderBatches/RenderPassImmediately machinery, so no new ctx vfuncs are expected, but per-decal state pokes were not enumerated.
- The PerFrame b12 upload `FUN_140d6b210` is called from the main-scene finish; I did not find a call inside the shadow finish (0x1412CC3C0). Utility shaders compute WVP CPU-side into their slot-0/2 CBs, so shadow rendering likely never touches b12 — but if any pass reads b12, the deferred context must have it bound (see redirect #3). Unverified for every technique in 0x2B..0x4000002B.
- `RenderPassImmediately_Custom` (0x141308B20, geometry flag &8 path) was not decompiled; on shadow paths it should be rare (custom shadow geometry), and it feeds the same helper set, but not verified.
- Whether frustum/parabolic Func10 bodies (0x14132D040/0x14132E4E0) add calls beyond the shared `sub_141305610` body was not checked; the scissor-rect call (idx 45) is confirmed reachable via the utility shader's non-directional branch regardless.
- dword_143027EBC[12] doubling as ClearDepthStencilView arg 4 in SDS (stencil value?) is odd in the decompile; the ClearFlush variant (0x140d6a4b4) passes only (dsv, flags) with depth/stencil in registers the decompiler dropped — actual values (1.0f, 0) inferred from calling convention, not read from disasm.

### Caveats
- Vfunc indices mapped from the standard d3d11.h vtable order; arg shapes verified per call but not mechanically diffed against the SDK header.
- BSDistantTreeShader Func2/Func6 and grass Func2 not fully decompiled; their ctx xrefs match the Map/Unmap/SetConstantBuffers quartet, slots assumed by pattern.
- sub_1412CC3C0 identified as the shadowmap FinishAccumulating by technique ranges and accumulator render-mode 13; the Func43 dispatcher switch itself was not decompiled.
- Decal sub-path (sub_1412CBCC0/sub_1412CBB20) not decompiled; expected to reuse the same machinery, no new vfuncs verified.
- PerFrame b12 upload (FUN_140d6b210) not observed inside the shadow finish; utility shaders appear to compute WVP CPU-side, but b12 dependence of every technique in 0x2B..0x4000002B is unverified — bind b12 on the deferred context anyway.
- RenderPassImmediately_Custom (0x141308B20) not decompiled (rare custom-geometry path).
- Frustum/parabolic light Func10 bodies (0x14132D040/0x14132E4E0) not decompiled; assumed to funnel into sub_141305610 like the directional light.
- ClearDepthStencilView depth/stencil values (1.0f/0) inferred from calling convention; the decompiler dropped the float/int register args at both call sites.

================================================================================================
## Shadow cluster 4
================================================================================================

# Shadow render path — complete mutable-global-state inventory (SkyrimSE 1.5.97, base 0x140000000)

Scope: everything between the per-frame driver `Main::RenderShadowmaps` **0x1412E3480** (called from `DrawWorld::MainAccum` **0x1412E2A60**) down through `BSShadowLight::RenderShadowmap` **0x141305610** → `NiCamera::Render` **0x1412C15C0** → accumulator batch walk (`0x141307930` / `0x141308030` / `0x1413094A0`) → `RenderPassImmediately` **0x141308440** → `SetDirtyStates` **0x140D705B0** → D3D11.

Top-level loop (verified decompile of 0x1412E3480):
```c
v1 = TLS[0x143497408] + 1896; saved = *v1; *v1 = 29;      // per-thread memory-context tag
for (light = ShadowSceneNode::GetShadowCasterLightArrayEntry(shadowSceneNode /*0x141E0DED0*/, 0);
     light; light = ...(shadowSceneNode, cursor))
    light->vtbl+0x50 /*RenderShadowmaps*/(light, &cursor);
*v1 = saved;
```
Per-light `Func10` (directional **0x141324C30**, frustum **0x14132D040**, parabolic **0x14132E4E0**) loops the light's shadowmap-data array (stride **240** at light+320, count light+312), writing `+84 = depthStencilTarget` (2 = sun cascades, 3 = debug-mode-2, 4(+slice k+4) = focus maps, cube path uses stored idx) and `+88 = slice`, then calls **0x141305610** per view. `0x141305610` per view: releases the map's deferred-texture list (light+1312/+1328, refcounted), allocates a slice from bitmask **dword_141E10538** when `+84==-1` (freed by `BSShadowLight::sub_141304AF0` 0x141304AF0), sets clear color (1,1,1,1), sets DS render target via RenderTargetManager, renders the accumulator through `NiCamera::Render`, then recomputes the shadow view/proj from the shadow-state camera mirror and stores it back into the shadowmap-data struct (+0..63) — i.e. the *output* of a view render is read from global camera state.

## Dirty-state block field map (RendererShadowState, base B = 0x143027EB0, size 0x5D8 → 0x143028488)

All writers below are the batch-walk / shader Setup* functions on the render thread; the sole consumer is `SetDirtyStates` 0x140D705B0 (+ direct context calls noted). Defaults from `RendererShadowState::InitFlags` **0x140D73BD0** (called by `Renderer::ResetState` 0x140D70290).

| Addr | B+ | Type | Meaning (verified use in SetDirtyStates / setters) |
|---|---|---|---|
| 0x143027EB0 | 0x00 | u32 | stateUpdateFlags. bit0=RT/DSV rebind+clears; bit1=viewport(RSSetViewports of B+0x70); bits2-3=OMSetDepthStencilState; bit4(0x10)+0x40+0x1000+0x20 (mask 0x1070)=RSSetState; bit6(0x40)=depth-bias→also rewrites viewport minZ/maxZ from 0x143028470 + bias table 0x143026180[biasMode]; bit7(0x80)=OMSetBlendState; bits8-9(0x300)=alpha-test CB Map/write(renderer[783]); bit10(0x400)=IASetInputLayout (hash `vertexDesc & VS->descMask(VS+0x48)`); bit11(0x800)=IASetPrimitiveTopology |
| 0x143027EB4 | 0x04 | u32 | PSResourceModifiedBits → PSSetShaderResources(slot,1,&B+0x140[slot]) |
| 0x143027EB8 | 0x08 | u32 | PSSamplerModifiedBits → PSSetSamplers(slot,1,&rendererData[5*filter[slot]+748+addr[slot]]) |
| 0x143027EBC | 0x0C | u32 | CSResourceModifiedBits (CSSetShaderResources from B+0x240) |
| 0x143027EC0 | 0x10 | u32 | CSSamplerModifiedBits (from B+0x1C0/0x200) |
| 0x143027EC4 | 0x14 | u32 | CSUAVModifiedBits (CSSetUnorderedAccessViews from B+0x300) |
| 0x143027EC8 | 0x18 | u32[8] | renderTargets[8] — index into renderer-instance RTV table (inst+2648, stride 48, RTV at +8) |
| 0x143027EE8 | 0x38 | u32 | depthStencil index — DSV table inst+8112 (or +8176 when inst byte +34 "read-only depth" set), 19 views per depth target |
| 0x143027EEC | 0x3C | u32 | depthStencilSlice (also ClearDepthStencilView arg) |
| 0x143027EF0 | 0x40 | u32 | cubeMapRenderTarget (-1 = normal path; else RTV = inst + 8*(view + 8*idx) + 9936) |
| 0x143027EF4 | 0x44 | u32 | cubeMapRenderTargetView (face) |
| 0x143027EF8 | 0x48 | u32[8] | setRenderTargetMode[8] (0=CLEAR→ClearRenderTargetView(inst+10088 color) then 4) |
| 0x143027F18 | 0x68 | u32 | setDepthStencilMode (≤2 or 6 → clear; clear-flags map 0/6→3, 2→2, 1→1; →4 after) |
| 0x143027F1C | 0x6C | u32 | setCubeMapRenderTargetMode |
| 0x143027F20 | 0x70 | f32[6] | D3D11_VIEWPORT x,y,w,h,minZ,maxZ. Written by `Renderer::UpdateViewPort` **0x140D69D00** from normalized rect 0x143028460-46C × RT dims (RT-manager getters; DRS scale 0x14302C98C/990 unless 0x14302C9A0) |
| 0x143027F38 | 0x88 | u32 | depthStencilDepthMode (dirty 4) |
| 0x143027F3C | 0x8C | u32 | depthStencilDepthModePrevious (default 6; dirty-bit cancel logic) |
| 0x143027F40 | 0x90 | u32 | depthStencilStencilMode (written as u64 pair with stencilRef, e.g. 0xFF00000000) |
| 0x143027F44 | 0x94 | u32 | stencilRef. DS state object = rendererData + 8*(40*depthMode+stencilMode) |
| 0x143027F48 | 0x98 | u32 | rasterStateFillMode (×72) |
| 0x143027F4C | 0x9C | u32 | rasterStateCullMode (×24; toggled per pass-type in batch BeginPass 0x141308030, dirty 0x20) |
| 0x143027F50 | 0xA0 | u32 | rasterStateDepthBiasMode (×2; dirty 0x40; utility SetupTechnique forces 1 for shadowmaps) |
| 0x143027F54 | 0xA4 | u32 | rasterStateScissorMode (+base 240 qwords into rendererData; dirty 0x1000) |
| 0x143027F58 | 0xA8 | u32 | alphaBlendMode (×52, blend table at rendererData+0xC00) |
| 0x143027F5C | 0xAC | u32 | alphaBlendAlphaToCoverage (×26; gated on global unk_1431D0E5D) |
| 0x143027F60 | 0xB0 | u32 | alphaBlendWriteMode (×2; + last blend index = dword **0x143028480**, a global outside per-pass control) |
| 0x143027F64 | 0xB4 | u32 | alphaTestEnabled (dirty 0x100) |
| 0x143027F68 | 0xB8 | f32 | alphaTestRef → Map(WRITE_DISCARD) of CB renderer[783] @0x143027A28 (bound once PS b11 in ResetState) |
| 0x143027F6C | 0xBC | u32[16] | PS sampler filter mode / slot |
| 0x143027FAC | 0xFC | u32[16] | PS sampler address mode / slot (default 3) |
| 0x143027FF0 | 0x140 | ptr[16] | PS SRV cache. Slot2 (0x143028000) = main depth SRV, slot5 (0x143028018) = focus-shadowmap depth SRV — set in utility SetupTechnique from inst qword [19*dsIdx+1032]/[1033]; also sets hazard byte **0x1430284C2** (inst+50, cleared in ResetState) |
| 0x143028070 | 0x1C0 | u32[16] | CS sampler filter mode |
| 0x1430280B0 | 0x200 | u32[16] | CS sampler address mode (default 3) |
| 0x1430280F0 | 0x240 | ptr[16] | CS SRV cache |
| 0x143028170 | 0x2C0 | 0x40 B | zero-initialized region (poss. CS SRV slots 16-23) |
| 0x1430281B0 | 0x300 | ptr[8] | CS UAV cache |
| 0x1430281F0 | 0x340 | u64 | vertexDesc (input-layout hash half; written by draw wrappers e.g. 0x140D6BFE0, dirty 0x400) |
| 0x1430281F8 | 0x348 | ptr | current BSGraphics::VertexShader — written by `Renderer_SetShader` **0x140D6F9B0** which ALSO calls VSSetShader immediately |
| 0x143028200 | 0x350 | ptr | current PixelShader — `Renderer_SetPixelShader` **0x140D6FD60**, immediate PSSetShader |
| 0x143028208 | 0x358 | u32 | topology (dirty 0x800) |
| 0x14302820C | 0x35C | f32[3] | currentPosAdjust |
| 0x143028218 | 0x368 | f32[3] | previousPosAdjust |
| 0x143028230 | 0x380 | 0x250 B | camera mirror, block-copied by **0x140D7D8C0**: ViewMat@0x143028260, ProjMat@0x1430282A0, ViewProjMat@0x1430282E0, +0x320 unk, ViewProjUnjittered@0x143028360, PrevViewProjUnjittered@0x1430283A0, ProjUnjittered@0x1430283E0, +0x420 unk, normalized viewport rect@**0x143028460**[4], camera depth range near/far@**0x143028470**[2]. Written per view by `State::SetCameraData` 0x140D7BAB0 / explicit variant 0x140D7BBC0 (→0x140D7D430→0x140D7BEF0) |
| 0x143028480 | 0x5D0 | u32 | final blend-table index term (flt_143028470[4]); writer not found in render path — treat as quasi-static config |

## Technique/material/geometry caches

| Addr | Type | Meaning | Writers |
|---|---|---|---|
| 0x143283BA4 | u32 | current technique ID (skip-BeginPass dedup; 0x5C006076 never dedups) | RenderPassImmediately 0x141308440, BeginPass helper **0x1413086C0** (calls shader vfunc+0x10 SetupTechnique / +0x18 RestoreTechnique), ClearState **0x141308760**, batch walk 0x141307930/0x141308030/0x1413090C0, InitSDM 0x141308520 |
| 0x143283BA8 | BSShader* | current shader (paired with above) | same |
| 0x143490BB0 | ptr | current BSShaderMaterial (skip-SetupMaterial dedup, shader vfunc+0x20) | same |
| 0x141E0DF8C | u32 | current technique (debug mirror, written unconditionally in 0x141308440) | RenderPassImmediately |
| 0x141E10660 | u32 | BSUtilityShader saved alphaBlendWriteMode token (13 = empty); set in SetupGeometry 0x14130EC70, consumed/restored in RestoreTechnique 0x141310300 | utility shader only |
| 0x143283BA0 | u32 | count of per-frame sorted pass array; reset each frame by 0x141308540 (Main::Swap), qsorted by 0x141308560 | BSBatchRenderer 0x141309280/0x1413093E0/0x1413094A0 |
| 0x143490BD0 | 0x28-stride array | the sorted pass-group list itself | same |
| 0x1431D0E20 | ptr | current BSShaderAccumulator (`SetCurrentAccumulator` 0x1412966B0 / `GetCurrentAccumulator` 0x1412966A0 — plain global, NOT TLS; read inside utility SetupGeometry) | NiCamera::Render per view |
| 0x1431D0E68 | NiCamera* | current camera (read for depth calc in SetupGeometry/SetupTechnique) | NiCamera::sub_1412C1600 |
| BSShader+0x90 / +0x94 | u32×2 | per-shader-OBJECT scratch: current technique descriptor / (desc & 0x7F). Written by SetupTechnique (e.g. BSUtilityShader::Func2 **0x14130DF90**), read by SetupGeometry 0x14130EC70 and RestoreTechnique 0x141310300 | shader object mutation |
| VertexShader +0x18/+0x20, +0x38/+0x40; +0x48 desc-mask; +0x50.. offset bytes | ptr pairs | per-technique CB (b0) / mapped-ptr scratch; per-geometry CB (b2) / mapped-ptr scratch. Map(WRITE_DISCARD) on the **immediate context**, mapped pointer stored INTO the shared shader object, filled across SetupTechnique/SetupGeometry, Unmapped + VSSetConstantBuffers(0 or 2) at end | shader object mutation |
| PixelShader +0x10/+0x18, +0x30/+0x38; +0x40.. offset words; +0x08 | ptr pairs | same for PS (b0/b2); PS+0x08 is a COM ptr released by 0x140D6FCF0 in the stencil-textured path | shader object mutation |
| skin instance (+72/+80 arrays, per-frame stamp) | heap | `BSDismemberSkinInstance::sub_140D74F70` recomputes bone matrices once per frame per skin (reads frame counter 0x14302C8DC, stamps the instance) | bone path |
| 0x143283BB0, 0x143477BB0 | scratch blocks | strip-particle geometry staging used by 0x140D76D80/0x140D6CE60 (RenderPassImmediately geometry case 1) | particle passes |
| 0x141E07140/0x141E07144/0x141E07150/0x141E07160 | hashmap | input-layout cache keyed by `vertexDesc & VS->mask`; misses call **0x140D70F90** (device CreateInputLayout) and insert (0x140D730E0/grow 0x140D73F70) | SetDirtyStates bit 0x400 |

## Ring/pool state

| Addr | Meaning |
|---|---|
| 0x143027A00 | = rendererData[778]: bone-CB ring cursor, `cursor = (cursor+1) & 3`, advanced by `GetID3D11Resource` **0x140D6FFD0** on every bone upload (VS b9 previous-bones + b10 bones = 2 ring slots per skinned pass, `NiBoneMatrixSetterI::Func1` **0x14131F630**) |
| 0x143027A08 | rendererData[779..782]: 4× ID3D11Buffer* 3840 B (created 0x140D720D0, Map WRITE_DISCARD on immediate context) |
| 0x14302AC58 | global scratch qword: "currently mapped bone CB" handle returned by GetID3D11Resource (aliased into VSSetConstantBuffers by callers); [2] also reused as a QPC timestamp at present. Sits right after the renderer critical section |
| 0x143025F18 | dynamic-VB ring block: [0..2] = 3× 4 MB vertex buffers; **0x143025F30** ([3]) = packed u64 {lo=ring index 0..2, hi=write offset}; +0x250 (0x143026168) = 3 event queries; byte flags ~0x143026164+i = query-signaled. Allocator **0x140D6C8A0**: overflow → End(query), rotate, GetData busy-wait (Sleep 1), Map NO_OVERWRITE — all on the immediate context |
| 0x143025F00 | qword ptr → renderer instance 0x143028490 (RT/DSV/cubemap/clear-color tables) |
| 0x143025F08 | ID3D11Device* |
| 0x143025F10 | swapchain holder (present at frame end 0x140D6A2B0) |
| 0x143027EA0 | ID3D11DeviceContext* (immediate) == rendererData[926]; **every** function in this path calls D3D through 0x143027EA0 or rendererData[926] |
| 0x1430261B0 | rendererData base: DS states +0 (40×n), raster states +0x780 (72/24/2/1 dims), blend states +0xC00 (52/26/2 dims), sampler states +8×748, CB pools [779..924] (incl. [783] alpha-test 16 B @0x143027A28 → PS b11, [922] 576 B @0x143027E80, [923] 720 B PerFrame @0x143027E88 → VS+PS b12, [924] 16 B instance @0x143027E90 → VS b7 grass) |
| 0x143028490 | renderer instance (`Renderer::QInstance`, = *0x143025F00): RTV entries +2648 (48 B), DSV tables +8112/+8176 (19/target; per-target SRVs at qword idx [19i+1032]/[1033]), cubemap RTVs +9936, clear color +10088, byte +34 = use-read-only-DSV flag (WRITTEN during clears), byte +50 (0x1430284C2) = depth-SRV-bound hazard flag, CRITICAL_SECTION +10128 (0x14302AC20, Renderer_Lock 0x140D6A080) |
| 0x14302BB20 | RenderTargetManager (RT dims/props; width/height getters 0x140D74C20/C60, cube 0x140D74CB0/CD0; shadowmap dims read at dword [4×dsIdx+798/799] in 0x141305610) |
| 0x141E10538 | shadowmap-slice allocator bitmask (alloc in 0x141305610, free in 0x141304AF0) |

Per-frame CB upload `0x140D6B210` (runs on every camera change while flag **0x14302C8E0** set): Map(DISCARD) rendererData[923], writes 12 matrices + posAdjust + DRS constants from the S-block camera mirror and 0x14302C98C/990/994/998/9A0 + INI 0x141E073D8/0x141E07240.

## Camera + TLS

- **0x14302C890** = BSGraphics::State. Verified members touched per shadow view: frame counter +0x4C (0x14302C8DC, ++ at present 0x140D6A2B0), per-frame-CB flag +0x50 (0x14302C8E0, set at BeginFrame 0x140D6A0C0, cleared at present), default texture +0x58 (0x14302C8E8, stuffed into PS SRVs 0-5/7 on missing textures in 0x141307160), DRS scales 0x14302C98C/0x14302C990, previous 0x14302C994/0x14302C998, DRS-off flag 0x14302C9A0, fallback CameraStateData 0x14302C9B0, plus a per-camera CameraStateData cache that `SetCameraData` 0x140D7BAB0 looks up (0x140D7D7B0) and can **insert into** (0x140D7D130) — State is mutated per view.
- `SetCameraData`/`0x140D7BBC0` → `0x140D7D430`: copies the selected CameraStateData into the S-block mirror (0x14302820C posAdjust, 0x143028218 prevPosAdjust, 0x143028230..0x143028480 matrices+viewport-rect+depth-range) then re-uploads PerFrame CB (0x140D6B210).
- TLS: index dword **0x143497408**; block = `TEB->TLS[idx]`.
  - **+1896**: memory-context tag. Saved/set/restored: =29 around the whole shadow-light loop (0x1412E3480), =26 inside bone upload (0x14131F630).
  - **+10752 (0x2A00)**: last-uploaded-skin-instance pointer (dedup for bone-CB upload); cleared to 0 at the top of RenderPassImmediately_Standard by 0x14131F7C0. Already per-thread by construction.
  - **+10760**: CRT `_Init_thread` epoch (magic-static guard in SetCameraData) — CRT, not engine.

## Privatize vs share classification

**Must be replicated per-thread (written during pass dispatch):**
1. Whole RendererShadowState block 0x143027EB0..0x143028488 (incl. camera mirror + viewport rect/depth range tail; 0x5D8 bytes) — the natural unit: memcpy the live block into a private copy, run a private SetDirtyStates against the deferred context.
2. Technique/material dedup trio 0x143283BA4/BA8 + 0x143490BB0, debug 0x141E0DF8C, utility blend token 0x141E10660.
3. Bone-CB ring cursor 0x143027A00 + the 4 CBs 0x143027A08 and scratch 0x14302AC58 — Map(DISCARD) must move to the deferred context and the 4-deep ring must be per-thread (or per-thread CB sets).
4. Dynamic-VB ring 0x143025F18/0x143025F30 — worse: uses Map(NO_OVERWRITE) + **event-query GetData busy-wait, which is illegal on a deferred context**; needs a per-thread VB or a lock-free suballocator. Only relevant if particles/dynamic tri-shapes render into shadowmaps (they can — geometry cases 0/1/3/10 of 0x141307160).
5. Per-shader-object scratch: BSShader+0x90/+0x94; VS +0x20/+0x40 mapped pointers; PS +0x18/+0x38 mapped pointers (+0x08 released ptr). Shader objects are process-global singletons per (shader,permutation) — two threads in the same technique will clobber each other's mapped pointers. Options: per-thread shadow copies of the CB+scratch fields, or thread-indexed CB arrays.
6. Current accumulator 0x1431D0E20 and current camera 0x1431D0E68 — per view; State 0x14302C890 camera mirror + PerFrame CB (b12): each thread needs its own PerFrame CB (contents differ per shadow view: ViewProj) or must pre-bake per-view CBs.
7. Skin-instance per-frame bone matrices (sub_140D74F70 stamp) — race if two threads render the same skinned object; must lock per instance or pre-update.
8. Particle staging 0x143283BB0/0x143477BB0 — per-thread if particle shadow passes are kept.
9. Sorted-group bookkeeping 0x143283BA0/0x143490BD0 and the accumulator/batch-renderer pass-group tables (`*(v20+40)` bucket-bit clears in 0x141308030 when a1+100 set) — pass lists are per-accumulator (each shadow light view owns its accumulator at shadowmapData+72/+224), so they parallelize per-view naturally, but the sorted array 0x143490BD0 is a single global.
10. Shadowmap slice bitmask 0x141E10538 + shadowmap-data structs (+84/+88 targets, +72 deferred-release list) — per-light mutation; serialize per light.

**Shared, read-only during the pass (deferred context can read freely; cannot privatize, must not write):** device 0x143025F08; rendererData state-object/sampler/CB tables 0x1430261B0 (object handles immutable after init — only their *contents* are mapped); renderer instance RTV/DSV/cubemap tables + clear color at 0x143028490 (**except** mutable bytes +34 and +50=0x1430284C2 — treat as per-thread state); RenderTargetManager 0x14302BB20; ShadowSceneNode 0x141E0DED0 + sunShadowDirLight split distances (+0x598 area) + shadowCasterLights array; scene graph/geometry; config/INI globals (0x141E0DE34, 0x141E0DE43, 0x141E0DE4C, 0x141E10670, 0x141E106A0, 0x141E106B8, 0x141E10B78/7C, 0x141E0DF04, 0x141E0DF70/74, 0x1431D0E5D, 0x143283B78/B7C/B88/B90, depth-bias table 0x143026180, blend factor 0x141E07168, focus arrays 0x1431D0FA8/0x1431D0FB8, camera ptr 0x1431D0F88); frame counter 0x14302C8DC (read-only inside the frame).

**Shared-mutable caches needing a lock or pre-warm:** input-layout hashmap 0x141E07140/44/50/60 (insert path calls device->CreateInputLayout — device methods are thread-safe, the map is not); State's per-camera CameraStateData cache (insert on first sight of a camera); the renderer critical section 0x14302AC20 (Main frame already holds it — a worker must NOT recursively depend on it).

**Immediate-context calls that bypass the dirty system (each needs redirection to the deferred context in the detour):** VS/PSSetShader (0x140D6F9B0/0x140D6FD60); Map/Unmap of shader CBs, bone CBs, per-frame CB, alpha-test CB, dynamic VB; VS/PSSetConstantBuffers slots 0,2,7,9,10,11,12; RSSetScissorRects (0x140D70100, used by shadow-light passes from projectedBoundingBox); IASetIndexBuffer/IASetVertexBuffers/DrawIndexed (0x140D6BFE0 et al.); CopyResource (SetRenderTarget mode 5); Clear*/OMSetRenderTargets/RSSetState/OMSetBlendState/etc. inside SetDirtyStates; dynamic-VB event-query End/GetData (deferred-context-illegal, see above).

## CAVEATS
- `FUN_140D6A0C0` (BeginFrame), `FUN_140D6A330` (likely depth clear), `FUN_140D7C200` (camera math), `FUN_140D7D7B0/130` (camera cache internals), `FUN_140D6CE60/140D6CBE0` full bodies, `sub_141307DD0`/`Func3 0x141307930`/`sub_1413094A0` details (treated as ground truth), `GetDepthStencilTarget_MAIN 0x140D74E50`, and `FUN_14130F960`/`SetupShadowLightParameters 0x14130FBE0` were not fully decompiled; their global writes beyond what callers show are unverified.
- Writer of the blend-index dword 0x143028480 not located (assumed init-time/output-mode config).
- The 0x40-byte zero region B+0x2C0 (0x143028170) is unidentified (possibly CS SRV slots 16-23).
- 0x14302C8E0 bit semantics (bit1 first-person, bit2 alpha-pass, bit7 CK per PerFrame CB field names) inferred from FUN_140D6B210's use; the decompile of the flag byte read there is mangled.
- Sizes of particle scratch 0x143283BB0/0x143477BB0 not measured.
- `FUN_1405B29F0`/queue 0x141EC4320 (Main::Swap loop, DS target 5, render mode 27) is a separate cached-shadowmap-style path outside the main shadow loop; identity ("unowned"/interface shadows) not confirmed.
- Exact BSShadowLight/shadowmap-data struct layout beyond the observed offsets (+64 camera, +72 accumulator?, +84 dsTarget, +88 slice, +208..220 clip, +224 accumulator ref, +232 cube flag, +240 stride) is partial; +72 holds a refcounted object released per render, +224 compared against light->shaderAccumulator.
- BSShader+0x90 verified for BSUtilityShader (the shadowmap shader); other BSShader subclasses were not checked for extra per-object scratch beyond the same pattern (BSLightingShader::Func4/Func6 reference the same VS/PS global pair heavily; assume same mechanism).

### Caveats
- FUN_140D6A0C0 (BeginFrame), FUN_140D6A330 (likely depth clear), camera-cache internals (0x140D7D7B0/0x140D7D130/0x140D7C200), FUN_140D6CE60/0x140D6CBE0 full bodies, sub_141307DD0/Func3/sub_1413094A0 internals, GetDepthStencilTarget_MAIN, and SetupShadowLightParameters (0x14130FBE0) were not fully decompiled — their additional global writes are unverified.
- Writer of blend-index dword 0x143028480 not located; assumed init-time config.
- 0x40-byte zeroed region at 0x143028170 (B+0x2C0) unidentified (possibly CS SRV slots 16-23).
- 0x14302C8E0 bitfield semantics (first-person/alpha-pass/CK bits) inferred from PerFrame CB field usage; decompiler output there was mangled.
- Particle staging buffers 0x143283BB0/0x143477BB0 sizes not measured.
- FUN_1405B29F0 / queue 0x141EC4320 (DS target 5, render mode 27, states 3→4) is a separate shadow-like path in Main::Swap whose exact identity (cached/unowned shadowmaps) is unconfirmed.
- BSShadowLight shadowmap-data struct layout is partial (verified offsets only: +0 viewMat, +64 camera, +72 released ref, +84 dsTarget, +88 slice, +208..220 clip, +224 accumulator, +232 cube flag, stride 240).
- BSShader+0x90 scratch verified on BSUtilityShader only; other shader classes assumed to follow the same SetupTechnique-writes / SetupGeometry-reads pattern but not individually audited.

================================================================================================
## Shadow cluster 5
================================================================================================

# Sun-shadow cascade / shadow-map render loop — SkyrimSE 1.5.97 (base 0x140000000)

## Loop entry (addr)

**`0x1412E3480`** (`DrawWorld::RenderShadowmaps`, auto-named FUN_1412e3480) is the per-frame driver that renders **every queued shadow map (sun cascades, focus maps, spot, parabolic)**. It iterates the ShadowSceneNode's shadow-caster-light array and calls each light's virtual **`+0x50` (vtbl idx 10) = `BSShadowLight::RenderShadowmaps`**:

```c
BSShadowParabolicLight *__fastcall FUN_1412e3480()   // 0x1412E3480
{
  v0 = shadowSceneNode;                              // global 0x141E0DED0
  tls[+1896] = 29;                                   // memory-context tag (TLS idx dword 0x143497408)
  a2 = 0;                                            // cumulative shadow-map cursor
  for ( result = ShadowSceneNode::GetShadowCasterLightArrayEntry_1412BC7D0(v0, 0);
        result;
        result = ShadowSceneNode::GetShadowCasterLightArrayEntry_1412BC7D0(v0, a2) )
  {
    (result->vft->Func10_50)(result, &a2);           // BSShadowLight::RenderShadowmaps(light, &cursor)
  }
}
```

It is called from **`DrawWorld::MainAccum` 0x1412E2A60** (at 0x1412e2ba6), *concurrently* with two `SceneListAccumRegister` jobs (job manager `0x143233208`, worker fn `BSAccumProcess::DoSceneListAccumRegisterJob` 0x1412D6FB0) that register the main-view scene lists into the z-prepass accumulator `unk_1432333F8` (renderMode 12) and the main accumulator `0x143233400`; `JobList::Finish` follows the loop. So on the vanilla frame, **shadow-map GPU submission overlaps main-view pass registration** — the engine already treats it as an independent work item.

The directional (sun) light override of Func10 is **`BSShadowDirectionalLight::Func10_141324C30` = the cascade loop**; the per-map body shared by all light types is **`BSShadowLight::RenderShadowmap` = `0x141305610`** (auto-named BSShadowParabolicLight::sub_141305610 but sits in the BSShadowLight vtable region and is called by all three Func10 overrides).

## Full decompile

### Cascade loop — `BSShadowDirectionalLight::Func10_141324C30` (RenderShadowmaps, directional)

```c
int32 __fastcall BSShadowDirectionalLight::Func10(BSShadowDirectionalLight *a1, uint32 *a2 /*cursor*/)
{
  // (A) OPTIONAL volumetric-lighting shadow cascades (only when dword_141E0E2E8 == 2)
  if ( dword_141E0E2E8 == 2 )
    for ( i = 0; i < *(a1+320) /*numShadowmaps=cascade count*/; ++i ) {
      v5 = 240*i;
      *(*(a1+328) + v5 + 84) = 3;                 // depth-stencil target index 3 (VL shadowmaps)
      *(*(a1+328) + v5 + 88) = i;                 // array slice = cascade index
      a3 = 0;
      RenderShadowmap_141305610(a1, *(a1+328)+v5, &a3, 0x100);  // flags 0x100 = VL branch
    }
  // (B) THE SUN CASCADES
  for ( j = 0; j < *(a1+320); ++j ) {             // numShadowmaps (per fShadowDistance splits, set via Func11_141304BF0)
    v9 = 240*j;
    *(*(a1+328) + v9 + 84) = 2;                   // depth-stencil target index 2 (dir cascade array)
    *(*(a1+328) + v9 + 88) = j;                   // slice = cascade index
    RenderShadowmap_141305610(a1, v9 + *(a1+328), a2, 0);
  }
  // (C) FOCUS SHADOW MAPS (if a1+1368 flag; entries INLINE at light+352+240k)
  if ( a1[1368] )
    for ( k = 0; k < unk_1431D0FB8._used /*iNumFocusShadow*/; ++k ) {
      v11 = &a1[240*k + 352];  a3 = 0;
      *(v11+84) = 4;                              // depth-stencil target index 4
      v11[232] = 1;                               // force clear
      *(v11+88) = k + 4;                          // slice k+4
      RenderShadowmap_141305610(a1, v11, &a3, 0);
    }
}
```

### Per-map body — `BSShadowLight::RenderShadowmap` `0x141305610` (shared: dir cascade, focus, spot, parabolic)

```c
int32 __fastcall RenderShadowmap(BSShadowLight *a1, ShadowMapData *a2, uint32 *a3, uint32 a4 /*flags*/)
{
  ... release stale focus-target refs (a1+1312 array) ...
  ++*a3;                                                    // advance caster-array cursor
  if ( *(a2+84) == -1 ) {                                   // local-light pool path (spot/parabolic)
    *(a2+84) = 4;                                           // DS target 4 = shared local shadowmap array
    for ( i = 1, v9 = 0; (i & dword_141E10538) == 0; ++v9 ) i *= 2;   // scan free-slice bitmask
    dword_141E10538 &= ~i;  *(a2+88) = v9;                  // claim slice
  }
  BSGraphics_Renderer_SetClearColors_140d6a6d0(0x143028490, .., 1.f,1.f,1.f,1.f);
  if ( *(a2+232) ) {                                        // clear/valid flag (default 0x100 -> byte+233=1; +232 forced for focus)
    RenderTargetManager::SetDepthStencilRenderTarget(0x14302BB20, *(a2+84), 1 /*=clear DEPTH*/, *(a2+88));
    RenderTargetManager::SetRenderTarget(0x14302BB20, 0, -1 /*no color RT*/, 3, 1 /*update viewport*/);
    FUN_140d6a330(0x143028490, 0);                          // FLUSH: OMSetRenderTargets(0, [], dsv-slice) + ClearDepthStencilView(DEPTH)
  }
  NiCamera::RenderPreAndPostResolveDepth_1412C15C0(*(a2+64) /*camera*/, *(a2+72) /*accumulator*/, a4 | 0x400);
  BSGraphics__Renderer_SetClearColorFromArray(0x143028490); // restore clear colors
  // then: rebuild the ShadowMapProj matrix for the shadowmask pass:
  v14 = *(a2+64);                                           // camera: world pos +160.., rotation cols +124..156
  ... focus maps rescale near/far + x-extent by rendered-viewport/RT-dims (mgr 0x14302BB20 + 4*target + 798/799)
      using a2+208..220 (shadowMapRect) ...
  FUN_140d7bbc0(0x14302C890, v14+160, right, up, look, nearFar, port);  // compute view-proj into renderer scratch
  // scratch matrices read from 0x143028260/270/280/290 (+ 0x1430282A0..D0 alt set),
  // focus maps multiply in a 0.5-bias texture matrix (FUN_14044b4a0), translation row rebuilt from camera pos;
  *(a2+0)  = row0; *(a2+16) = row1; *(a2+32) = row2; *(a2+48) = row3;   // ShadowMapData.shadowMapProj (used by RENDER_SHADOWMASK)
}
```

### The flush into the batch renderer — `NiCamera::RenderPreAndPostResolveDepth` `0x1412C15C0` → `0x1412C1600` → accumulator vtbl idx 42

```c
void *NiCamera::Render_1412C15C0(NiCamera *cam, BSShaderAccumulator *accum, uint32 flags) {
  NiCamera::sub_1412C1600(cam, accum, flags);
  return (accum->vft->Func43_158)(accum, flags);       // 0x1412CAC90: if renderMode==0 -> sub_1412CB2E0 (main-scene path); then NiAccumulator::Func38 (accum+16=0)
}
int32 NiCamera::sub_1412C1600(NiCamera *a1, BSShaderAccumulator *a2, uint32 a3) {   // 0x1412C1600
  BSGraphics::State::SetCameraData(0x14302C890, a1, a3);           // 0x140D7BAB0 (known)
  if ( a3 & 0x400 )
    Renderer::UpdateViewPort(0x143028490, 0, 0, 1 /*ForceMatchRenderTarget*/);      // 0x140D69D00: full-RT viewport, NO dynamic-resolution scale
  MEMORY[0x1431D0E68] = a1;                                        // current camera global
  BSShaderAccumulator::SetCurrentAccumulator_1412966B0(a2);        // MEMORY[0x1431D0E20] = a2
  a2->vft->Func37_128(a2, a1);                                     // StartAccumulating
  return (a2->vft->Func42_150)(a2, a3);                            // 0x1412CAC20: THE FLUSH DISPATCH
}
void BSShaderAccumulator::Func42_1412CAC20(BSShaderAccumulator *a1, uint32 a2) {
  BSShaderManager::SetRenderMode(*(a1+0x150));       // 0x141295E90: 0x1431D0E28=mode; 0x1431D1C30/0x1431D1C38 = current register/flush fns from tables 0x1431D1B40 / 0x1431D1C40
  v4 = *(a1+0x150);                                  // accumulator renderMode
  if ( v4 ) { v5 = table_1431D1C40[v4]; (v5 ? v5 : table_1431D1C40[0])(a1, a2); }
  else BSShaderAccumulator::FinishAccumulating(a1, a2);            // 0x1412CACD0
}
```

**Shadow-map render modes: 13 = spot (BSShadowFrustumLight), 14 = dir cascades + focus, 15 = parabolic** (`mov dword ptr [accum+150h], 0Dh/0Eh/0Fh` at 0x14132d3a1 / 0x141305592 & 0x14132761d / 0x14132d988+0x14132db29). Table `0x1431D1C40` entries 12..17 all point at the **shadow/depth flush handler `BSShaderAccumulator::sub_1412CC3C0`**:

```c
void BSShaderAccumulator::sub_1412CC3C0(BSShaderAccumulator *a1, uint32 a2) {   // modes 12..17
  if ( !*(a1+16) ) return;                            // nothing accumulated
  v4 = ((a2 & 0x22) == 0x20);  if (v4) FUN_1412e1e80(a1);        // grass-shadow state bracket
  if ( a2 & 0x100 ) {                                 // VL shadow map branch (flags 0x100)
    if (unk_143027F4C) { dirty |= 0x20; unk_143027F4C = 0; }     // depth-bias/rasterizer dirty (S-block 0x143027EB0)
    pass = *(*(a1+0x130) + 232);                                  // persistent VL pass list
    if (pass & persistent) RenderPersistentPassList(pass, a2);    // 0x141306240
    else RenderBatches(a1, 1, 0x5C000074, a2, 15);                // VL-specific group
    goto done;
  }
  RenderBatches(a1, 0x2B,       0x4000002B, a2, -1);  // main opaque batch groups
  RenderBatches(a1, 0x5C000030, 0x5C00005C, a2, -1);  // grass dir-only
  ... blood-splatter group (idx1 window 0x5C006074), LowAniso (idx9), then Decals via sub_1412CBCC0 ...
  if (v4) FUN_1412e1fe0();
}
```

`RenderBatches` (= `BSShaderAccumulator::RenderGeometryGroup` **0x1412CCE40**) is the known ground-truth walker: sets current accumulator (0x1431D0E20), seeds group cursor via `BSBatchRenderer::sub_141307DD0`, then loops **`BSBatchRenderer::BeginPass 0x141308030`** (or alpha-walker 0x1413083B0 for groups 0x5C000058..5B) until exhausted, ending with `FUN_141308760` (end-technique: releases current shader/technique globals `0x143283BA4/0x143283BA8`, clears `0x143490BB0`). BeginPass bottoms out in **`RenderPassImmediately 0x141308440`** as established.

### D3D11 primitives touched per map (the deferred-context inventory)

- `RenderTargetManager::SetDepthStencilRenderTarget` **0x140D74D10**: pure state-cache write — `dword_143027EBC[11]=dsTarget, [12]=slice, [23]=depthMode(1=clear depth pending)`, dirty bit 1 on `0x143027EB0`. (dword_143027EBC = 0x143027EBC = S-block+0xC.)
- `RenderTargetManager::SetRenderTarget` **0x140D74CF0** → `BSGraphics::Renderer::SetRenderTarget` **0x140D74EC0**: RT slot cache at `0x143027EC8+4*slot`, mode at `0x143027EF8+4*slot`, `dword_143027EBC[13] = -1` (viewport-source marker), dirty bit 1; `updateViewport` arg → `Renderer::UpdateViewPort 0x140D69D00`.
- `Renderer::UpdateViewPort` **0x140D69D00**: writes viewport x/y/w/h to `dword_143027EBC[25..28]`, dirty bit 2; dims come from RT-manager getters (depth-target dims when RT slot0 == -1, i.e. shadow maps); `ForceMatchRenderTarget=1` (the 0x400 flag) **bypasses the dynamic-resolution scale** at `0x14302C98C` → full shadow-map viewport (`bDirShadowMapFullViewPort` behavior).
- `FUN_140d6a330(0x143028490, 0)` **0x140D6A330** = *output-merger flush + pending clears*, all through the immediate context `MEMORY[0x1430261B0][926]` (== `*(ID3D11DeviceContext**)0x143027EA0`): `OMSetRenderTargets` (vtbl idx 33) with the DSV picked from renderer DSV arrays (`renderer + 8*(slice + 19*dsTarget) + 8128`, read-only variant at `+0x2000` when depth-readonly flag `renderer+400`); `ClearRenderTargetView` (idx 50, clear color at renderer+10104 set by `SetClearColors 0x140D6A6D0`); `ClearDepthStencilView` (idx 53; depthMode 0→DEPTH|STENCIL, 1→DEPTH, 2→STENCIL; marks `dword_143027EBC[23]=4` cleared); plus deferred `OMSetDepthStencilState`/`RSSetState`/`RSSetViewports`/`OMSetBlendState` from S-block dirty bits 0xC/0x1070/2/0x80.
- The draws themselves then flow through `SetDirtyStates 0x140D705B0` inside `RenderPassImmediately` as already RE'd.

## Per-cascade sequence (exact order, per shadow map)

1. **DS-target select** (in Func10, before the call): `data+84 = 2` (cascades) / `3` (VL) / `4` (focus & pool lights), `data+88 = slice` (cascade index; focus k+4; pool = free bit of `dword_141E10538`).
2. **Clear color set**: `SetClearColors(0x143028490, 1,1,1,1)` (shadowmaps cleared white where color RTs exist; here only depth is bound).
3. **Bind + clear** (only if `data+232` valid/clear flag): `SetDepthStencilRenderTarget(mgr=0x14302BB20, target, mode=1, slice)`; `SetRenderTarget(slot0, -1, 3, updateViewport=1)`; `FUN_140d6a330(renderer, 0)` → `OMSetRenderTargets(0 RTVs, DSV slice)` + `ClearDepthStencilView(DEPTH, 1.0)`.
4. **Camera + viewport**: `NiCamera::sub_1412C1600`: `SetCameraData(0x14302C890, shadowCam)` (VP constants), `UpdateViewPort(force=1)` because flags|0x400 (full-RT viewport, no DRS), current-camera `0x1431D0E68`, current-accumulator `0x1431D0E20`.
5. **Batch flush**: `accum->Func42(flags)` → `SetRenderMode(13/14/15)` → `sub_1412CC3C0` → `RenderBatches` group windows → `BeginPass 0x141308030` → `RenderPassImmediately 0x141308440` per `m_PassGroupNext` pass. Then `Func43` → `NiAccumulator::Func38` (accum+16 = 0).
6. **ShadowMapProj rebuild**: view-proj computed into renderer scratch `0x143028260..0x1430282DF` (`FUN_140d7bbc0`), focus/pool maps get viewport-ratio near/far correction + 0.5-bias texture matrix (`FUN_14044b4a0`), stored to `data+0..63` — consumed later by the shadowmask pass (`ShadowMapProj[n]` cbuffer values).

**Camera & accumulator ownership (private state!)**: each 240-byte `ShadowMapData` owns `+64 NiPointer<NiCamera>` and `+72 NiPointer<BSShaderAccumulator>`; both are lazily created per cascade inside `BSShadowDirectionalLight::Func16_1413251C0` (cascade frustum update; `accum+0x148 = shadowSceneNode` @0x14132766a, `accum+0x150 = 14` @0x14132761d) and for focus maps in `BSShadowDirectionalLight::sub_1413054C0` (0x141305592). Accumulator fields: `+0x148` ShadowSceneNode*, `+0x150` renderMode, `+0x160` shadowmapIndex+1 (0xFFFF = main), `+0x164` shadowmask bit, `+0x168` split index, `+0x12E` byte flag (set 1 in Func9). So **each cascade already has its own accumulator + camera** — the pass lists are private per map; only the S-block/renderer/context state is shared.

**ShadowMapData (240B) layout**: `+0..63` shadowMapProj (4x4), `+64` camera, `+72` accumulator, `+80/84` (-1/dsTarget), `+88` slice, `+92..187` 6×NiPlane cull planes, `+188/+192` plane masks (63), `+204` float (fade=1.0 on release), `+208..223` shadowMapRect L/R/B/T (focus), `+224` BSCullingProcess* (197112B, portal-graph entry at cullProc+197008), `+232` dword flags (init 0x100; byte+232=needsRender/clear, byte+233=accumulate).

## Callers up to frame render

```
Main::Update_1405B2FF0 (0x1405B2FF0)                      // game frame
 └─ 0x1405b35c2 → Main::Swap_1405B1020 (0x1405B1020)      // render frame body: Renderer Lock, per-frame buffers,
    ├─ per-queued-request FUN_1405b29f0 (0x1405B29F0)     //   one-shot mode-27 accum renders (DS target 5, occl. query bracketed 0x140d70190/1b0 = ID3D11 Begin/End on ring 0x143025F18)
    └─ Main::sub_1405B1710 (0x1405B1710)                  // sets shadowSceneNode->cameraPos, clear colors
       └─ Main::Draw_1405B1860 (0x1405B1860)              // = DrawWorld/PreRender: the full frame
          ├─ jobs: DrawWorld_BuildSceneLists_1405B7C80 (main-view + shadow-space culling seeds)
          ├─ NiCamera::CalculateAndDrawShadowCasterLights_1412E2660   // CULL SIDE (see below)
          ├─ DrawWorld::MainAccum (0x1412E2A60)
          │   ├─ queue 2× DoSceneListAccumRegisterJob_1412D6FB0 (mgr 0x143233208)
          │   └─ FUN_1412E3480  ◄── SHADOW-MAP RENDER LOOP (GPU submission)
          ├─ Main::RenderDepth (0x1412E3520)              // z-prepass (accum unk_1432333F8, mode 12)
          ├─ Main::RenderShadowmasks (0x1412E3AC0) → FUN_1412E3B80    // screen-space shadow MASK pass
          ├─ Precipitation::RenderOcclusion (0x1403AE860)
          └─ Main::RenderWorld (0x1412E3E70) ...
```

**Cull side (runs BEFORE the render loop, same frame)**: `NiCamera::CalculateAndDrawShadowCasterLights 0x1412E2660` queues per-shadow-space jobs `FirstListAccumulationJob 0x1412E2C50` / `ListAccumulationJob 0x1412E2DE0` (mgr `0x143233200` "SceneListAccumCulling", one worker per shadow space `MEMORY[0x143233438]`, spaces array `unk_143233470` of BSGeometryListCullingProcess), and on the calling thread runs `CalculateActiveShadowCasterLights 0x1412E2F60`: budget of **4 shadow-casting lights/frame** (dir light first via `shadowSceneNode->shadowDirLight` vfunc `+0x48` Func9); per light: `Func16_+0x80` (dir 0x1413251C0 = cascade frusta from `fShadowDistance 0x141E10978`/`fSunUpdateThreshold`, lazy accum/camera create) then `Func9_+0x48` (dir 0x141324B40) which per cascade tags the accumulator (+0x160/164/168) and calls `sub_141305240` → `FUN_1412d6bb0` → `BSCullingProcess` cull + `sub_140D51280` (registers every collected BSGeometry into the accumulator via the per-mode registration table `0x1431D1B40[mode]`, building the BSRenderPass groups that BeginPass later walks). Directional also has the job-parallel variant `FUN_141324e00` (per-space `DoSceneListAccumCullingJob 0x1412D6FA0` on the light's own culling-process array light+1408).

## Shadow-map kinds

| Kind | Loop | DS target / slice | renderMode | Flush handler |
|---|---|---|---|---|
| **Sun cascades** | `Func10_141324C30` loop B, count `light+320` (cascade count from `BSShadowLight::Func11_141304BF0`) | **2**, slice = cascade idx | **14** | `sub_1412CC3C0` full batch set |
| **VL shadow cascades** | `Func10_141324C30` loop A, gated `dword_141E0E2E8 == 2` | **3**, slice = cascade idx | 14 (flags 0x100) | `sub_1412CC3C0` VL branch (group 0x5C000074 / persistent list) |
| **Focus shadows** | `Func10` loop C, count `unk_1431D0FB8._used` (iNumFocusShadow), camera/viewport per `sub_141304DA0` (`focusShadowMapDoubleEveryXUnit 0x141E10560`, `iShadowMapResolution 0x141E10548`) | **4**, slice k+4, forced clear | **14** | same |
| **Spot (BSShadowFrustumLight)** | `Func10_14132D040` → same `0x141305610` | **4**, slice = pool bit from `dword_141E10538` | **13** | same |
| **Parabolic (omni)** | `Func10_14132E4E0` | 4, pool slice | **15** | same |
| Shadow **mask** (screen space, not a map) | `Main::RenderShadowmasks 0x1412E3AC0` → `FUN_1412E3B80`: DS 3 mode 3 + RT 18, one `MakeRenderPass`+`RenderPassImmediately(0x141308440)` per caster with techniques RENDER_SHADOWMASK 0x200000 / SPOT 0x400000 / PB 0x800000 / DPB 0x1000000 | — | — | direct RenderPassImmediately |

Slot release: `BSShadowLight::Func12_141304D30` returns pool slices to `dword_141E10538` (called per light after the mask render in `FUN_1412E3B80`).

## Globals the loop touches (detour checklist)

Render state: S-block `0x143027EB0` (dirty flags; `0x143027EE8/EEC/F18` DSV target/slice/mode; `0x143027EC8..` RT slots; `0x143027F14` viewport-source marker [13]; `0x143027F20..F2C` viewport; `0x143027F48/F4C/F50/F54/F58/F5C/F60` depth/raster/alpha state ids); immediate context `MEMORY[0x1430261B0][926]`; renderer `0x143028490` (clear colors +10104, RTV/DSV arrays, view-proj scratch `0x143028260..2DF`); RT manager `0x14302BB20` (RT/DS dims at `+4*idx+798/799` etc.); camera state `0x14302C890`; DRS scale `0x14302C98C` / flag `0x14302C9A0`. Shader-manager: current accumulator `0x1431D0E20`, render mode `0x1431D0E28`, current camera `0x1431D0E68`, registration table `0x1431D1B40[32]`, flush table `0x1431D1C40[30]`, current reg/flush fns `0x1431D1C30/0x1431D1C38`, current shader/technique `0x143283BA8/0x143283BA4`, current pass `0x143490BB0`. Shadow: shadowSceneNode `0x141E0DED0` (2nd SSN slot 0x141E0DED8), slice pool `0x141E10538`, shadow-space count `0x143233438` + array `0x143233470`, per-space clear-lists `0x143233440`, caster-light array via `unk_1432334C0`, active-count `0x1432334D0/D4`, focus array `0x1431D0FA8/0x1431D0FB8`, job managers `0x143233200/0x143233208`, TLS tag `TLS[0x143497408]+1896`.

## CAVEATS

- `BSShadowDirectionalLight::Func16_1413251C0` (cascade frustum/space update, ~54KB pseudocode) was not fully decompiled; the lazy per-cascade accumulator creation (mode-14 store at 0x14132761d, SSN store at 0x14132766a, ptr-compare/store at 0x14132755a/0x1413275f4) is verified from disassembly windows only. The per-cascade **camera** create/assign site inside Func16 is inferred from the identical focus-map pattern (`sub_141304DA0`), not read line-by-line.
- `dword_141E0E2E8 == 2` (gate for the VL cascade pre-render into DS target 3) was identified as the value cell of `iEnableShadowCastingFlag:Display` (Setting object `0x141E0E2E0`) by adjacency; the setting's semantics were not verified in the settings-registration code.
- Depth-stencil target index semantics (2 = dir cascade array, 3 = VL shadowmaps, 4 = focus/local pool with 19 slices max per `19*target` DSV stride) are derived from these code paths; the RT-manager creation tables (formats/dims/array sizes) were not dumped.
- `sub_141305240`'s 5th arg `i + 11` lands in a cull-descriptor field (~+0x58); its consumer was not pinned down (suspected debug/event or scrap-arena tag, NOT the renderMode — renderMode 14 is set at accumulator creation). Decompiler stack layouts of the 0x68-byte descriptor differ slightly between callers; field map not finalized.
- `DoSceneListAccumRegisterJob_1412D6FB0` / `DoSceneListAccumCullingJob_1412D6FA0` internals were not decompiled (assumed register/cull as named); ditto `FUN_1412e1e80/FUN_1412e1fe0` (grass-shadow state bracket, only taken when `(flags & 0x22) == 0x20`, never in the paths seen here).
- The accumulator pool `0x1431D0E38` (count `0x1431D0E2C`, cursor `0x1431D0E30`, accessor chunk at 0x141296960) appears unreferenced/dead in 1.5.97 — shadow accumulators observed are heap-created per map instead.
- Purposes of Main-ctor accumulators `unk_143233408` (mode 16), `unk_143233410` (22), `unk_143233420` (23) were not chased; `unk_1432333F8` (mode 12) = z-prepass/RenderDepth accumulator is inferred from `Main::RenderDepth` usage but its body (0x1412E3520) was not fully read.
- `ClearDepthStencilView` depth/stencil clear values are dropped by the decompiler (float/imm args); assumed 1.0/0 defaults.


### Caveats
- Func16_1413251C0 (cascade frustum update) not fully decompiled; lazy accumulator create + mode-14 store verified from disasm windows (0x14132755a-0x141327680), per-cascade camera assignment inferred from the focus-map pattern
- dword_141E0E2E8==2 VL-cascade gate identified as iEnableShadowCastingFlag:Display value cell by adjacency only; semantics unverified
- DS-target index meanings (2=dir cascades, 3=VL, 4=focus/local pool, 19-slice DSV stride) derived from usage, RT-manager creation tables not dumped
- sub_141305240 arg a5=i+11 consumer unresolved (descriptor field ~+0x58); it is NOT the renderMode
- DoSceneListAccumRegisterJob/CullingJob (0x1412D6FB0/0x1412D6FA0) and FUN_1412e1e80/1fe0 internals not decompiled
- accumulator pool 0x1431D0E38 (cursor 0x1431D0E30, accessor 0x141296960) appears dead in 1.5.97 - shadow accumulators are heap-created per map
- modes 16/22/23 accumulators (unk_143233408/410/420) and Main::RenderDepth body not chased; unk_1432333F8 = z-prepass accumulator is inferred
- ClearDepthStencilView clear values (assumed 1.0/0) dropped by decompiler


================================================================================================
## Shadow-cluster verification CORRECTIONS (authoritative — override report text)
================================================================================================

1) WRONG OFFSET — focus-shadow enable byte is this+0x558, not this+0x550. BSShadowDirectionalLight::Func10 0x141324d05: `cmp [rbx+558h], bpl` (rbx=this); same field (_pad_8[1360] = 8+1360 = 0x558) gates the focus loop in BSShadowFrustumLight::Func10 0x14132d05b. All other descriptor offsets in that section verified exact (count +0x140, array ptr +0x148, focus descs embedded at +0x160, stride 240, dword +84/+88, byte/word +232).

2) WRONG STRUCTURAL CLAIM — parabolic light does NOT reuse "the same desc each time" for its two halves. BSShadowParabolicLight::Func10 0x14132E4E0 passes desc = *(this+0x148) + 240*i (i=0,1), i.e. two distinct 240-byte descriptors. On the second half (i==1) only the DS-target index is propagated: at 0x14132e50f `*(descBase+324) = *(descBase+84)` (desc[1]+84 ← desc[0]+84); the slice field +88 of desc[1] is NOT copied, so if desc[1]+84 is thus made != -1 the lazy slot alloc in 0x141305610 is skipped and desc[1]+88 keeps its own prior value — a deferred replica must treat the halves' target/slice independently, not as one shared descriptor. (ShadowSign 0x141E10B7C = +1.0 half0 / -1.0 half1 and ShadowRadius 0x141E10B78 = light->Radius confirmed.)

3) MERGED-PSEUDOCODE INACCURACY (behavioral only for modes the shadow path never uses) — the report's "shared bit-1 logic" (read-only-depth flag cleared when mode <= 2 || == 6; clear-flags case 0|6 → 3) is exactly true only of SetDirtyStates 0x140D705B0 (verified at 0x140d706d9 / 0x140d7072f switch). In FUN_140d6a330 (the explicit flush RenderShadowmap actually calls at 0x141305762) the read-only flag byte (data+34, i.e. a1+0x32 at 0x140d6a43e) is cleared ONLY when mode == 0, and mode 6 does NOT issue a ClearDepthStencilView (only modes 0→3, 1→1, 2→2 clear; disasm 0x140d6a47e-0x140d6a497). For the shadow-map path (mode 1) the two flushes behave identically, so the replica contract is unaffected, but the merged presentation is wrong for modes 0 (RO-flag parity) and 6.

4) MINOR/UNVERIFIED LABEL — stateUpdateFlags bit4 (0x10) is described as "depth-enable part of DSS", but in both flushes DSS is issued on mask 0xC only, while 0x10 is consumed solely by the raster-state mask 0x1070 (RSSetState). The 0x10 bit is a raster-group trigger; the "DSS" attribution is not supported by the flush code.

Everything else re-derived and CONFIRMED at instruction level: RenderShadowmap 0x141305610 body and all six callsite addresses/args (SetClearColors(1,1,1,1) @0x141305711; SetDSRT(mgr,+84,1,+88) @0x14130573b; SetRenderTarget(mgr,0,-1,3,1) @0x141305754; FUN_140d6a330(0x143028490,0) @0x141305762; NiCamera render(+64,+72,a4|0x400) @0x141305777; restore @0x141305783); lazy pool alloc (+84==-1→4, lowest free bit of dword_141E10538, +88=slice); SetDepthStencilRenderTarget 0x140D74D10 writes 0x143027EE8/0x143027EEC/0x143027F18 + dirty bit 1 + mode!=3 re-dirty; SetRenderTarget 0x140D74EC0 (mode5→CopyResource vfunc +0x178 on ctx, slots 0x143027EC8+4s, modes 0x143027EF8+4s, cubemap reset 0x143027EF0, updateViewport→UpdateViewPort(a1,0,0,0)); wrapper 0x140D74CF0 forwards with this=0x143028490; flush bit-1: RTV=data+2648+48i (disasm [rbp+48i+0A68h], rbp=0x143028490), clear color 0x14302AC08 (lea r8,[rbp+2778h]), DSV=data+8*(slice+19*idx)+8112 (read-only +8176, selector byte data+34), OMSetRenderTargets vfunc +0x108 with NumViews=0 when RT0==-1, ClearDepthStencilView vfunc +0x1A8 with depth=1.0f (movss xmm3, cs:0x1415232D8 = 0x3F800000) and stencil=0 at BOTH 0x140d6a4b4 and 0x140d70765; OMSetDepthStencilState +0x120 (mask 0xC, indices 0x143027F38/F40/F44), RSSetState +0x158 (mask 0x1070, indices F48/F4C/F50/F54), RSSetViewports +0x160 (mask 2, &0x143027F20) at 0x140d6a63f and 0x140d708bc, OMSetBlendState +0x118 (mask 0x80, indices F58/F5C/F60); UpdateViewPort 0x140D69D00 exact math incl. port globals 0x143028460/464/468/46C, DRS 0x14302C98C/0x14302C990/byte 0x14302C9A0, ForceMatchRenderTarget bypass, cubemap branch on 0x143027EF0==-1, dims getters 0x140D74C20/0x140D74C60 (RT props 28B/rec at mgr+0; DS props 16B/rec at mgr+3192/3196 = 0x14302C798, idx>12→0; the +798/+799 dword reads in RenderShadowmap's frustum math = byte 3192/3196, same table); Min/MaxDepth 0x143027F30/F34 ← 0x143028470/0x143028474 with MaxDepth -= float table 0x143026180[4*depthBiasMode] (subss …[rbx+rax*4]) and dirty|=2; SetMinandMaxViewportDepth 0x140D69E50; directional Func10 0x141324C30 (dword_141E0E2E8==2 → target 3/slice i/flags 0x100; main → target 2/slice j/flags 0; focus → target 4/slice k+4/+232=1, count unk_1431D0FB8._used); frustum Func10 word +232=0x0101 (*(WORD*)(v5+232)=257), slice i+4; RenderAllShadowmaps 0x1412E3480 (TLS dword 0x143497408, +1896 tag save/29/restore, GetShadowCasterLightArrayEntry 0x1412BC7D0 loop, vfunc call [r8+50h]); sub_1412C1600 (SetCameraData 0x140D7BAB0 into 0x14302C890, then a3&0x400 → UpdateViewPort(0x143028490,0,0,1)); z-prepass 0x1412E3520 (GetDepthStencilTarget_MAIN 0x140D74E50 = xor eax,ret; SetDSRT(0, mode 0, 0) @0x1412e358f; RT0/1/2=-1 mode 3; final CopyResource(unk_14302A870, DS-record[19*main].texture @unk_14302A448) — also confirms DS record base 0x14302A448 and 19-qword stride); precip FUN_1405b29f0 @0x1405b2ca2 target = edi+6 with edi=-1 → 5, slice 0, mode = cmovnz ebx (0-or-3 plausible), called from Main::sub_1405B1020 @0x1405b10eb; SetClearColors 0x140D6A6D0 saves old to 0x143025EF0..EFC and writes 0x14302AC08..; RenderShadowmasks 0x1412E3AC0 → FUN_1412e3b80, uses 0x1432334D0 count and 0x1432334E1 flag; Main::Draw 0x1405B1860 callsites 0x1405b1a24 (0x1412E2660), 0x1405b1b4c (0x1412E2A60), 0x1405b1bf5 (0x1412E3520), 0x1405b1bfc (0x1412E3AC0); MainAccum→RenderAllShadowmaps @0x1412e2ba6; context identity 0x1430261B0+8*926 = 0x143027EA0; cubemap RTV data+9936+8*(view+8*idx); xrefs to 0x141305610 are exactly the three Func10s (directional ×3, frustum ×2, parabolic ×1-in-loop). The deferred-replica contract paragraph is correct as stated, subject to correction (2) for the parabolic halves.

---

SUBSTANTIVE ERROR (call-chain step 4, wrong vfunc + missing dispatch mechanism):

1. The shadow FinishAccumulating (sub_1412CC3C0) is NOT reached via accumulator vfunc Func43 (+0x158). Verified in-binary:
   - NiCamera::Render 0x1412C15C0 calls sub_1412C1600 then Func43(+0x158).
   - sub_1412C1600 (0x1412C1600) calls SetCameraData(0x140D7BAB0, into 0x14302C890), Renderer::UpdateViewPort 0x140d69d00 when a3&0x400, sets current-camera global 0x1431D0E68, then calls Func37(+0x128) and **Func42 (+0x150)**.
   - Func42 = BSShaderAccumulator::Func42_1412CAC20 (vtable 0x14185CF50+0x150 = 0x14185D0A0 → 0x1412CAC20): calls BSShaderManager::SetRenderMode(0x141295E90) with *(accum+336), then dispatches through a RUNTIME function-pointer table at **0x1431D1C40 indexed by render mode**: `(*(&unk_1431D1C40 + mode))(accum, flags)`; mode 0 → FinishAccumulating 0x1412CACD0.
   - The table is populated at init by FUN_141294060 (0x1412947f7..0x141294939): entries [12..17] (0x1431D1CA0..0x1431D1CC8) all = **sub_1412CC3C0** (lea at 0x1412948de) — so mode 13 (shadowmap) AND mode 14 (focus shadow maps) both land there. Entry [0]/default at 0x1431D1C40 = sub_1412CABF0.
   - Func43 (+0x158) = 0x1412CAC90 does the OPPOSITE of the report's claim: `if (mode==0) sub_1412CB2E0(...)` (the MAIN-scene finish: groups 10/11/12/7, RenderEffects, FUN_140d6b210 b12 uploads) then NiAccumulator::Func38_140C907D0 (just zeroes a field). For mode 13 it renders nothing.
   - Consequence for the detour design: the shadow-loop D3D11 work is emitted under Func42's table dispatch, and 0x1431D1C40[13]/[14] is itself a clean, writable hook point (plain qword in .data, installed once). Also add 0x1431D0E68 (current-camera global set per NiCamera::Render) to the replicate list; SetCameraData runs inside sub_1412C1600 per shadowmap, not only via FUN_140d7bbc0.
   - Related precision: render mode 13 is not set "at ctor time" by FUN_1412c9c90 — the ctor is mode-agnostic; the light's Func16 sets *(accum+336)=13 after construction (verified BSShadowFrustumLight::Func16 0x14132d3a1; alloc size 384, ctor arg 0x63 ✓), and the focus-map accumulators get *(accum+336)=**14** (BSShadowDirectionalLight::sub_1413054C0 @0x141305592) — same table target, but a mode-keyed hook must cover both 13 and 14.

MINOR PRECISION (not address errors):

2. RSSetViewports arg spans dword_143027EBC[25..30] (full D3D11_VIEWPORT, 6 floats), not a "float4 @ [25..28]"; [29..30] are the viewport MinDepth/MaxDepth (which is exactly where the depth-bias trick lives — SDS/ClearFlush subtract table 0x143026180[dword_143027F50] from [30] under dirty-bit 0x40). The report's privatization list already includes [29..30], so no practical impact.

3. DSV table columns: SetDirtyStates uses base(=*0x143025F00, =0x143028480)+8112/+8176 selected by byte base+34; FUN_140d6a330 (ClearFlush) uses 0x143028490+8128/+0x2000 = base+8144/base+8208 selected by byte 0x1430284C2. So there are FOUR DSV columns (8112/8144/8176/8208 from base), not the three implied, and the two functions read DIFFERENT columns with DIFFERENT selector flags. Element index = EBC[12] + 19*EBC[11], stride 8 — as reported.

4. Step-2 prose: the directional main-cascade loop calls sub_141305610(light, desc, &counter, 0) — a4 flags are 0, not "caller flags" (only the VL pre-loop passes 0x100). The counter (a2) is what advances the light iteration in FUN_1412e3480.

5. The shadow-mask builder FUN_1412e3b80 iterates lights via a second SSN slot unk_1432334C0 (not 0x141E0DED0); techniques verified as ((useUnk?0x2100:0x2000)|0x200002)+0x2B and (v7|0x2002)+0x2B with v7 in {0x400000,0x400100,0x800000,0x1000000} — consistent with the report's "(flags|0x2002/0x200002)+0x2B" summary.

VERIFIED CORRECT (re-derived from disasm/decompile, no changes needed): 0x1430261B0[926]==0x143027EA0 single context slot, written at 0x140d71cd8 in FUN_140d718d0 (right after D3D11CreateDeviceAndSwapChain, ppImmediateContext); every cited D3D11 vtable offset/index (7,8,9,10,11,12,14,15,16,17,18,19,20,24,28,29,33,35,36,43,44,45,50,53,67,68,70) matches the SDK order at the cited call sites in SetDirtyStates 0x140D705B0 / ClearFlush 0x140D6A330 / draw helpers; FUN_140d6bfe0 body (SetDirtyStates(0); IASetIndexBuffer fmt 57 @0x140d6c05d; IASetVertexBuffers stride (4*desc)&0x3C @0x140d6c0a1; DrawIndexed 3*tris @0x140d6c0ba); FUN_140d6cab0 (2 VBs static+ring @0x140d6cba3, ring VB qword_143025F18[cursor]); fDrawGrass DrawIndexedInstanced @0x140d6c2ec args (3*tris, inst, start, 0, 0); ring allocator FUN_140d6c8a0 (End @0x140d6c8f9, GetData spin flags 4→0 + Sleep(1) @0x140d6c944, Map NO_OVERWRITE=5 @0x140d6c9a5, 4MB wrap, cursor packed in qword_143025F18[3]=0x143025F30, queries [74..76]); GetID3D11Resource 0x140d6ffd0 ((a4==7)?[924]:[779+cursor++&3], static scratch qword_14302AC58, Map DISCARD); ResetState 0x140D70290 (InitFlags(0x143027EB0); PS b11=&0x143027A28; VS+PS b12=&0x143027E88); bone setter 0x14131F630 (TLS idx 0x143497408 block +10752 dedup, 48B/bone, Unmap+VSSetCB slot 10 @0x14131f729 / slot 9 @0x14131f78b); RenderShadowmaps FUN_1412e3480 (TLS+1896=29, GetShadowCasterLightArrayEntry 0x1412BC7D0 on shadowSceneNode 0x141E0DED0, Func10 at vtable+0x50); BSShadowDirectionalLight::Func10_141324C30 (dword_141E0E2E8==2 VL pre-loop desc+84=3 flags 0x100; main loop desc+84=2 slice j; focus loop desc+84=4 slice k+4, count unk_1431D0FB8._used, desc stride 240, count at light+0x140); sub_141305610 body exactly as quoted (SetClearColors 1.0×4, needsClear +232 → SetDepthStencilRenderTarget(0x14302BB20, +84, 1, +88) 0x140d74d10 + SetRenderTarget(0,-1,3,1) 0x140d74cf0 + FUN_140d6a330(0x143028490,0), NiCamera::Render(+64,+72,a4|0x400), SetClearColorFromArray, FUN_140d7bbc0(&0x14302C890, cam+160,...), view/proj block 0x143028260..2D0 swap); sub_1412CC3C0 technique ranges (0x100→group 15; else 0x2B..0x4000002B, 0x5C000030..0x5C00005C grass, group 1 tech 0x5C006074, group 9, decals sub_1412CBCC0); BSUtilityShader Func2/4/6 = 0x14130DF90/0x14130E890/0x14130EC70 with all cited Map/Unmap/CB-slot sites (Func2 slot 0 VS@0x14130e6f4/0x14130e861 PS@0x14130e70a, Maps @0x14130e085/0x14130e0d1, Unmaps @0x14130e6c6/6de/84b; Func4 slot 1 VS@0x14130ebfa/0x14130ec51 PS@0x14130ec19, Unmaps @0x14130ebe2/0x14130ec32); scissor FUN_140d70100 (RSSetScissorRects 1 rect @0x140d7013a) called from Func6 @0x14130f025; grass instance CB slot 8 @0x1412cecbc (BSGrassShader::sub_1412CEB90, Unmap @0x1412ceca0); grass per-instance CB=[924] slot 7 @0x14130786d, Unmap @0x141307854 (case 9 of BSRenderPass::FUN_141307160, switch on BSGeometry+0x150); BeginPass 0x1413086C0 (tech dword 0x143283BA4, shader ptr 0x143283BA8, material 0x143490BB0 cleared, shader Func2 vcall); SetShader 0x140d6f9b0 (VSSetShader obj+8, cache dword_143028070[98]=0x1430281F8, dirty 0x400) and SetPixelShader 0x140d6fd60 (PSSetShader obj+8, cache [100]=0x143028200); unk_143283B78 = shadowmap slice written in FUN_1412e3b80 @0x1412e3d2d and read in Func2 @0x14130e500; shadow-mask path Main::RenderShadowmasks 0x1412E3AC0 → FUN_1412e3b80 (RT 18 @0x1412e3bd4, utility shader singleton 0x143495D50, RenderPassImmediately 0x141308440 direct); input-layout cache 0x141E07140/0x141E07160 with miss→FUN_140d70f90 under locks FUN_140d730e0/FUN_140d73f70; sampler/SRV/CS dirty-bit loops and index tables (EBC[0..2], unk_143027EB4/EB8, dword_143027F6C/FAC, qword_143027FF0) all as tabulated; frustum/parabolic Func10 0x14132D040/0x14132E4E0 route into sub_141305610 (frustum adds focus-map sub-loop, no new ctx calls); alpha-test CB [783] Map/Unmap @0x140d70951/0x140d70987; blend factor consts 0x141E07168 (SDS) / 0x141E07178 (ClearFlush); RTV columns base+2648 (SDS) / +2664-from-0x143028490 (ClearFlush) consistent with *(0x143025F00)=0x143028490-16; device ptr 0x143025F08 (written in Renderer::Init 0x140d68dd0); ClearDepthStencilView flags mapping 0/6→3, 1→1, 2→2 (the SDS 5th-arg oddity is decompiler float-register confusion, as the report already caveats).

---

1) WRONG VFUNC INDEX: 0x141310300 is BSUtilityShader vfunc +0x38 (slot 7, RestoreGeometry), NOT "RestoreTechnique" (+0x18). Verified against the BSUtilityShader vtable at 0x1418685B0: +0x10=0x14130DF90 SetupTechnique, +0x18=0x14130DD80 (the actual RestoreTechnique), +0x20=0x14130E890 SetupMaterial, +0x28=0x14130EC60, +0x30=0x14130EC70 SetupGeometry, +0x38=0x141310300. The report's two rows ("utility blend token 0x141E10660 ... consumed/restored in RestoreTechnique 0x141310300" and "BSShader+0x90/+0x94 ... read by ... RestoreTechnique 0x141310300") should say RestoreGeometry. Semantic consequence for the detour: the 0x141E10660 blend-write-mode token round-trips PER GEOMETRY/PASS (set in SetupGeometry's stencil-textured branch, consumed in RestoreGeometry immediately after the draw), not on technique switches — it still must be privatized, but its lifetime is one pass, and BSUtilityShader's real RestoreTechnique is 0x14130DD80 (invoked from BeginPass-helper 0x1413086C0 via +0x18). All addresses/behavior otherwise correct.

2) "0x141E0DF8C ... written unconditionally in 0x141308440" is wrong on the "unconditionally": decompile shows it is assigned only in the second arm of the dedup || (i.e., only when the technique/shader dedup check FAILS and BeginPass is about to run: `MEMORY[0x143283BA4]==Technique && Technique!=0x5C006076 && shader==MEMORY[0x143283BA8] || (dword_141E0DF8C = Technique, BeginPass(...))`). On dedup hits it is not touched. Same privatization verdict, but a replica must mirror the conditional order or its debug-mirror diverges.

3) Internal inconsistency in the RTV-table row: SetDirtyStates 0x140D705B0 loads the RTV pointer at `*(inst + 48*idx + 2648)` (0x140d70680). "inst+2648, stride 48, RTV at +8" is self-contradictory — as observed it is either (table base 2648, RTV at entry +0) or (table base 2640, RTV at entry +8); the load-bearing fact is the effective address inst+2648+48*idx.

Everything else re-derived and confirmed exactly: all function addresses; the entire 0x143027EB0 field map (offsets, dirty bits 1/2/0xC/0x1070/0x40/0x80/0x300/0x400/0x800, mask 0x1070 = 0x10|0x20|0x40|0x1000); D3D11 context vfunc offsets used (+64/+72/+80/+88/+112/+120/+128/+136/+192/+224/+232/+264/+280/+288/+344/+352/+360/+400/+424/+536/+544/+560); context == rendererData[926] == *(ID3D11DeviceContext**)0x143027EA0; DSV tables 8112/8176 with 19/target gated on inst byte+34 (also zeroed on clear), cube RTVs +9936, clear color +10088, DS state 8*(40*d+s), raster 72/24/2 (+240 qwords), blend 52/26/2 (+384 qwords = +0xC00) + final term dword 0x143028480 (= flt_143028470[4]); depth-bias table 0x143026180 rewriting viewport minZ/maxZ from 0x143028470 and ORing dirty bit 2; sampler 5*filter+748+addr; input-layout hashmap 0x141E07140/44/50/60 with miss->0x140D70F90 and insert/grow 0x140D730E0/0x140D73F70; hash = vertexDesc(B+0x340) & *(VS+0x48); alpha-test CB [783]@0x143027A28 (16 B, ResetState binds PS b11); PerFrame [923]@0x143027E88 720 B bound VS+PS b12 in ResetState 0x140D70290 (which also calls InitFlags and clears hazard byte inst+50=0x1430284C2); [922] 576 B, [924] 16 B (selected by GetID3D11Resource arg==7 -> b7); bone ring cursor rendererData[778]=0x143027A00 advancing (c+1)&3 over 4x3840 B CBs [779..782] created in 0x140D720D0, scratch qword_14302AC58 (and [2] = QPC at present 0x140D6A2B0, gated on byte_141E07128); dynamic-VB ring 0x143025F18: 3 buffers, packed u64 at 0x143025F30, 4 MB wrap, event queries at +0x250 with GetData busy-wait Sleep(1) and Map NO_OVERWRITE(5) — deferred-context-illegal as stated; top-level 0x1412E3480 (TLS 0x143497408 +1896 saved/29/restored, light vtbl+0x50); Func10 trio 0x141324C30/0x14132D040/0x14132E4E0 all calling 0x141305610, directional: stride 240 @light+320, count @+312, +84=2 cascades /3 when dword_141E0E2E8==2 /4 focus (+88=k+4, byte+232=1, focus array 0x1431D0FB8); 0x141305610: releases +1312/+1328 list, slice alloc from dword_141E10538 when +84==-1 (freed + refs released in 0x141304AF0, +224 vs light shaderAccumulator), clear colors 1.0x4, cube path via RT-manager 0x14302BB20 (dims at dword [4*ds+798/799]), NiCamera::Render(cam=+64, accum=+72, flags|0x400) at 0x1412C15C0, camera recompute via 0x140D7BBC0 into mirror 0x143028230.. and store-back to +0..63; camera chain SetCameraData 0x140D7BAB0 (TLS+10760 magic-static, cache lookup 0x140D7D7B0, INSERT 0x140D7D130, fallback 0x14302C9B0) -> 0x140D7D430 (0x250-byte mirror copy via 0x140D7D8C0, posAdjust 0x14302820C/prev 0x143028218) -> 0x140D7BEF0 -> per-frame CB 0x140D6B210 (gated on 0x14302C8E0, set at 0x140D6A0C0, cleared + frame counter 0x14302C8DC++ at present 0x140D6A2B0, swapchain holder 0x143025F10); SetShader 0x140D6F9B0 / SetPixelShader 0x140D6FD60 write B+0x348/B+0x350 + immediate VS/PSSetShader with *(obj+8); RenderPassImmediately 0x141308440 dedup trio 0x143283BA4/BA8 + 0x5C006076 + material 0x143490BB0 (SetupMaterial = vfunc+0x20), dispatch to Standard 0x141308970 (which calls 0x14131F7C0 clearing TLS+10752)/Skinned 0x1413088C0/Custom 0x141308B20; BeginPass helper 0x1413086C0 calls old-shader +0x18 then new-shader +0x10; writer sets for the technique globals match exactly (0x141307930, 0x141308030, 0x141308440, InitSDM 0x141308520, 0x1413086C0, ClearState 0x141308760, 0x1413090C0); sorted pass array = qsort(0x143490BD0, count 0x143283BA0, stride 0x28, cmp 0x141309750), reset 0x141308540; accumulator global 0x1431D0E20 set by plain-global setter 0x1412966B0 (getter 0x1412966A0), camera global 0x1431D0E68 written by NiCamera::sub_1412C1600 and read in utility Setup*; bone setter 0x14131F630 (TLS tag 26, VSSetConstantBuffers slots 10 then 9, skin stamp 0x140D74F70 reads frame counter); utility SetupTechnique 0x14130DF90: writes shader+0x90/+0x94 (a1+0x20+112/116), PS SRV slot2 cache 0x143028000 from inst qword [19*ds+1032], slot5 0x143028018 from [1033] (gated byte_141E0DE43 + focus count), hazard byte 0x1430284C2=1, forces biasMode 1 (dirty 0x40), scissor-mode 1 (dirty 0x1000), sun splits at sunShadowDirLight+0x598, Map(DISCARD) VS b0 pair +0x18/+0x20 and PS b0 pair +0x10/+0x18, config globals 0x141E10670/0x141E106A0/0x141E106B8/0x141E10B78/7C/0x141E0DE34/0x141E0DE4C/0x143283B78/B7C/B88/B90/0x1431D0FA8 all as listed; SetupGeometry 0x14130EC70: VS b2 pair +0x38/+0x40, PS b2 pair +0x30/+0x38, PS offset words +0x40.., VS offset bytes +0x50.., saves blend-write token to 0x141E10660, PS+0x08 released via 0x140D6FCF0, scissor via 0x140D70100 (RSSetScissorRects, vfunc+360) from projectedBoundingBox, calls 0x14130F960 + SetupShadowLightParameters 0x14130FBE0, reads 0x1431D0E20/0x1431D0E68/0x1431D0F88; RestoreGeometry 0x141310300 consumes 0x141E10660 (13=empty) into B+0xB0 with dirty 0x80 and writes the u64 stencil pair 0xFF00000000 to B+0x90; InitFlags 0x140D73BD0 defaults (sampler addr 3 at +252/+512 blocks, DS-mode-prev 6 at +140=0x8C); Renderer_Lock 0x140D6A080 = EnterCriticalSection(inst+10128) = 0x14302AC20. The privatize/share classification follows from these verified writers and needs no change beyond correction (1)'s naming.

---

# Adversarial verification — SkyrimSE 1.5.97 shadow-loop report

Verdict: the report is substantially correct — every major address, struct offset, vfunc index, table entry, and the call order were re-derived from the binary and match. Four concrete errors (3 offsets/addresses, 1 target-index description) require correction.

## CORRECTIONS (wrong in the report)

1. **Depth-readonly flag offset: `renderer+400` is WRONG → `renderer+0x32` (+50).**
   In `FUN_140d6a330`, the DSV read-only selector is `cmp [rbp+32h], r14b` at 0x140d6a461 (rbp = renderer 0x143028490 base), i.e. a byte flag at **renderer+50**, and it is zeroed at 0x140d6a43e when depthMode==0 (`*(a1 + 50) = 0` is byte-addressed, not `_QWORD*` indexing). DSV fetch itself is confirmed: `mov r14,[rbp+rcx*8+1FC0h]` (normal, base 8128) / `mov r14,[rbp+rcx*8+2000h]` (read-only) with `rcx = slice + 19*dsTarget` (`imul rcx, rax, 13h` at 0x140d6a454).

2. **Viewport-source marker flattened address: `0x143027F14` is WRONG → `0x143027EF0`.**
   The index is correct (`dword_143027EBC[13]`), but 0x143027EBC + 4*13 = **0x143027EF0**. 0x143027F14 would be index [22]. (Neighbors check out: [11]=0x143027EE8, [12]=0x143027EEC, [23]=0x143027F18, viewport [25..28]=0x143027F20..F2C.)

3. **Stale focus-target ref array: `a1+1312` is WRONG → pointer at `light+1320`, count at `light+1336`.**
   `sub_141305610` reads `v8 = *(light+1320)` (`_pad_8[1312]`, decompiled at 0x141305671) and count `*(light+1328+8)`= +1336 (0x14130567f), clearing the count at 0x1413056b6. `light+1312` is a *different* dword — the caster-array index written by `CalculateActiveShadowCasterLights` at 0x1412e31ea (`*(v10+1312) = v13`).

4. **Shadowmask-pass depth binding: "DS 3 mode 3" is WRONG → DS target −1 (none), depthMode 3, slice 0.**
   `FUN_1412E3B80` at 0x1412e3bba calls `SetDepthStencilRenderTarget(0x14302BB20, -1, 3, 0)`; only RT 18 is bound (`SetRenderTarget(0, 18, 0, 1)` at 0x1412e3bd4). If "DS 3" was meant as depth target index 3, that is incorrect; the correct reading is depthMode=3 with no DS target change.

## Notes (not errors, but sharpen the report)

- **Accumulator pool caveat refinement**: the pool at `0x1431D0E38` is *not* unreferenced — `FUN_141294060` allocates it (count `unk_1431D0E2C`, cursor `unk_1431D0E30`=0, per-entry 384-byte accumulators via `FUN_1412c9c90(v45, 0x63)` at 0x141294a2e..0x141294a93). Whether anything *consumes* it at runtime remains open, per the original caveat.
- Spot mode-13 store 0x14132d3a1 sits inside `BSShadowFrustumLight::Func16_14132D1D0` (frustum update), and mode-15 stores 0x14132d988/0x14132db29 inside `FUN_14132d800` — consistent with the report's "set at accumulator creation" claim, just noting the owning functions.
- `SetRenderTarget` (0x140D74EC0) has an unreported `a4==5` special case: `CopyResource` (ctx vtbl +376 = idx 47) from renderer+48*rt+2648 to +2656, then treats mode as 3. Irrelevant to the shadow path (mode 3 is passed) but relevant to a full deferred-context inventory.
- Focus SPOT shadowmask technique variant: focus-flagged lights get `0x400100` (0x400000|0x100), not bare 0x400000 (0x1412e3cfc).

## VERIFIED (re-derived, exact)

- **Driver loop 0x1412E3480**: TLS `[dword 0x143497408]+1896` saved/set to 29/restored; SSN global 0x141E0DED0; cursor-driven `GetShadowCasterLightArrayEntry_1412BC7D0`; per-light virtual call `call qword ptr [r8+50h]` at 0x1412e34eb = vtbl idx 10.
- **Call site**: `DrawWorld::MainAccum` 0x1412E2A60 calls it at **0x1412e2ba6**, after queueing 2× `DoSceneListAccumRegisterJob_1412D6FB0` (accums `unk_1432333F8` and `MEMORY[0x143233400]`) on mgr 0x143233208 and `Submit`, with `JobList::Finish` after — shadow GPU submission does overlap main-view registration.
- **Dir Func10 0x141324C30**: exact structure — count at light+320, ShadowMapData* at light+328, 240-byte stride; VL loop gated `dword_141E0E2E8==2` writing +84=3/+88=i, flags 0x100; sun loop +84=2/+88=j; focus loop gated byte light+1368, entries inline at light+352+240k, +84=4, byte+232=1 forced, slice k+4, count `unk_1431D0FB8._used`.
- **RenderShadowmap 0x141305610**: `++*a3`; pool path `*(a2+84)==-1` → 4 + free-bit scan/claim of `dword_141E10538`; `SetClearColors(0x143028490, 1,1,1,1)` (0x140D6A6D0); gate `*(a2+232)`; `SetDepthStencilRenderTarget(0x14302BB20, *(a2+84), 1, *(a2+88))`; `SetRenderTarget(0x14302BB20, 0, -1, 3, 1)`; `FUN_140d6a330(0x143028490, 0)`; `RenderPreAndPostResolveDepth(*(a2+64), *(a2+72), a4|0x400)`; clear-color restore; focus near/far+extent rescale via RT-mgr dims at float-index 798/799 (= byte +16*idx+3192/3196, confirmed in `FUN_140d74c20`) with rect a2+208..220; `FUN_140d7bbc0(0x14302C890, cam+160, cols +124..156, ...)`; scratch 0x143028260..2D0; focus 0.5-bias `FUN_14044b4a0`; rows stored to a2+0/16/32/48.
- **Camera/flush chain**: 0x1412C15C0 → sub_1412C1600 + vtbl +0x158 (idx 43, 0x1412CAC90: mode==0 → sub_1412CB2E0, then NiAccumulator::Func38); sub_1412C1600: `SetCameraData(0x14302C890,…)` = 0x140D7BAB0, `UpdateViewPort(0x143028490,0,0,1)` iff flags&0x400, `unk_1431D0E68=cam`, SetCurrentAccumulator 0x1412966B0, vtbl +0x128 (Func37), vtbl +0x150 (idx 42) = 0x1412CAC20.
- **Func42 0x1412CAC20**: renderMode at accum+0x150; `SetRenderMode` 0x141295E90 (writes 0x1431D0E28, picks from tables 0x1431D1B40/0x1431D1C40 into 0x1431D1C30/0x1431D1C38); dispatch with entry-0 fallback; mode 0 → FinishAccumulating 0x1412CACD0.
- **Flush table entries 12..17 = sub_1412CC3C0**: populated in `FUN_141294060` at 0x1412948e5–0x141294908 (0x1431D1CA0..0x1431D1CC8 = indices 12–17 off base 0x1431D1C40).
- **sub_1412CC3C0**: accum+16 gate; grass bracket `(a2&0x22)==0x20` → FUN_1412e1e80/FUN_1412e1fe0; VL branch `a2&0x100` (unk_143027F4C → dirty|0x20 on 0x143027EB0; persistent list at *(accum+0x130)+232; `RenderBatches(1, 0x5C000074, a2, 15)`); main windows `(0x2B, 0x4000002B, −1)`, `(0x5C000030, 0x5C00005C, −1)`, blood `(1, 0x5C006074, 1)` via *(accum+0x130)+120, LowAniso `(1, 0x5C000074, 9)` via +184, decals sub_1412CBCC0; RenderPersistentPassList 0x141306240.
- **RenderBatches/RenderGeometryGroup 0x1412CCE40**: seeds via `BSBatchRenderer::sub_141307DD0`, loops `sub_141308030` (BeginPass) or alpha walker `sub_1413083B0` when group−0x5C000058 ≤ 3, ends `FUN_141308760`.
- **renderMode stores**: 13 @0x14132d3a1 (`mov dword ptr [rcx+150h], 0Dh`, in BSShadowFrustumLight::Func16_14132D1D0); 14 @0x141305592 (sub_1413054C0) and @0x14132761d (Func16_1413251C0); 15 @0x14132d988 & 0x14132db29 (FUN_14132d800). Accum+0x148=SSN store confirmed @0x14132766a (`mov [rcx+148h], rax`).
- **SetDepthStencilRenderTarget 0x140D74D10**: [11]=target, [12]=slice, [23]=depthMode, dirty|1 — exact.
- **SetRenderTarget 0x140D74CF0 → 0x140D74EC0**: slot cache = 0x143027EB0+4*slot+24 (=0x143027EC8+4*slot), mode = +72 (=0x143027EF8+4*slot), `[13]=-1`, dirty|1, optional UpdateViewPort.
- **UpdateViewPort 0x140D69D00**: dirty|2; viewport → [25..28]; DRS scale floats 0x14302C98C(+0x4)/flag 0x14302C9A0, bypassed by ForceMatchRenderTarget; dims via [13]==-1 → FUN_140d74c20/c60 which use depth-target[11] dims when RT-slot0 index [3] is negative — exactly the reported `bDirShadowMapFullViewPort` behavior.
- **FUN_140d6a330** vtbl indices on ctx `MEMORY[0x1430261B0][926]`: +264=idx 33 OMSetRenderTargets; +400=idx 50 ClearRenderTargetView (clear color renderer+10104); +424=idx 53 ClearDepthStencilView (mode 0→3 D|S, 1→1 D, 2→2 S; [23]=4 after); deferred +288=36 OMSetDepthStencilState (dirty 0xC), +344=43 RSSetState (0x1070), +352=44 RSSetViewports (2), +280=35 OMSetBlendState (0x80); state-id cells 0x143027F38..F60 as listed.
- **Caller chain**: Main::Update 0x1405B2FF0 → call 0x1405B1020 @0x1405b35c2; 0x1405B1020 → 0x1405B1710 @0x1405b12fc; 0x1405B1710 → Main::Draw 0x1405B1860 @0x1405b182d; Draw → CalculateAndDrawShadowCasterLights 0x1412E2660 @0x1405b1a24 (before) → MainAccum @0x1405b1b4c. RenderDepth 0x1412E3520, RenderShadowmasks 0x1412E3AC0→FUN_1412E3B80, Precipitation::RenderOcclusion 0x1403AE860, RenderWorld 0x1412E3E70 all exist as named.
- **Cull side 0x1412E2F60**: light budget `v1 < 4`; dir light first via `shadowSceneNode->shadowDirLight` vfunc +0x48 (Func9); per pool light vfunc +0x80 (Func16) gate then vfunc +72=0x48 (Func9); portal-graph entry at cullProc+197008; focus flag byte light+1368; caster registration via SetShadowCasterLightArrayEntry_1412BC770; count → 0x1432334D0.
- **Mask pass FUN_1412E3B80**: per-caster `MakeRenderPass` + `RenderPassImmediately_141308440(pass, *(pass+0x18), 0, 0)`; techniques (base+43): 0x200000|0x2 RENDER_SHADOWMASK (grayscale variant 8448|… ), SPOT 0x400000 (focus 0x400100), PB 0x800000, DPB 0x1000000; caster array via `unk_1432334C0`; per-light vfunc +0x60 (Func12 = slot release, 0x141304D30 exists) after the pass; blend-id restore via unk_143027F60/dirty 0x80.
- **Spot/parabolic Func10**: BSShadowFrustumLight::Func10_14132D040 (calls 0x141305610 twice) and BSShadowParabolicLight::Func10_14132E4E0 — both bottom out in the shared per-map body, as claimed.

---

