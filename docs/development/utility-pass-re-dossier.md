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