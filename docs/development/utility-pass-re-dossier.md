

====================================================================================================
RESULT 1 (agent a6f820ff1a72515c3)
====================================================================================================
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

## D3D11 CALLS
ClearRenderTargetView(rtv, color) <- rtv = RD->pRenderTargets[S->m_RenderTargets[i]].RTV = *(RD + 48*rt + 0xA58) [non-cubemap, per slot with m_SetRenderTargetMode[i]==SRTM_CLEAR] or cubemap side *(RD + 8*(S->m_CubeMapRenderTargetView + 8*S->m_CubeMapRenderTarget) + 0x26D0); color = (float[4])(RD+0x2768) ClearColor; ctx from qword[0x143027EA0]
ClearDepthStencilView(dsv, clearFlags, 1.0f, 0) <- dsv = *(RD + 8*(S->m_DepthStencilSlice + 19*S->m_DepthStencil) + (RD->bReadOnlyDepth ? 0x1FF0 : 0x1FB0)); clearFlags from S->m_SetDepthStencilMode {0,6->3, 1->1, 2->2}; depth const 1.0f; stencil 0 (r15b zeroed in prologue)
OMSetRenderTargets(viewCount, rtvs[8], dsv) <- viewCount = count of non -1 entries in S->m_RenderTargets[0..7] (or 1 for cubemap path); rtvs built as above; dsv as above or nullptr when S->m_DepthStencil == -1 [gated on flags & 0x1]
OMSetDepthStencilState(stateObj, stencilRef) <- stateObj = qword[0x1430261B0 + 8*(40*S->m_DepthStencilDepthMode(S+0x88) + S->m_DepthStencilStencilMode(S+0x90))] = G->m_DepthStates[6][40]; stencilRef = S->m_StencilRef (S+0x94) [gated on flags & 0xC]
RSSetState(stateObj) <- stateObj = qword[0x143026930 + 8*(72*S->m_RasterStateFillMode(S+0x98) + 24*S->m_RasterStateCullMode(S+0x9C) + 2*S->m_RasterStateDepthBiasMode(S+0xA0) + S->m_RasterStateScissorMode(S+0xA4))] = G->m_RasterStates[2][3][12][2] [gated on flags & 0x1070]
RSSetViewports(1, &S->m_ViewPort) <- D3D11_VIEWPORT at S+0x70; MinDepth(S+0x80)/MaxDepth(S+0x84) possibly rewritten this call from S->m_CameraData.m_ViewDepthRange (S+0x5C0/S+0x5C4) and MaxDepth -= float[0x143026180 + 4*biasMode] (m_DepthBiasFactors) [gated on flags & 0x2, which the 0x40 block can set]
OMSetBlendState(stateObj, blendFactor, 0xFFFFFFFF) <- stateObj = qword[0x143026DB0 + 8*(52*S->m_AlphaBlendMode(S+0xA8) + 26*S->m_AlphaBlendAlphaToCoverage(S+0xAC) + 2*S->m_AlphaBlendWriteMode(S+0xB0) + S->m_AlphaBlendModeExtra(S+0x5D0))] = G->m_BlendStates[7][2][13][2]; blendFactor = static {1,1,1,1} at 0x141E07168 [gated on flags & 0x80]
Map(cb, 0, D3D11_MAP_WRITE_DISCARD(4), 0, &mapped) <- cb = *(ID3D11Buffer**)0x143027A28 = G->m_AlphaTestRefCB (16-byte CB); then *(float*)mapped.pData = S->m_AlphaTestEnabled(byte S+0xB4) ? S->m_AlphaTestRef(S+0xB8) : 0.0f [gated on flags & 0x300]
Unmap(cb, 0) <- same G->m_AlphaTestRefCB
IASetInputLayout(layout) <- layout = BSTScatterTable@0x141E07140 lookup of key (S->m_VertexDesc(S+0x340) & S->m_CurrentVertexShader(S+0x348)->m_VertexDescription(VS+0x48)) hashed by CRC32_u64 (0x140C06570); on miss layout = CreateInputLayoutFromVertexDesc(0x140D70F90) result (may be nullptr), cached unless (nullptr && key==0x300000000407) [gated on !isComputeShader && flags & 0x400]
IASetPrimitiveTopology(S->m_Topology) <- dword S+0x358 [gated on flags & 0x800]
CSSetUnorderedAccessViews(i, 1, &S->m_CSUAV[i], nullptr) <- per set bit i of S->m_CSUAVModifiedBits (S+0x14, cleared per-bit before call); UAV array at S+0x300 [flush section, unconditional]
PSSetSamplers(i, 1, &sampler) <- per set bit i of S->m_PSSamplerModifiedBits (S+0x08); sampler = &qword[0x143027910 + 8*(5*S->m_PSTextureAddressMode[i](S+0xBC+4i) + S->m_PSTextureFilterMode[i](S+0xFC+4i))] = &G->m_SamplerStates[addr][filter] [flush section]
PSSetShaderResources(i, 1, &S->m_PSTexture[i]) <- per set bit i of S->m_PSResourceModifiedBits (S+0x04); SRV array at S+0x140; exactly ONE slot per call (no adjacent merge in 1.5.97) [flush section]
CSSetSamplers(i, 1, &sampler) <- per set bit i of S->m_CSSamplerModifiedBits (S+0x10); sampler = &qword[0x143027910 + 8*(5*S->m_CSTextureAddressMode[i](S+0x1C0+4i) + S->m_CSTextureFilterMode[i](S+0x200+4i))] [flush section]
CSSetShaderResources(i, 1, &S->m_CSTexture[i]) <- per set bit i of S->m_CSResourceModifiedBits (S+0x0C); SRV array at S+0x240 [flush section]
ID3D11Device::CreateInputLayout(elems, numElems, pBytecode, bytecodeLen, &out) <- device = *(ID3D11Device**)0x143025F08 (G+0x18); elems built from vertexDesc bits per the table in the report; pBytecode = (uint8*)S->m_CurrentVertexShader + 0x68 (inline bytecode); bytecodeLen = *(uint32*)(VS+0x10); called only on input-layout-map miss inside 0x140D70F90

## DIVERGENCES FROM NUKEM
Compute-shader flag retention: 1.5.97 executes `S->m_StateUpdateFlags = isComputeShader ? (flags & 0x400 /*DIRTY_VERTEX_DESC*/) : 0` (disasm-verified `and edx,400h; cmovnz` at 0x140D70B20). Nukem's source says `flags & DIRTY_PRIMITIVE_TOPO (0x800)` — a transcription bug in skyrim64_test (keeping VERTEX_DESC is the logically correct behavior since that block is skipped for compute; topology IS issued for compute).
FlushD3DResources is fully inlined into SetDirtyStates in 1.5.97 (label at 0x140D70B35); there is no separate call. Nukem models it as a distinct member function.
Flush-loop order differs: binary drains CSUAVModifiedBits FIRST, then PSSamplers, PSResources, CSSamplers, CSResources. Nukem's rewrite orders PSSamplers, PSResources, CSSamplers, CSResources, CSUAV last. Only matters for compute paths (calls are independent slots), but replicate the binary order for bit-exactness.
PSSetShaderResources adjacent-slot merge (PSSSR(i,2,...) when bits i and i+1 both set) is Nukem's own optimization; the 1.5.97 binary issues exactly one PSSetShaderResources per dirty bit.
InputLayoutLock + tbb concurrent map is Nukem's replacement. The 1.5.97 binary uses an UNLOCKED BSTScatterTable<uint64, ID3D11InputLayout*> at 0x141E07140 (CRC32-of-u64 hash fn 0x140C06570, insert 0x140D730E0, grow 0x140D73F70).
Nukem writes m_DepthBiasFactors[0][biasMode] with declared dims float[3][4]; the binary indexes a flat float[12] at 0x143026180 by biasMode (0..11) — same memory, but Nukem's [0][...] notation only 'works' by overflowing rows; treat as flat.
Address relocation 1.5.23 -> 1.5.97: HACK_Globals 0x304BEF0 -> 0x3025EF0, RendererShadowState 0x304DEB0 -> 0x3027EB0 (same G+0x1FC0 relation), input-layout creator 0xD70620 -> 0xD70F90. All internal struct layouts (HACK_Globals, RendererShadowState, RendererData, VertexShader, ViewData) verified byte-identical to Nukem's headers.
State-array dimensions all confirmed identical to Nukem: m_DepthStates[6][40], m_RasterStates[2][3][12][2], m_BlendStates[7][2][13][2], m_SamplerStates[6][5]; index math verified instruction-by-instruction.
Nukem omits (via his cleaner C++) that the binary writes the flags word back to the global immediately inside the DIRTY_RASTER_DEPTH_BIAS block (S->m_StateUpdateFlags |= DIRTY_VIEWPORT stored at 0x140D70875/0x140D70893 before the viewport branch reads it) — same net effect, but relevant if hooking mid-function.

## OPEN QUESTIONS
POSITION is always emitted as DXGI_FORMAT_R32G32B32A32_FLOAT (2) with offset 0 regardless of desc bits — consistent with SSE's float3-position + packed bitangentX-in-w vertex layout, but half-precision-position meshes (if any survive at runtime in 1.5.97) would mismatch; assume the runtime vertex buffers are always full-precision (verify in the geometry/vertex-buffer cluster).
If a desc has neither position bit 44 nor 54, the POSITION element gets InputSlot = -1 -> CreateInputLayout fails -> nullptr is still passed to IASetInputLayout AND is cached for any desc other than 0x300000000407. IASetInputLayout(NULL) is legal API-wise; whether such descs ever reach a utility draw is unverified.
SRTM enum values 3 (RESTORE) and 5 (FORCE_COPY_RESTORE) are taken from Nukem, not observed in this function (only 0,1,2,4,6 appear here).
S+0x8C (Nukem m_DepthStencilUnknown / likely 'previous depth mode') is never touched by this function; its writer is in DepthStencilStateSetDepthMode (different cluster).
The alpha-test-ref CB (G+0x1B38 = 0x143027A28) is only Mapped/written here; the VS/PSSetConstantBuffers binding to slot 11 (CONSTANT_GROUP_LEVEL_ALPHA_TEST_REF per Nukem) happens elsewhere and must be captured by the shader-setup cluster.
CRC32 hash (0x140C06570, table 0x14175BF90) assumed to be standard Bethesda BSCRC32 over the 8 key bytes; polynomial not re-derived from the table.
FUN_140D73A20 (scatter-table free-entry allocator) and FUN_140D73F70 (grow/rehash) not fully decompiled — treated as standard BSTScatterTable machinery; only their contract (insert retry loop) is load-bearing here.
DepthStencilData assumed layout {Texture, Views[8], ReadOnlyViews[8], +2 trailing ptrs} = 152 bytes: Views base RD+0x1FB0 and ReadOnlyViews base RD+0x1FF0 are disasm-proven; the trailing two pointers (SRV/stencil SRV per Nukem) are inferred from the 19-qword stride only.
Viewport TopLeftX/Y/Width/Height (S+0x70..0x7C) are produced by UpdateViewPort (not in this cluster); this function only resyncs MinDepth/MaxDepth.
RENDER_TARGET count inferred ~114 from (0x1FA8-0xA48)/48 and DEPTH_STENCIL count = 12 from (0x26C8-0x1FA8)/152; enum values themselves not re-verified against 1.5.97.

## KEY ADDRESSES
0x140D705B0 BSGraphics::Renderer::SetDirtyStates(bool isComputeShader) — state flush; FlushD3DResources inlined at tail (0x140D70B35)
0x140D70F90 CreateInputLayoutFromVertexDesc(uint64 desc) -> ID3D11InputLayout* (Nukem 1.5.23: 0x140D70620)
0x140C06570 CRC32_u64 hash for BSTScatterTable (writes u32 hash to out param)
0x140D730E0 InputLayoutMap insert (returns false if table null/alloc fail; updates value on existing key)
0x140D73A20 BSTScatterTable free-entry allocator (called by insert)
0x140D73F70 BSTScatterTable grow/rehash (retry loop partner of insert)
0x14175BF90 CRC32 lookup table (BSCRC32)
0x141E07140 InputLayoutMap BSTScatterTable<uint64 vertexDesc, ID3D11InputLayout*> (cap@+4, freeCount@+8, sentinel@+0x10=0x141E0718C, entries@+0x20; 24B entries {key,value,next}; NO lock)
0x141E07168 static const float blendFactor[4] = {1,1,1,1} for OMSetBlendState
0x143025EF0 BSGraphics globals block G (Nukem HACK_Globals; 1.5.23 0x304BEF0)
0x143025F00 G+0x10 RendererData* (RD)
0x143025F08 G+0x18 ID3D11Device*
0x143026180 G+0x290 m_DepthBiasFactors float[12] (Nukem [3][4]) indexed by rasterDepthBiasMode
0x1430261B0 G+0x2C0 m_DepthStates[6][40] ID3D11DepthStencilState* (index 40*depthMode+stencilMode)
0x143026930 G+0xA40 m_RasterStates[2][3][12][2] ID3D11RasterizerState* (index 72*fill+24*cull+2*bias+scissor)
0x143026DB0 G+0xEC0 m_BlendStates[7][2][13][2] ID3D11BlendState* (index 52*mode+26*atoc+2*write+extra)
0x143027910 G+0x1A20 m_SamplerStates[6][5] ID3D11SamplerState* (index 5*addressMode+filterMode; shared PS/CS)
0x143027A28 G+0x1B38 m_AlphaTestRefCB ID3D11Buffer* (16-byte CB, slot 11 alpha-test ref)
0x143027EA0 G+0x1FB0 ID3D11DeviceContext2* immediate context (reloaded before every call)
0x143027EB0 RendererShadowState S base = G+0x1FC0 (m_StateUpdateFlags)
0x143027EB4 S+0x04 m_PSResourceModifiedBits
0x143027EB8 S+0x08 m_PSSamplerModifiedBits
0x143027EBC S+0x0C m_CSResourceModifiedBits (IDA's dword_143027EBC array anchor)
0x143027EC0 S+0x10 m_CSSamplerModifiedBits
0x143027EC4 S+0x14 m_CSUAVModifiedBits
0x143027EC8 S+0x18 m_RenderTargets[8]
0x143027EE8 S+0x38 m_DepthStencil; +0x3C slice; +0x40 cubeRT; +0x44 cubeView
0x143027EF8 S+0x48 m_SetRenderTargetMode[8]; +0x68 setDepthStencilMode; +0x6C setCubeMode
0x143027F20 S+0x70 m_ViewPort (MinDepth@S+0x80=0x143027F30, MaxDepth@S+0x84=0x143027F34)
0x143027F38 S+0x88 m_DepthStencilDepthMode; 0x143027F40 S+0x90 stencilMode; 0x143027F44 S+0x94 stencilRef
0x143027F48 S+0x98 rasterFill; +0x9C cull; +0xA0 depthBiasMode; +0xA4 scissor
0x143027F58 S+0xA8 alphaBlendMode; +0xAC atoc; +0xB0 writeMode; 0x143027F64 S+0xB4 alphaTestEnabled(byte); 0x143027F68 S+0xB8 alphaTestRef(float)
0x143027F6C S+0xBC m_PSTextureAddressMode[16]; 0x143027FAC S+0xFC m_PSTextureFilterMode[16]; 0x143027FF0 S+0x140 m_PSTexture[16]
0x143028070 S+0x1C0 m_CSTextureAddressMode[16]; 0x1430280B0 S+0x200 m_CSTextureFilterMode[16]; 0x1430280F0 S+0x240 m_CSTexture[16]; 0x1430281B0 S+0x300 m_CSUAV[8]
0x1430281F0 S+0x340 m_VertexDesc; 0x1430281F8 S+0x348 m_CurrentVertexShader (VS: bytecodeLen@+0x10, vertexDescription@+0x48, bytecode@+0x68); 0x143028208 S+0x358 m_Topology
0x143028230 S+0x380 m_CameraData (ViewData 0x250); 0x143028470 S+0x5C0 m_ViewDepthRange (NiPoint2); 0x143028480 S+0x5D0 m_AlphaBlendModeExtra
RD+0x22 bReadOnlyDepth (byte); RD+0xA48 pRenderTargets[] stride 48 (RTV@+0x10); RD+0x1FA8 pDepthStencils[] stride 152 (Views@+8, ReadOnlyViews@+0x48); RD+0x26C8 pCubemapRenderTargets[] stride 64 (CubeSideRTV@+8); RD+0x2768 ClearColor float[4]

====================================================================================================
RESULT 2 (agent a027bb1fad272ea67)
====================================================================================================
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

## D3D11 CALLS
Map(ctx, curVS->PerTechnique.buf [curVS=[0x1430281F8], buf=+0x18], 0, D3D11_MAP_WRITE_DISCARD, 0, &m) — SetupTechnique @0x14130E085; mapped ptr stored to curVS+0x20
Map(ctx, curPS->PerTechnique.buf [curPS=[0x143028200], buf=+0x10], 0, WRITE_DISCARD, 0, &m) — SetupTechnique @0x14130E0D1, only when hasPixelShader; mapped ptr to curPS+0x18
Unmap(ctx, curVS->PerTechnique.buf, 0) — SetupTechnique @0x14130E6C6/0x14130E84B
Unmap(ctx, curPS->PerTechnique.buf, 0) — SetupTechnique @0x14130E6DE (hasPS only)
VSSetConstantBuffers(0, 1, &curVS->PerTechnique.buf) — SetupTechnique @0x14130E6F4/0x14130E861; data = HighDetailRange (g_141E0DF04 − PosAdjust.xy, z/w−15) at vsOff[3], EndSplitDistances copy at vsOff[3] (focus), ParabolaParam (1/[0x141E10B78], [0x141E10B7C]) at vsOff[4]
PSSetConstantBuffers(0, 1, &curPS->PerTechnique.buf) — SetupTechnique @0x14130E70A (hasPS); data = VPOSOffset(1/RTdims from 0x14302BB20[m_RenderTargets[0]]) psOff[10], End/StartSplitDistances (depth-space cascade splits from sunShadowDirLight+0x598/+0x5A4, near/far from [0x1431D0E68]+0x160/164) psOff[11]/[12], ShadowSampleParam.zw (fPoissonRadiusScale/shadowmapRes) psOff[7], FocusShadowFadeParam[i] (fade from 0x1431D0FA8 dist² vs fMaxFocusShadowMapDistance²/fFadeStart) psOff[13]
VSSetShader(ctx, vertexShader->m_Shader(+8), NULL, 0) — Renderer::SetVertexShader 0x140D6F9B0 via BeginTechnique; shader from m_VertexShaderTable[VSID(F)] scatter lookup
PSSetShader(ctx, pixelShader ? pixelShader->m_Shader(+8) : NULL, NULL, 0) — Renderer::SetPixelShader 0x140D6FD60 via BeginTechnique; NULL for depth-only techniques (NO_PIXEL_SHADER condition)
Map/Unmap(ctx, curVS->PerMaterial.buf(+0x28), WRITE_DISCARD) — SetupMaterial 0x14130E890
Map/Unmap(ctx, curPS->PerMaterial.buf(+0x20), WRITE_DISCARD) — SetupMaterial (PS present)
VSSetConstantBuffers(1, 1, &curVS->PerMaterial.buf) — SetupMaterial @0x14130EBFA/0x14130EC51; data = TexcoordOffset float4 from material +12/+16/+28/+32 (+8*dword_141E0DFF0 buffer index) at vsOff[1]
PSSetConstantBuffers(1, 1, &curPS->PerMaterial.buf) — SetupMaterial @0x14130EC19; data = BaseColor (mat+0x48..0x54 × mat+0x6C) psOff[3], RefractionPower (mat+0x84) psOff[1]
Map/Unmap(ctx, curVS->PerGeometry.buf(+0x38), WRITE_DISCARD) — SetupGeometry @0x14130ECDF; 128B/32-float CB per the validated geomcb map
Map/Unmap(ctx, curPS->PerGeometry.buf(+0x30), WRITE_DISCARD) — SetupGeometry @0x14130ED26 (PS present)
VSSetConstantBuffers(2, 1, &curVS->PerGeometry.buf) — SetupGeometry @0x14130F8EC/0x14130F946; data = World (camera-relative FUN_1412C3440(geometry->world), transposed) vsOff[0], ShadowFadeParam.x/.z vsOff[5], EyePos vsOff[2] (accumulator+356 through FUN_140D42C50 matrix; .w=property+0x10C), WaterParams.x (water height 0x141E0E014) vsOff[7], TreeParams (leaf anim + shadowSceneNode->windMagnitude) vsOff[6]
PSSetConstantBuffers(2, 1, &curPS->PerGeometry.buf) — SetupGeometry @0x14130F90B; data = AlphaTestRef.x (effectData->alphaTestRef/255 | 0.99217 blend | threshold(+0x32)/255+eps) psOff[0], AlphaTestRef.yz = shadow bias (light->shadowBiasScale*0.00025*dirScale*0x143283B8C, and *1/3*0x141E1053C*0x143283B8C) via SetupShadowLightParameters, AlphaTestRef.w = fade alpha (Aam), ShadowLightParam.x=falloff/radius .z=lodFade?distSq:1e8 psOff[8], ShadowMapProj float3x4[3] per cascade gated by pass->extraParam bit i (light->shadowMapDataList._data stride 240, translation += M·(−lightingOffset/origin + PosAdjust)) psOff[6], FocusShadowMapProj float3x4[4] psOff[5], DebugColor psOff[2], PropertyColor (property+0xB8 float3, w=property->alpha) psOff[4]
RSSetScissorRects(ctx, 1, &{l=bb.x, t=bb.y, r=bb.x+bb.w, b=bb.y+bb.h}) — FUN_140D70100 (ctx vtable+360), called IMMEDIATELY from SetupGeometry @0x14130F025 for spot/PB/DPB shadowmask passes; rect = light->projectedBoundingBox
ID3D11PixelShader::Release() — FUN_140D6FCF0 from SetupGeometry STENCIL_ABOVE_WATER path @0x14130F3A5: releases curPS->m_Shader(+8) and NULLS it (subsequent PSSetShader of this technique binds NULL)
Map (inside GetID3D11Resource 0x140D6FFD0 dynamic-CB pool, not decompiled) + Unmap(ctx, bonesBuf, 0) + VSSetConstantBuffers(10, 1, &bonesBuf) — SetBoneMatrix 0x14131F630; data = memcpy of skinInstance->boneMatrices(+0x48), 16*3*boneCount bytes (boneCount = skinData(+0x10)+0x58)
Unmap(ctx, prevBonesBuf, 0) + VSSetConstantBuffers(9, 1, &prevBonesBuf) — SetBoneMatrix; data = skinInstance->prevBoneMatrices(+0x50)
DEFERRED (via dirty caches consumed by SetDirtyStates 0x140D705B0, not direct calls): PSSetShaderResources slots 0(base/blockout),1(normal),2(main depth SRV [0x14302A4D0]),3/4(light shadowmap depth SRV, unfiltered/filtered by 0x141E0DE30),5(main stencil SRV +8),6(SHADOWMAPS depth SRV 0x14302A730),7(grayscale); PSSetSamplers 0(addr=matClamp/3,filt=3),1(addr=matClamp,filt=3),3(0/0),4(0/4-comparison),6(0/4),7(0/1); OMSetDepthStencilState (depth modes 0/3/5, stencil {0,255}/{1,255}/{11,fade*31}); RSSetState (depth-bias mode 0/1, scissor on/off); OMSetBlendState (blend mode 0, write mode 0/1); alpha-test-ref 0

## DIVERGENCES FROM NUKEM
Nukem never RE'd BSUtilityShader as a class (no Shaders/BSUtilityShader.cpp); only the BSShaderInfo name tables exist. All tables verified correct against 1.5.97: every VS/PS constant index maps exactly to the m_ConstantOffsets slot used by the binary, including his guessed PS#13 FocusShadowFadeParam (confirmed by the focus-fade write loop at 0x14130E5B6).
The 'other CB' in SetupGeometry is the current PIXEL shader's PerGeometry buffer ([0x143028200]+0x30, bound PSSetConstantBuffers slot 2) — the earlier geomcb-map agent's label 'pCurrentVertexShader+48' was wrong; corrected here.
BSShader::SetupGeometryAlphaBlending (Nukem 1.5.23 0x1413360D0) is NOT called anywhere in the Utility path in 1.5.97; alpha-blend mode selection for utility passes lives in the BSBatchRenderer pass-walk (0x141308030 / 0x141307930 are the only per-pass writers of the alpha-blend globals 0x143027F58/F5C). Utility manages write-mode brackets itself via dword_141E10660.
BSShader::SetupAlphaTestRef semantics (testRef * property->GetAlpha() → renderer m_AlphaTestRef) do NOT apply to Utility: SetupTechnique force-clears renderer alpha-test-ref (0x143027F64=0), and the per-pass ref goes into the PS PerGeometry AlphaTestRef constant with different math — blend-enabled → 0.99217f (0x3F7DFEFF), else threshold/255 + 0.0039215293 (+1/255 extra if threshold==4), no multiply by property alpha; effectData->alphaTestRef/255 takes priority.
GetDepthStencilTarget_MAIN (0x140D74E50) is literally 'xor eax,eax; ret' in 1.5.97 — the depth/stencil SRVs always come from DS-target entry 0 (0x14302A4D0/+8); the callers' -1 checks are dead code.
BeginTechnique 1.5.97 has NO hull/domain shader support (Nukem's HullShaders/DomainShaders maps are his own addition, 'removed from SkyrimSE.exe' as he notes) — only VSSetShader + PSSetShader.
SetBoneMatrix: Nukem's GetShaderConstantGroup/Flush/ApplyConstantGroupVS abstraction resolves concretely to: dynamic-CB pool map (GetID3D11Resource 0x140D6FFD0), memcpy, Unmap, VSSetConstantBuffers with StartSlot == constant-group level (10=bones, 9=previous bones). His helper sub_140D74600 (1.5.23) = 0x140D74F70 in 1.5.97, called with skinInstance only in the decompile.
RestoreTechnique mixes flag domains: tests raw technique for the shadowmask family (a2 & 0x1E00000) but (technique−43) for the 0x2000 bit — Nukem-style code that assumes F==technique for high bits; replicate exactly as-is.
Directional cascade ShadowMapProj writes are gated per-cascade by pass->extraParam & (1<<i) — the cascade-select bitmask is carried in the BSRenderPass extraParam byte, a detail absent from Nukem's sources.
STENCIL_ABOVE_WATER path Release()es the current PixelShader's ID3D11PixelShader and nulls the table entry (FUN_140D6FCF0, unique callsite) — after first use the technique runs PS-less. Not documented anywhere in skyrim64_test.

## OPEN QUESTIONS
FUN_140D6FCF0 releasing curPS->m_Shader in the STENCIL_ABOVE_WATER path: intent presumed 'demote to PS-less after first use' (the null survives in m_PixelShaderTable), but not runtime-verified; a replicator must decide whether to reproduce the Release or just bind PS=NULL for (F&0x1200)==0x1200.
FUN_14130F960: derivation of the light's shadowmap DS-target index (v6 = *(*(light+0x18)+0x54)) — pointer-chain typing unverified (IDA's RE::BSLight stub size distorts field math); also the vtable predicate gating the t6/SHADOWMAPS bind (v8->vftable[1].SetLight name is bogus) and the light+~0x74 'shadowmap rendered' byte are unnamed.
Identity of technique flag bits 5 (Nukem 'L') and 6 — both stripped from the VS id, no other reads found in this cluster.
INI/global names for byte_141E0DE43 (gates stencil-SRV t5 bind) and byte_141E0DE4C (switches shadowmask VS to the focus/0x2002 variant and flips the SetupShadowLightParameters origin sign); also unk_143283B8C (final bias multiplier) and g_141E1053C exact meaning.
Shadow bias lands at PS AlphaTestRef.y/.z (psOff[0]+4/+8) — assumed the compiled Utility PS reads bias from that cbuffer slot (CS package/Shaders/Utility.hlsl should confirm); not cross-checked against DXBC.
Derived-property raw offsets: PropertyColor source ptr at property+0xB8, EyePos.w at property+0x10C, Aam fade override at property+0x104 — computed from Hex-Rays 'shaderProperty[1].field' expressions (sizeof(BSShaderProperty)=0xB8); which derived class (BSEffectShaderProperty/BSWaterShaderProperty?) owns them is unverified.
GetID3D11Resource (0x140D6FFD0, dynamic CB ring used by SetBoneMatrix) internals not decompiled — Map type/pool recycling assumed WRITE_DISCARD ring per Nukem's GetShaderConstantGroup.
ctor stores m_Type=8 via a decompiler-ambiguous offset expression (*(a1+8)); assumed the canonical BSShader::m_Type@0x20 — not disasm-verified.
Depth-mode enum values used (0, 5 vs default 3, plus bias modes 0/1) map to Nukem's DEPTH_STENCIL_DEPTH_MODE / rasterizer depth-bias tables in Renderer::SetDirtyStates — exact D3D11 depth-stencil desc per mode belongs to the SetDirtyStates cluster and was not re-derived here.
SetupTechnique F&0x40000 non-shadow powf write (DEBUG_SHADOWSPLIT split-display constants into EndSplitDistances.xy) purpose/consumer not verified (debug-only path).

## KEY ADDRESSES
0x1418685B0 BSUtilityShader::vftable (primary, 10 slots; _0 @0x141868608, _1 @0x141868620; 0x141990B18/0x141990B40 are RTTI COL ptrs)
0x14130DCE0 BSUtilityShader::ctor (m_Type=8, pInstance store)
0x143495D50 BSUtilityShader::pInstance
0x141310770 ~BSUtilityShader
0x140C61A30 DeleteThis
0x14130DF90 BSUtilityShader::SetupTechnique
0x14130DD80 BSUtilityShader::RestoreTechnique
0x14130E890 BSUtilityShader::SetupMaterial
0x14130EC60 BSUtilityShader::RestoreMaterial (nullsub)
0x14130EC70 BSUtilityShader::SetupGeometry
0x141310300 BSUtilityShader::RestoreGeometry
0x14131F430 BSShader::GetTechniqueName (nullsub)
0x14131F7F0 BSShader::ReloadShaders(bool) thunk → 0x14131FB10
0x14131F800 BSShader::ReloadShaders(BSIStream*)
0x14131F630 BSShader::SetBoneMatrix (bones→VS b10, prev→VS b9)
0x141334900 UtilityTechniqueToVertexShaderID
0x141334970 UtilityTechniqueToPixelShaderID
0x14131FBD0 BSShader::BeginTechnique
0x14131FCE0 BSShader::EndTechnique (nullsub)
0x140D6F9B0 Renderer::SetVertexShader (immediate VSSetShader + dirty 0x400)
0x140D6FD60 Renderer::SetPixelShader (immediate PSSetShader)
0x140D6FCF0 ReleaseShaderCOM(shader) — releases +8 and nulls (STENCIL_ABOVE_WATER)
0x14130F960 BindShadowMapTextures(pass, lightIdx) — PS t3/t4/t5/t6 + samplers
0x14130FBE0 BSRenderPass::SetupShadowLightParameters (ShadowMapProj/FocusShadowMapProj/bias)
0x140D70100 Renderer::SetScissorRect (immediate RSSetScissorRects, ctx vtbl+360)
0x1412C3440 BuildCameraRelativeWorldMatrix (scale*rotate, translate−PosAdjust) [validated]
0x140D42C50 BuildObjectMatrixForEyePos [per validated geomcb map]
0x140D74C20 GetCurrentRTWidth(0x14302BB20)
0x140D74C60 GetCurrentRTHeight(0x14302BB20)
0x140D74E50 GetDepthStencilTarget_MAIN — returns 0 constant
0x140D74F70 BSDismemberSkinInstance helper (Nukem 1.5.23 0x140D74600)
0x140D6FFD0 GetID3D11Resource (dynamic CB pool for constant-group levels)
0x140D705B0 BSGraphics::Renderer::SetDirtyStates (consumer of all dirty caches)
0x1412966A0 BSShaderAccumulator::GetCurrentAccumulator
0x1412FD8A0 BSRenderPass::GetNiProperty (alpha property)
0x141308030 BSBatchRenderer pass-list walk (owns per-pass alpha-blend state — other cluster)
0x141307930 BSBatchRenderer::Func3 (alpha-blend writer — other cluster)
0x143027EA0 ID3D11DeviceContext* global (= [0x1430261B0]+0x1CF0)
0x143027EB0 m_StateUpdateFlags
0x143027EB4 m_PSResourceModifiedBits
0x143027EB8 m_PSSamplerModifiedBits
0x143027EC8 m_RenderTargets[0] index
0x143027EE8 m_CubeMapRenderTarget index
0x143027F38 m_DepthStencilDepthMode
0x143027F3C m_DepthStencilDepthModePrevious
0x143027F40 {stencilMode,u32 stencilRef} qword
0x143027F50 m_RasterStateDepthBiasMode
0x143027F54 m_ScissorEnabled
0x143027F58 m_AlphaBlendMode
0x143027F5C m_AlphaBlendAlphaToCoverage
0x143027F60 m_AlphaBlendWriteMode
0x143027F64 m_AlphaTestRef (float)
0x143027F6C m_PSSamplerAddressMode[16]
0x143027FAC m_PSSamplerFilterMode[16]
0x143027FF0 m_PSTexture[16] SRV cache
0x1430281F8 m_CurrentVertexShader
0x143028200 m_CurrentPixelShader
0x14302820C PosAdjust.x (camera-relative origin; .y/.z at 0x143028210/0x143028214)
0x1430284C2 depth-SRV-bound flag
0x143028490 Renderer data mid-block (this for setters; flt_143028470[8])
0x14302A4D0 DS-target runtime array (stride 152: +0 depthSRV, +8 stencilSRV)
0x14302A730 SHADOWMAPS (entry 4) depth SRV
0x14302BB20 render-target properties (stride 28: w,h; cubemap dims +3192)
0x141E0DED0 shadowSceneNode (SSN[0])
0x1431D0E68 BSShaderManager camera (frustum near/far +0x160/+0x164)
0x1431D0F88 camera node (WorldTransform +0x7C)
0x1431D0E28 BSShaderManager render mode
0x1431D0FA8 focus-shadow array (stride 16, [0]=dist²)
0x1431D0FB8 focus-shadow count (_used)
0x1431D0DA8 sentinel BSFadeNode
0x141E0DF04 HighDetailRange source float4
0x141E0DFF0 texcoord double-buffer index
0x141E0DE30 shadow-filter-quality bits (drives PS id bits 17-19 + t3/t4 select)
0x141E0DE34 shadow filter mode (Poisson gate: ==2/3)
0x141E0DE43 stencil-SRV bind gate bool
0x141E0DE4C focus-shadow VS variant bool
0x143283B78 EndSplitDistances.x for local-light masks
0x143283B7C fShadowDistance
0x143283B88 ShadowDistanceSquared
0x143283B8C shadow bias final multiplier
0x143283B90 shadowmap resolution (Poisson divisor)
0x141E10670 fPoissonRadiusScale
0x141E106A0 focus-shadow fade start fraction
0x141E106B8 fMaxFocusShadowMapDistance
0x141E10A38 fShadowDirectionalBiasScale
0x141E1053C bias .z multiplier global
0x141E10B78 parabola radius / 0x141E10B7C parabola sign
0x141E0E014 water height (STENCIL_ABOVE_WATER WaterParams.x)
0x141E0DF70/0x141E0DF74 wind-curve min/max
0x141E10660 saved blend-write-mode (GRAYSCALE_MASK bracket; sentinel 13)
0x143012370 default lighting origin float3
0x143497408 TLS index (TLS+0x2A00 last NiSkinInstance, TLS+1896 memory-context)

====================================================================================================
RESULT 3 (agent af67a4ee5638499a5)
====================================================================================================


## D3D11 CALLS


## DIVERGENCES FROM NUKEM


## OPEN QUESTIONS


## KEY ADDRESSES


====================================================================================================
RESULT 4 (agent af2340ae512a8f0c8)
====================================================================================================

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


## D3D11 CALLS
VSSetShader(vs->m_Shader, NULL, 0) <- vs = m_VertexShaderTable scatter-find(vsId) result stored to 0x1430281F8; called from Renderer::SetVertexShader 0x140D6F9B0 (no null check on vs); ctx from 0x143027EA0 (all calls below same ctx)
PSSetShader(ps ? ps->m_Shader : NULL, NULL, 0) <- ps = m_PixelShaderTable find(psId), forced NULL when ignorePixelShader; stored to 0x143028200; Renderer::SetPixelShader 0x140D6FD60
Map(vs->m_PerTechnique.m_Buffer /*vs+0x18*/, 0, D3D11_MAP_WRITE_DISCARD, 0, &m) then vs->m_PerTechnique.m_Data=m.pData <- SetupTechnique of each shader (utility 0x14130DF90); only if buffer non-null
Map(ps->m_PerTechnique.m_Buffer /*ps+0x10*/, 0, WRITE_DISCARD, 0, &m) <- same, only when technique needs a pixel shader
Unmap(vs->m_PerTechnique.m_Buffer, 0) / Unmap(ps->m_PerTechnique.m_Buffer, 0) <- SetupTechnique tail
VSSetConstantBuffers(0, 1, &vs->m_PerTechnique.m_Buffer) <- SetupTechnique tail; slot 0 = per-technique; binds NULL if group has no GPU buffer
PSSetConstantBuffers(0, 1, &ps->m_PerTechnique.m_Buffer) <- SetupTechnique tail, only when PS present
Map/Unmap(vs->m_PerMaterial.m_Buffer /*vs+0x28*/, WRITE_DISCARD) + Map/Unmap(ps->m_PerMaterial.m_Buffer /*ps+0x20*/) <- SetupMaterial (utility 0x14130E890)
VSSetConstantBuffers(1, 1, &vs->m_PerMaterial.m_Buffer) + PSSetConstantBuffers(1, 1, &ps->m_PerMaterial.m_Buffer) <- SetupMaterial tail; slot 1 = per-material
Map/Unmap(vs->m_PerGeometry.m_Buffer /*vs+0x38*/, WRITE_DISCARD) + Map/Unmap(ps->m_PerGeometry.m_Buffer /*ps+0x30*/) <- SetupGeometry (utility 0x14130EC70); constants written at (float*)m_Data + m_ConstantOffsets[i] (vs table +0x50, ps table +0x40)
VSSetConstantBuffers(2, 1, &vs->m_PerGeometry.m_Buffer) + PSSetConstantBuffers(2, 1, &ps->m_PerGeometry.m_Buffer) <- SetupGeometry tail; slot 2 = per-geometry
Map(ringCB, 0, WRITE_DISCARD, 0, &m) <- Renderer::GetShaderConstantGroup 0x140D6FFD0; ringCB = level==7 ? globals+0x1CE0 : m_ConstantBuffers1[(idx++)&3] at globals+0x1858 (idx at globals+0x1850); size arg ignored; buffer ptr also stored to static 0x14302AC58
Unmap(ringCB, 0) + VSSetConstantBuffers(10, 1, &ringCB) <- BSShader::SetBoneMatrix 0x14131F630; data = memcpy of skinInstance->m_pvBoneMatrices (si+0x48), 16*3*boneCount bytes
Unmap(ringCB, 0) + VSSetConstantBuffers(9, 1, &ringCB) <- SetBoneMatrix, previous bones from si+0x50
PSSetConstantBuffers(11, 1, &g_AlphaTestRefCB@0x143027A28) <- Renderer::ResetState 0x140D70290 (per-frame, PS only)
VSSetConstantBuffers(12, 1, &g_PerFrameCB@0x143027E88) + PSSetConstantBuffers(12, 1, same) <- ResetState; per-frame CB (720B)
Map(g_AlphaTestRefCB, 0, WRITE_DISCARD, 0, &m); *(float*)m.pData = m_AlphaTestEnabled(0x143027F64) ? m_AlphaTestRef(0x143027F68) : 0.0f; Unmap <- SetDirtyStates 0x140D705B0 on flags 0x300
IASetInputLayout(layout) <- SetDirtyStates on DIRTY_VERTEX_DESC(0x400, skipped for compute); layout from hash map 0x141E07140/60 keyed on crc32_64(m_VertexDesc@0x1430281F0 & vs->m_VertexDescription@vs+0x48)
CreateInputLayout(elemDescs, n, vs->m_RawBytecode /*vs+0x68*/, vs->m_ShaderLength /*vs+0x10*/, &out) on device @0x143025F08 <- 0x140D70F90 on layout-map miss; element formats/offsets decoded from 64-bit desc (see report table)
IASetPrimitiveTopology(m_Topology@0x143028208) <- SetDirtyStates on 0x800
OMSetRenderTargets(n, rtvs, dsv) <- SetDirtyStates on 0x1; rtvs = RendererData->pRenderTargets[m_RenderTargets[i]].RTV (+0xA58+0x30*i via RendererData* @0x143025F00), dsv = pDepthStencils[m_DepthStencil].(bReadOnlyDepth ? ReadOnlyViews : Views)[m_DepthStencilSlice]
ClearRenderTargetView(rtv, RendererData->ClearColor@+0x2768) / ClearDepthStencilView(dsv, flags from SRTM mode, ...) <- SetDirtyStates SRTM clear modes
OMSetDepthStencilState(g_DepthStates[40*m_DepthStencilDepthMode + m_DepthStencilStencilMode] @globals+0x0, m_StencilRef@0x143027F44) <- SetDirtyStates on 0xC
RSSetState(g_RasterStates[72*fill + 24*cull + 2*depthBias + scissor] @globals+0x780) <- SetDirtyStates on 0x1070
RSSetViewports(1, &m_ViewPort@state+0x70) <- SetDirtyStates on 0x2 (MaxDepth adjusted by depth-bias table 0x143026180[mode] when 0x40)
OMSetBlendState(g_BlendStates[52*mode + 26*a2c + 2*writeMode + extra@0x143028480] @globals+0xC00, blendFactor@0x141E07168, 0xFFFFFFFF) <- SetDirtyStates on 0x80
PSSetShaderResources(i, 1, &m_PSTexture[i]@0x143027FF0) <- SetDirtyStates loop over m_PSResourceModifiedBits@0x143027EB4
PSSetSamplers(i, 1, &g_SamplerStates[5*m_PSTextureAddressMode[i] + m_PSTextureFilterMode[i]] @globals+0x1760) <- loop over m_PSSamplerModifiedBits@0x143027EB8
CSSetShaderResources / CSSetSamplers / CSSetUnorderedAccessViews <- CS modified-bit loops, arrays at state+0x240/+0x1C0,+0x200/+0x300
ID3D11PixelShader::Release() on ps->m_Shader (then nulled) <- 0x140D6FCF0 called from utility SetupGeometry when (raw & 0x1200)==0x1200 (RenderNormal|RenderNormalClear)

## DIVERGENCES FROM NUKEM
Vanilla 1.5.97 BSShader::BeginTechnique (0x14131FBD0) has NO hull/domain shader lookup or HSSetShader/DSSetShader calls - Nukem's HullShaders/DomainShaders maps and Renderer::SetHullShader/SetDomainShader are his additions, not game code.
Renderer::SetVertexShader in vanilla dereferences Shader->m_Shader unconditionally (no null check); Nukem's reimplementation writes 'Shader ? Shader->m_Shader : nullptr'. Vanilla callers guarantee non-null VS.
Nukem's constant-group implementation (CustomConstantGroup, unified ring buffer, VSSetConstantBuffers1 with first-constant offsets, FlushConstantGroup writing 0xFEFEFEFE) is his patched renderer, NOT vanilla. Vanilla: per-shader-owned pool buffers mapped WRITE_DISCARD inline in each Setup* function, full-buffer binds via plain VS/PSSetConstantBuffers, no Flush/Apply helper functions exist in the binary flow.
Nukem's Renderer::GetShaderConstantGroup(VertexShader*,level) reads the buffer size via GetDesc and caches it into m_Buffer - pure Nukem patch behavior. The vanilla ring allocator 0x140D6FFD0 ignores the size argument entirely and only special-cases level==7.
Vanilla dynamic-group allocator returns a pointer to a single STATIC slot (0x14302AC58) holding the chosen buffer; the result must be consumed (Unmap+bind) before the next call - implicit sequencing contract absent from Nukem's API.
HACK_Globals base layout differs from Nukem's 1.5.23 header ordering: in 1.5.97 the block at 0x1430261B0 STARTS at m_DepthStates[6][40] (raster +0x780, blend +0xC00, samplers +0x1760, m_NextConstantBufferIndex +0x1850, CB ring +0x1858, alphaTestRefCB +0x1878, perFrameCB +0x1CD8, slot7 CB +0x1CE0, context +0x1CF0); the early fields Nukem lists (clear color, device, window, dynamic VB pool) are not at this base - device is mirrored at 0x143025F08 and RendererData* at 0x143025F00. m_DepthBiasFactors (12 floats) sits immediately BEFORE the base at 0x143026180.
Input-layout resolution detail not in Nukem's RE: the lookup key is (shadowState.m_VertexDesc BITWISE-AND vertexShader->m_VertexDescription), hashed with a CRC32 over the 8 key bytes (0x140C06570, table 0x14175BF90), cached in a global 24-byte-entry hash map at 0x141E07140/0x141E07160; desc 0x300000000407 is special-cased to allow a permanently-NULL layout without insertion. Nukem's header only notes 'input layout may need to be created'.
Per-frame slot bindings: vanilla ResetState (0x140D70290) binds the alpha-test-ref CB (slot 11) for PS ONLY, and the per-frame CB (slot 12) for VS+PS; Nukem's comments claim slot 11 is 'PS/VS'. No VSSetConstantBuffers(11,...) was found on this path.
RendererShadowState is a plain global block at 0x143027EB0 in 1.5.97 (Nukem's 1.5.23 patch accesses it through his threaded-globals hack at different offsets, e.g. current technique/shader at GraphicsGlobals+0x3014/0x3018 vs absolute 0x143283BA4/0x143283BA8 here); field ORDER matches his RendererShadowState struct exactly.
RendererData field offsets (bReadOnlyDepth 0x22, pDevice 0x38, pContext 0x40, pRenderTargets 0xA48, pDepthStencils 0x1FA8, pCubemapRenderTargets 0x26C8, ClearColor 0x2768) are identical between 1.5.23 and 1.5.97 - confirmed live in SetDirtyStates/SetupTechnique/RestoreTechnique.
BSBatchRenderer::RenderPassImmediately/BeginPass logic including the 0x5C006076 re-setup exception and the write-only technique global (1.5.23 dword_141E32FDC == 1.5.97 dword_141E0DF8C) match Nukem line-for-line; 1.5.97 inlines EndPass into BeginPass's prologue.
BSShader::SetBoneMatrix matches Nukem's decompile exactly (TLS slot 0x2A00 skin-instance cache, 3*boneCount*16 bytes, slots 10 then 9); his helper sub_140D74600 is 0x140D74F70 in 1.5.97.

## OPEN QUESTIONS
FUN_140D6FCF0: utility SetupGeometry under (raw & 0x1200)==0x1200 (RenderNormal|RenderNormalClear, facegen normals path) Releases and NULLs the current PixelShader's m_Shader (ps+8). Intent unverified - after this the struct permanently loses its D3D object (subsequent binds of that PS technique set a null pixel shader). Replication must reproduce the observable effect only if those techniques are exercised; flagged as a likely one-shot 'clear' trick.
POSITION element format in generated input layouts is DXGI_FORMAT_R32G32B32A32_FLOAT (2) unconditionally (disasm-verified at 0x140D70FAD), implying float4 positions in the runtime vertex streams; not cross-checked against actual vertex buffer contents.
Two TEXCOORD semantic-index-2 element variants exist in the layout builder (bits 51/61: R8G8B8A8_UNORM pair with TEXCOORD3; bits 52/62: lone R32_FLOAT). Assumed mutually exclusive per desc; not proven.
POSITION InputSlot becomes 0xFFFFFFFF if neither presence bit 44 nor 54 is set - presumably never happens; unverified.
Special vertex desc 0x300000000407 (POSITION+TEXCOORD0, slot-0) may legitimately produce and cache a NULL input layout - which draw uses it and why is unknown.
Technique ID 0x5C006076 (BSSM_TILE+1) always forces SetupTechnique re-run in RenderPassImmediately; the technique's name/owner is unidentified (also unnamed in Nukem).
Depth-bias float table at 0x143026180 (12 floats, indexed flat by m_RasterStateDepthBiasMode, subtracted from viewport MaxDepth) - row semantics ([3][4] selection, who writes it and the floats at 0x143028470/74 compared against viewport MinDepth/MaxDepth) not traced; belongs to the raster-state cluster but affects shadow rendering.
FUN_1410A2370 (only other caller of Renderer::SetVertexShader/SetPixelShader): a self-contained compositor drawing TriShapes with VS/PS taken from its argument struct (+88/+96), sampler slot 0 forced to addr 0 / filter 2. Identity (likely facegen or LOD atlas blit) unconfirmed; bypasses BeginTechnique and the technique cache entirely.
No VSSetConstantBuffers(11) (alpha-test-ref for VS) found; if utility vertex shaders reference the AlphaTestRefCB the binding must come from another site or never occurs - unresolved.
The 4-buffer dynamic CB ring (globals+0x1858) has no visible fencing in the allocator; safety presumably relies on MAP_WRITE_DISCARD renaming. Per-frame wrap behavior unanalyzed.
GetShaderConstantGroup level argument only distinguishes 7 vs rest; whether any caller besides grass uses level 7 (slot 7 CB at globals+0x1CE0, Nukem's 16-byte m_TempConstantBuffer4 position) unverified.
SetupTechnique's depth-mode value 5 written to m_DepthStencilDepthMode for RenderDepth (0x2000) techniques does not map to a named value in Nukem's DepthStencilDepthMode enum (he documents 0,1,3,4,6) - actual D3D11_DEPTH_STENCIL_DESC for index 5 not extracted (state-object creation is another cluster).
dword_141E0DE30 (shadow-mask filter level driving PS technique bits 0x20000/0x40000/0x80000) and byte_141E0DE4C / byte_141E0DE43 ini/setting origins not traced.

## KEY ADDRESSES
0x141308440 BSBatchRenderer::RenderPassImmediately(Pass, Technique, AlphaTest, RenderFlags)
0x1413086C0 BSBatchRenderer::BeginPass (inlines EndPass: RestoreTechnique of previous shader, clears current shader/technique/material)
0x141308970 RenderPassImmediately_Standard (unskinned draw path, other cluster)
0x1413088C0 RenderPassImmediately_Skinned (calls SetBoneMatrix path)
0x141308B20 RenderPassImmediately_Custom (geom flag 8 path)
0x14131FBD0 BSShader::BeginTechnique(this, vsId, psId, ignorePixelShader) - single shared bind point for all BSShaders + imagespace shaders
0x14131FCE0 BSShader::EndTechnique (empty function)
0x140D6F9B0 BSGraphics::Renderer::SetVertexShader (VSSetShader + DIRTY_VERTEX_DESC + m_CurrentVertexShader)
0x140D6FD60 BSGraphics::Renderer::SetPixelShader (PSSetShader + m_CurrentPixelShader, no dirty flag)
0x140D6FCF0 Renderer::ReleasePixelShaderObject-like (Release + null *(arg+8))
0x140D6FFD0 Renderer::GetShaderConstantGroup(sizeIgnored, out pData, level) - 4-buffer WRITE_DISCARD ring / slot-7 CB; returns static slot 0x14302AC58
0x140D705B0 BSGraphics::Renderer::SetDirtyStates(bool isComputeShader)
0x140D70F90 Renderer::CreateInputLayoutFromVertexDesc(uint64 desc) - builds D3D11_INPUT_ELEMENT_DESCs, CreateInputLayout with current VS bytecode
0x140C06570 crc32_64(key) hash used for input-layout map (CRC table 0x14175BF90)
0x140D730E0 input-layout hash-map insert; 0x140D73F70 map grow
0x140D70290 Renderer::ResetState/SetPerFrameBuffers (re-init dirty flags; binds CB slot 11 PS-only, slot 12 VS+PS)
0x140D73BD0 RendererShadowState dirty-bit re-init
0x140D70100 Renderer::SetScissor-like used by utility SetupGeometry (projected bounding box)
0x14130DF90 BSUtilityShader::SetupTechnique (vtbl[2])
0x14130DD80 BSUtilityShader::RestoreTechnique (vtbl[3])
0x14130E890 BSUtilityShader::SetupMaterial (vtbl[4])
0x14130EC60 BSUtilityShader::RestoreMaterial (vtbl[5])
0x14130EC70 BSUtilityShader::SetupGeometry (vtbl[6])
0x141310300 BSUtilityShader::RestoreGeometry (vtbl[7])
0x141334900 BSUtilityShader technique->VS-ID converter (input = technique - 0x2B)
0x141334970 BSUtilityShader technique->PS-ID converter
0x14131F630 BSShader::SetBoneMatrix (secondary vtable; slots 10=bones, 9=prev bones)
0x140D74F70 skin-instance bone-matrix prep helper (Nukem's sub_140D74600)
0x14130FBE0 BSRenderPass::SetupShadowLightParameters (called from utility SetupGeometry)
0x141307160 BSRenderPass light-data setup (uses CB ring)
0x1412966A0 BSShaderAccumulator::GetCurrentAccumulator
0x1410A2370 standalone compositor - only non-BeginTechnique caller of Set(Vertex|Pixel)Shader
0x1418685B0 BSUtilityShader vtable
0x1430261B0 HACK_Globals base: +0x000 m_DepthStates[6][40]; +0x780 m_RasterStates[2][3][12][2]; +0xC00 m_BlendStates[7][2][13][2]; +0x1760 m_SamplerStates[6][5]; +0x1850 m_NextConstantBufferIndex; +0x1858 m_ConstantBuffers1[4] ring; +0x1878 m_AlphaTestRefCB; +0x1CD8 m_PerFrameCB(720B, slot 12); +0x1CE0 slot-7 CB; +0x1CF0 ID3D11DeviceContext*
0x143026180 m_DepthBiasFactors float[12] (before globals base)
0x143025F00 RendererData* mirror (-> 0x1430284A0); 0x143025F08 ID3D11Device*
0x143027EA0 ID3D11DeviceContext* (immediate context global)
0x143027EB0 RendererShadowState base (full field map in report §6; m_VertexDesc 0x1430281F0, m_CurrentVertexShader 0x1430281F8, m_CurrentPixelShader 0x143028200, m_Topology 0x143028208, m_PosAdjust 0x14302820C)
0x143028490 BSGraphics::Renderer singleton (Data @ +0x10 = 0x1430284A0; bReadOnlyDepth 0x1430284C2; pRenderTargets 0x143028EE8; pDepthStencils 0x14302A448, elem0 DepthSRV 0x14302A4D0)
0x14302AC58 static return slot of GetShaderConstantGroup
0x143283BA4 g_CurrentTechnique; 0x143283BA8 g_CurrentShader (BSShader*); 0x143490BB0 g_CurrentMaterial; 0x141E0DF8C write-only last-submitted technique
0x141E07140 input-layout map struct (capacity 0x141E07144, end sentinel 0x141E07150, entries 0x141E07160; entry = {u64 key, ID3D11InputLayout*, next})
0x141E07168 static blend factor passed to OMSetBlendState
0x143497408 TLS index global; TLS+0x2A00 current NiSkinInstance cache
0x141E0DED0 shadowSceneNode; sunShadowDirLight cascade split array at light+0x598 (stride 16, start/end floats)
BSShader object layout: +0x20 m_Type, +0x28 m_VertexShaderTable, +0x58 m_PixelShaderTable (scatter tables: mask +0xC, sentinel +0x18, entries +0x28, 16B entries {value,next}, key = value->m_TechniqueID)
BSUtilityShader: +0x90 currentRawTechnique, +0x94 raw & 0x7F
VertexShader struct: 0x00 id, 0x08 m_Shader, 0x10 len, 0x18/0x28/0x38 Buffer{buf,data} per-technique/material/geometry, 0x48 m_VertexDescription, 0x50 m_ConstantOffsets[20], 0x68 bytecode
PixelShader struct: 0x00 id, 0x08 m_Shader, 0x10/0x20/0x30 Buffer groups, 0x40 m_ConstantOffsets[64]