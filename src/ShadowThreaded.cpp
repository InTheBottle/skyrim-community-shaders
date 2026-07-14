#include "ShadowThreaded.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include <spdlog/spdlog.h>

#include "Globals.h"
#include "State.h"
#include "UtilityPassReplica.h"
#include "Utils/D3D.h"
#include "Vanilla/DrawState.h"
#include "Vanilla/ShaderReflect.h"

#include <RE/B/BSRenderPass.h>

// Multithreaded shadow rendering, built on the byte-exact UtilityPassReplica. Two levels of the
// shadow-map walk are bracketed:
//   RenderShadowmaps 0x1412E3480 : once-per-frame driver.
//   RenderShadowmap  0x141305610 : once-per-map body (AddrLib 100820); the planning interceptor
//         snapshots each map (params + clean render-state block) and brackets its passes.
//
// KEY RE FINDING that makes this tractable: shadow depth passes (technique & 0x1E00000) read ONLY
// per-frame globals (main camera/frustum 0x1431D0F88/E68, shadowSceneNode), the pass's own
// light/geometry, and the 0x5D8 render-state block. They do NOT read the current accumulator, the
// render-mode 0x1431D0E28, or any per-map global (those are all in the material/effect path). So a
// worker replaying a map needs only: the map's block (RT/viewport/depth target) + its own context.
// A claimed pass skips its inline setup, so the block captured right after a map's original is the
// clean per-map render-target state -- exactly the seed a replay needs.
//
// STAGES (CS_SHADOW_MT): 1=capture/observe (M1/M2), 2=serial deferred replay (Phase 0, this file's
// first threaded output path -- de-risks capture+replay before fan-out). See mt-shadow-plan.md.

namespace
{
	namespace engine
	{
		inline REL::Relocation<ID3D11DeviceContext**> g_immediateContext{ REL::Offset(0x3027EA0) };
		inline REL::Relocation<std::uint8_t*>         S_base{ REL::Offset(0x3027EB0) };
		constexpr std::uint32_t                       kBlockBytes = 0x5D8;
		inline REL::Relocation<std::uint32_t*>        g_currentTechnique{ REL::Offset(0x3283BA4) };
		inline REL::Relocation<void**>                g_currentShader{ REL::Offset(0x3283BA8) };
		inline REL::Relocation<void**>                g_currentMaterial{ REL::Offset(0x3490BB0) };
		inline REL::Relocation<std::uint32_t*>        g_boneCBRingCursor{ REL::Offset(0x3027A00) };
		inline REL::Relocation<std::uint64_t*>        g_dynVBRingState{ REL::Offset(0x3025F30) };
		inline REL::Relocation<std::uint32_t*>        g_shadowGeomToken{ REL::Offset(0x1E10660) };
		inline REL::Relocation<ID3D11Buffer**>        g_perFrameCameraCB{ REL::Offset(0x3027E88) };  // VS/PS b12
		inline REL::Relocation<ID3D11Buffer**>        g_psB11CB{ REL::Offset(0x3027A28) };            // PS b11
		// RT pool base: the depth SRV for render-target index i is at +152*i+0x2040 (validated by
		// the byte-exact FlushSetupTechniqueReplica). The shadow atlas is target 4 (all maps).
		inline REL::Relocation<std::uint8_t*>         g_renderer{ REL::Offset(0x3028490) };
		inline REL::Relocation<std::uint8_t**>        g_rtPoolPtr{ REL::Offset(0x3025F00) };  // rtBase = *this; the DSV pool
	}

	// LEVEL-2 output validator: FNV-hash the shadow depth atlas (target 4). Comparing this hash
	// across runs on a FROZEN scene -- vanilla (mode 1, engine renders inline) vs serial replay
	// (mode 2) -- proves the replayed shadow depth is bit-identical to vanilla. The CopyResource+Map
	// is a GPU sync point (validate-only, throttled to once per log window). Returns 0 on failure.
	// Validation-only, opt-in via CS_SHADOW_HASH (default off). The depth atlas is a Texture2DArray
	// whose staging Map under DXVK does not guarantee RowPitch >= Width*4 for the packed depth layout,
	// so the per-row read is clamped to RowPitch to stay in-bounds. Cross-run comparison is only valid
	// on a frozen scene (sgtm 0 + no animating casters); on a live scene the sun/skinned casters move
	// the atlas every frame, so this is a within-run liveness probe, not a cross-run equality gate.
	bool ShadowHashEnabled()
	{
		static const bool on = [] {
			char buf[8] = {};
			return GetEnvironmentVariableA("CS_SHADOW_HASH", buf, sizeof(buf)) && buf[0] && buf[0] != '0';
		}();
		return on;
	}

	std::uint64_t HashShadowAtlas(int a_target)
	{
		if (!ShadowHashEnabled())
			return 0;
		auto* rtPool = reinterpret_cast<std::uint8_t*>(engine::g_renderer.address());
		auto* srv = *reinterpret_cast<ID3D11ShaderResourceView**>(rtPool + 152 * a_target + 0x2040);
		if (!srv || !globals::d3d::device)
			return 0;
		winrt::com_ptr<ID3D11Resource> res;
		srv->GetResource(res.put());
		winrt::com_ptr<ID3D11Texture2D> tex;
		if (!res || FAILED(res->QueryInterface(IID_PPV_ARGS(tex.put()))))
			return 0;
		D3D11_TEXTURE2D_DESC desc{};
		tex->GetDesc(&desc);

		static winrt::com_ptr<ID3D11Texture2D> s_staging;
		static D3D11_TEXTURE2D_DESC            s_desc{};
		if (!s_staging || std::memcmp(&s_desc, &desc, sizeof(desc)) != 0) {
			D3D11_TEXTURE2D_DESC sd = desc;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			sd.MiscFlags = 0;
			s_staging = nullptr;
			if (FAILED(globals::d3d::device->CreateTexture2D(&sd, nullptr, s_staging.put())))
				return 0;
			s_desc = desc;
		}

		auto* ctx = *engine::g_immediateContext;
		ctx->CopyResource(s_staging.get(), tex.get());

		std::uint64_t h = 0xcbf29ce484222325ull;
		for (UINT slice = 0; slice < desc.ArraySize; ++slice) {
			D3D11_MAPPED_SUBRESOURCE mapped{};
			const UINT               sub = D3D11CalcSubresource(0, slice, desc.MipLevels);
			if (!mapped.pData && FAILED(ctx->Map(s_staging.get(), sub, D3D11_MAP_READ, 0, &mapped)))
				continue;
			if (!mapped.pData || !mapped.RowPitch)
				continue;
			// Read at most RowPitch bytes/row: the mapped region is RowPitch*Height, and the packed
			// depth pitch can be < Width*4, so Width*4 would run off the row (observed AV).
			const UINT  bytesPerRow = std::min<UINT>(desc.Width * 4u, mapped.RowPitch);
			const auto* base = static_cast<const std::uint8_t*>(mapped.pData);
			for (UINT row = 0; row < desc.Height; ++row) {
				const auto* r = base + static_cast<std::size_t>(row) * mapped.RowPitch;
				for (UINT b = 0; b < bytesPerRow; ++b) {
					h ^= r[b];
					h *= 0x100000001b3ull;
				}
			}
			ctx->Unmap(s_staging.get(), sub);
		}
		return h;
	}

	// Debug (CS_SHADOW_DUMP): write each shadow-atlas slice's raw depth to F:\claudetmp\rtprof\atlas\<tag>_s<N>.raw
	// (header: u32 width,height,DXGI_FORMAT,rowPitch; then rowPitch bytes per row). A/B mode 0 (tag "vanilla")
	// vs mode 9 (tag "instanced") on a FROZEN scene to see WHICH light's slice differs and by how much.
	void DumpShadowAtlas(int a_target, const char* a_tag)
	{
		static const bool on = [] { char b[8] = {}; return GetEnvironmentVariableA("CS_SHADOW_DUMP", b, sizeof(b)) && b[0] && b[0] != '0'; }();
		if (!on || !globals::d3d::device)
			return;
		auto* rtPool = reinterpret_cast<std::uint8_t*>(engine::g_renderer.address());
		auto* srv = *reinterpret_cast<ID3D11ShaderResourceView**>(rtPool + 152 * a_target + 0x2040);
		if (!srv)
			return;
		winrt::com_ptr<ID3D11Resource> res;
		srv->GetResource(res.put());
		winrt::com_ptr<ID3D11Texture2D> tex;
		if (!res || FAILED(res->QueryInterface(IID_PPV_ARGS(tex.put()))))
			return;
		D3D11_TEXTURE2D_DESC desc{};
		tex->GetDesc(&desc);
		D3D11_TEXTURE2D_DESC sd = desc;
		sd.Usage = D3D11_USAGE_STAGING;
		sd.BindFlags = 0;
		sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sd.MiscFlags = 0;
		winrt::com_ptr<ID3D11Texture2D> staging;
		if (FAILED(globals::d3d::device->CreateTexture2D(&sd, nullptr, staging.put())))
			return;
		auto* ctx = *engine::g_immediateContext;
		ctx->CopyResource(staging.get(), tex.get());
		for (UINT slice = 0; slice < desc.ArraySize; ++slice) {
			D3D11_MAPPED_SUBRESOURCE mapped{};
			const UINT sub = D3D11CalcSubresource(0, slice, desc.MipLevels);
			if (FAILED(ctx->Map(staging.get(), sub, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
				continue;
			}
			char path[260];
			snprintf(path, sizeof(path), "F:\\claudetmp\\rtprof\\atlas\\%s_s%u.raw", a_tag, slice);
			std::ofstream f(path, std::ios::binary);
			if (f) {
				const std::uint32_t hdr[4] = { desc.Width, desc.Height, static_cast<std::uint32_t>(desc.Format), mapped.RowPitch };
				f.write(reinterpret_cast<const char*>(hdr), sizeof(hdr));
				for (UINT row = 0; row < desc.Height; ++row)
					f.write(reinterpret_cast<const char*>(static_cast<std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch), mapped.RowPitch);
			}
			ctx->Unmap(staging.get(), sub);
		}
	}

	// Re-establish the frame-persistent CBs a fresh deferred context lacks (per-frame camera CB in
	// VS/PS b12, PS b11, the CS shared CBs). Standalone read of the same engine/CS globals the
	// engine binds from -- no live-context read, so it is multithread-ready. Mirrors
	// ShadowDeferred::BindPersistentStandalone.
	void BindPersistentStandalone(ID3D11DeviceContext* a_ctx)
	{
		ID3D11Buffer* const perFrame = *engine::g_perFrameCameraCB;
		ID3D11Buffer* const psB11 = *engine::g_psB11CB;
		auto* const         state = globals::state;
		ID3D11Buffer* const permutation = (state && state->permutationCB) ? state->permutationCB->CB() : nullptr;
		ID3D11Buffer* const shared = (state && state->sharedDataCB) ? state->sharedDataCB->CB() : nullptr;
		ID3D11Buffer* const feature = (state && state->featureDataCB) ? state->featureDataCB->CB() : nullptr;

		a_ctx->VSSetConstantBuffers(12, 1, &perFrame);
		ID3D11Buffer* psSharedRange[3] = { permutation, shared, feature };
		a_ctx->PSSetConstantBuffers(4, 3, psSharedRange);
		a_ctx->PSSetConstantBuffers(11, 1, &psB11);
		a_ctx->PSSetConstantBuffers(12, 1, &perFrame);
		ID3D11Buffer* csSharedRange[2] = { shared, feature };
		a_ctx->CSSetConstantBuffers(5, 2, csSharedRange);
	}

	// One covered shadow pass, captured for replay. The BSRenderPass and its geometry/light/property
	// persist for the frame (EngineFixes RenderPassCache), so a same-frame replay is safe.
	struct CapturedPass
	{
		RE::BSRenderPass* pass;
		std::uint32_t     technique;
		bool              alphaTest;
		std::uint32_t     renderFlags;
	};

	// One shadow map's replay unit: the clean per-map render-state block (RT/viewport/depth, seeds
	// the replay) + the ordered covered-pass list. Non-replicable passes render inline on the engine
	// path during the walk (the serial remainder), so they are counted, not stored.
	struct MapWork
	{
		std::uint8_t              block[engine::kBlockBytes];
		std::vector<CapturedPass> passes;
		std::uint64_t             unsupported = 0;
		std::uint64_t             skinnedInline = 0;  // covered-but-skinned; render on the engine path
	};

	std::vector<MapWork> g_mapWorkList;
	MapWork*             g_curMap = nullptr;
	bool                 g_claiming = false;         // serial/threaded: claim covered passes for replay
	bool                 g_concurrentRestrict = false;  // kConcurrent: claim ONLY the thread-safe subset
	bool                 g_instanceRestrict = false;    // kInstance: claim ONLY the instanceable subset

	// UtilityPassReplica::ShadowCaptureHook. Stores each covered pass on the current map and (when
	// claiming) takes ownership so the inline render is skipped -- the replay renders it later.
	std::atomic<bool> g_drawStateVerify{ false };  // kDrawStateVerify: per-pass engine-vs-worker draw-state diff

	bool CaptureHook(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest,
		std::uint32_t a_renderFlags, bool a_canReplicate)
	{
		// Regime-B MT gate: for each covered pass, run the engine-vs-worker draw-state compare (the
		// worker path is the exact MT code). Uncovered passes render inline on the engine path. The verify
		// does the engine render itself, so claim (return true) to skip the normal inline render.
		if (g_drawStateVerify.load(std::memory_order_relaxed)) {
			if (!a_canReplicate)
				return false;
			// Restrict the verify to the MT COVERED SET: whole-TRISHAPE (geom type 3), NON-skinned static
			// geometry -- exactly what the threaded path claims. Skinned / BSDynamicTriShape use per-context
			// dynamic-VB ring slices whose buffer IDENTITY legitimately differs (byte-identical contents),
			// which the identity-based vb fingerprint can't see through; SUB_INDEX (type 8) draws its
			// segments via the global-wholeDraw path. Those stay on the engine serial remainder, so
			// verifying them here only produces false vb/startIndex divergences. Render them inline.
			auto* const geom = a_pass->geometry;
			const bool  wholeTri = geom && *(reinterpret_cast<const std::uint8_t*>(geom) + 0x150) == 3;
			const bool  skinned = geom && *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(geom) + 0x130);
			if (!wholeTri || skinned)
				return false;
			UtilityPassReplica::GetSingleton()->VerifyPassDrawStateThreaded(a_pass, a_technique, a_alphaTest, a_renderFlags);
			return true;
		}
		if (!g_curMap)
			return false;
		if (!a_canReplicate) {
			++g_curMap->unsupported;
			return false;  // uncovered -> engine renders inline (serial remainder)
		}
		// Skinned casters (geom+0x130 != 0) drive the bone-CB + dynamic-VB rings, which map with
		// WRITE_NO_OVERWRITE + a GPU query -- illegal on a deferred context (null base -> AV). Until
		// those rings are made per-worker (skinned-parallel phase), leave skinned passes on the
		// engine's inline path; depth-only shadow output is order-independent so mixing is safe.
		auto* geom = a_pass->geometry;
		if (geom && *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(geom) + 0x130)) {
			++g_curMap->skinnedInline;
			return false;
		}
		// kConcurrent phase 1 admits only the thread-safe subset: pure directional cascades ((tech &
		// 0x200000); non-directional spot/point/stencil call the scissor helpers that mutate the shared
		// renderer scissor global) that are WHOLE TRISHAPE (geom type 3; SUB_INDEX drives SubIndexPreDraw's
		// global wholeDraw flag). Everything else stays on the serial remainder (rendered inline).
		if (g_concurrentRestrict) {
			// Concurrent claim = the thread-safe subset: WHOLE TRISHAPE (geom type 3) only. Both
			// directional cascades (scissor-free) and spot/point (their scissor is now bound on the
			// worker's own deferred context, not the shared global) are safe. SUB_INDEX (SubIndexPreDraw
			// mutates a global wholeDraw flag), skinned (bone/dyn-VB rings), and stencil (CanReplicate
			// excludes it) stay on the serial remainder.
			const bool wholeTri = geom && *(reinterpret_cast<const std::uint8_t*>(geom) + 0x150) == 3;
			if (!wholeTri) {
				++g_curMap->unsupported;
				return false;  // serial remainder
			}
		}
		// kInstance admits the instanceable subset: WHOLE TRISHAPE (geom type 3), NON-alpha-test. The
		// instanced path fills World from the per-instance stream and does NOT run the alpha-test /
		// alpha-blend geometry setup, so alpha-tested casters (foliage) and SUB_INDEX stay inline; a
		// per-object depth for them is cheap relative to the dense repeated static meshes we instance.
		if (g_instanceRestrict) {
			const bool wholeTri = geom && *(reinterpret_cast<const std::uint8_t*>(geom) + 0x150) == 3;
			if (!wholeTri || a_alphaTest) {
				++g_curMap->unsupported;
				return false;  // serial remainder (alpha-test / sub-index render inline)
			}
		}
		// Capture the map's clean render-state block at the FIRST covered pass -- while the engine's
		// RT setup is still live (DSV bound to this map's atlas slice, viewport set). Capturing AFTER
		// RenderShadowmap returns (as before) grabbed a block whose DSV the engine had already unbound,
		// so the worker replay bound no depth target and its draws never reached the atlas.
		if (g_curMap->passes.empty())
			std::memcpy(g_curMap->block, reinterpret_cast<void*>(engine::S_base.address()), engine::kBlockBytes);
		g_curMap->passes.push_back(CapturedPass{ a_pass, a_technique, a_alphaTest, a_renderFlags });
		return g_claiming;  // claim -> skip inline; replay owns it
	}

	std::atomic<std::uint64_t> g_deferredDraws{ 0 };
	using DrawIndexed_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
	DrawIndexed_t g_origDeferredDrawIndexed = nullptr;
	void STDMETHODCALLTYPE DrawIndexedCounter(ID3D11DeviceContext* a_ctx, UINT a_n, UINT a_start, INT a_base)
	{
		g_deferredDraws.fetch_add(1, std::memory_order_relaxed);
		g_origDeferredDrawIndexed(a_ctx, a_n, a_start, a_base);
	}

	// =================================================================================================
	// Post-RenderShadowmaps STATE VALIDATION (leak detector). The class of bug this catches: our shadow
	// path leaves GPU state (bindings, IL residue, stale caches) that the engine's CPU-side state model
	// does not know about, silently corrupting MAIN-SCENE rendering later in the frame (e.g. the
	// 2026-07-14 pillar-texture corruption). Two layers, both run at RenderShadowmapsDetour EXIT:
	//
	//  1. HARD CANARIES (mode-independent invariants):
	//     - the bound VS object must equal the engine model's (S+0x348)->shader;
	//     - S+0x340 renderStateVertexDesc must not carry the VA_INSTANCEDATA slot-1 bit (kInstBits residue);
	//     - IA vertex-buffer slot 1 must be exactly what it was at entry (the instance stream must not leak);
	//     - the bound PS must equal the engine model's (S+0x350)->shader.
	//
	//  2. LEARNED BASELINE: in mode 0 (vanilla), record the exit fingerprint (IL, topology, IB, VB0/1,
	//     VS/PS, RTV0/DSV, blend/DS/raster objects, viewport0, scissor0, VS/PS b12 ptr) for N frames and
	//     learn which fields are stable across frames. In any replica mode, compare those stable fields
	//     against the learned values -- a mismatch means our path exits with different state than vanilla
	//     would, i.e. a leak the engine may or may not tolerate. Fields vanilla itself varies are auto-
	//     excluded, so this is false-positive-free by construction.
	//
	// Gated by CS_SHADOW_STATE_VALIDATE (env) or devbench shadowmt action=stateval; ~30 Get* calls/frame
	// when armed. First divergence per field logs once (warn); counters readable via devbench.
	struct StateSnap
	{
		// Keep POD so memcmp-per-field works; refs released immediately after Get (ptr identity only).
		void*                    il;
		std::uint32_t            topo;
		void*                    ib;
		std::uint32_t            ibFmt;
		void*                    vb0;
		void*                    vb1;
		std::uint32_t            vb0Stride, vb1Stride;
		void*                    vs;
		void*                    ps;
		void*                    rtv0;
		void*                    dsv;
		void*                    blend;
		void*                    dss;
		std::uint32_t            stencilRef;
		void*                    raster;
		void*                    vsCb12;
		void*                    psCb12;
		D3D11_VIEWPORT           vp0;
		D3D11_RECT               sc0;
		std::uint32_t            nVp, nSc;

		static StateSnap Capture(ID3D11DeviceContext* ctx)
		{
			StateSnap s{};
			ID3D11InputLayout* il = nullptr;
			ctx->IAGetInputLayout(&il);
			s.il = il;
			if (il)
				il->Release();
			D3D11_PRIMITIVE_TOPOLOGY topo{};
			ctx->IAGetPrimitiveTopology(&topo);
			s.topo = static_cast<std::uint32_t>(topo);
			ID3D11Buffer* ib = nullptr;
			DXGI_FORMAT   fmt{};
			UINT          off{};
			ctx->IAGetIndexBuffer(&ib, &fmt, &off);
			s.ib = ib;
			s.ibFmt = static_cast<std::uint32_t>(fmt);
			if (ib)
				ib->Release();
			ID3D11Buffer* vbs[2] = {};
			UINT          strides[2] = {}, offs[2] = {};
			ctx->IAGetVertexBuffers(0, 2, vbs, strides, offs);
			s.vb0 = vbs[0];
			s.vb1 = vbs[1];
			s.vb0Stride = strides[0];
			s.vb1Stride = strides[1];
			for (auto* v : vbs)
				if (v)
					v->Release();
			ID3D11VertexShader* vs = nullptr;
			ctx->VSGetShader(&vs, nullptr, nullptr);
			s.vs = vs;
			if (vs)
				vs->Release();
			ID3D11PixelShader* ps = nullptr;
			ctx->PSGetShader(&ps, nullptr, nullptr);
			s.ps = ps;
			if (ps)
				ps->Release();
			ID3D11RenderTargetView* rtv = nullptr;
			ID3D11DepthStencilView* dsv = nullptr;
			ctx->OMGetRenderTargets(1, &rtv, &dsv);
			s.rtv0 = rtv;
			s.dsv = dsv;
			if (rtv)
				rtv->Release();
			if (dsv)
				dsv->Release();
			ID3D11BlendState* bs = nullptr;
			float             bf[4]{};
			UINT              sm{};
			ctx->OMGetBlendState(&bs, bf, &sm);
			s.blend = bs;
			if (bs)
				bs->Release();
			ID3D11DepthStencilState* dss = nullptr;
			UINT                     sref{};
			ctx->OMGetDepthStencilState(&dss, &sref);
			s.dss = dss;
			s.stencilRef = sref;
			if (dss)
				dss->Release();
			ID3D11RasterizerState* rs = nullptr;
			ctx->RSGetState(&rs);
			s.raster = rs;
			if (rs)
				rs->Release();
			ID3D11Buffer* cb = nullptr;
			ctx->VSGetConstantBuffers(12, 1, &cb);
			s.vsCb12 = cb;
			if (cb)
				cb->Release();
			cb = nullptr;
			ctx->PSGetConstantBuffers(12, 1, &cb);
			s.psCb12 = cb;
			if (cb)
				cb->Release();
			s.nVp = 1;
			ctx->RSGetViewports(&s.nVp, &s.vp0);
			s.nSc = 1;
			ctx->RSGetScissorRects(&s.nSc, &s.sc0);
			return s;
		}
	};

	struct StateValidator
	{
		// Learned baseline (mode-0 exit): value + stability per field index.
		static constexpr int kFields = 20;
		std::uint64_t        base[kFields]{};
		bool                 unstable[kFields]{};
		std::uint32_t        baselineFrames = 0;
		std::uint32_t        divergences = 0;
		std::uint32_t        canaryHits = 0;
		std::uint32_t        checkedFrames = 0;
		std::uint32_t        loggedMask = 0;  // one log per field per session

		static const char* FieldName(int i)
		{
			static const char* names[kFields] = {
				"inputLayout", "topology", "indexBuffer", "ibFormat", "vb0", "vb1", "vb0Stride", "vb1Stride",
				"vs", "ps", "rtv0", "dsv", "blend", "depthStencil", "stencilRef", "raster",
				"vsCb12", "psCb12", "viewport0", "scissor0"
			};
			return names[i];
		}

		static void Fields(const StateSnap& s, std::uint64_t out[kFields])
		{
			out[0] = reinterpret_cast<std::uint64_t>(s.il);
			out[1] = s.topo;
			out[2] = reinterpret_cast<std::uint64_t>(s.ib);
			out[3] = s.ibFmt;
			out[4] = reinterpret_cast<std::uint64_t>(s.vb0);
			out[5] = reinterpret_cast<std::uint64_t>(s.vb1);
			out[6] = s.vb0Stride;
			out[7] = s.vb1Stride;
			out[8] = reinterpret_cast<std::uint64_t>(s.vs);
			out[9] = reinterpret_cast<std::uint64_t>(s.ps);
			out[10] = reinterpret_cast<std::uint64_t>(s.rtv0);
			out[11] = reinterpret_cast<std::uint64_t>(s.dsv);
			out[12] = reinterpret_cast<std::uint64_t>(s.blend);
			out[13] = reinterpret_cast<std::uint64_t>(s.dss);
			out[14] = s.stencilRef;
			out[15] = reinterpret_cast<std::uint64_t>(s.raster);
			out[16] = reinterpret_cast<std::uint64_t>(s.vsCb12);
			out[17] = reinterpret_cast<std::uint64_t>(s.psCb12);
			std::uint64_t vph = 0;
			std::memcpy(&vph, &s.vp0.TopLeftX, 8);  // x+y packed
			out[18] = vph ^ (static_cast<std::uint64_t>(*reinterpret_cast<const std::uint32_t*>(&s.vp0.Width)) << 1);
			std::uint64_t sch = 0;
			std::memcpy(&sch, &s.sc0, 8);
			out[19] = sch;
		}

		bool prevWasVanilla = true;  // mode of the PREVIOUS frame's shadow walk

		// Called at detour ENTRY (before any of this frame's shadow work). The state here is the product
		// of the PREVIOUS frame's full pipeline (its shadow walk + main scene + post) -- so if the previous
		// frame's replica path leaked anything the engine did not recover from, THIS snapshot diverges from
		// the vanilla-entry baseline. This is the semantically meaningful invariant: exit-state comparison
		// over-fires on benign transients (null PS after depth-only work) that the engine tolerates by
		// design through the cleared technique caches + force-dirty.
		void OnEntry(ID3D11DeviceContext* ctx, bool a_curIsVanilla)
		{
			const StateSnap snap = StateSnap::Capture(ctx);
			std::uint64_t   f[kFields];
			Fields(snap, f);

			// ---- entry canary: engine CPU model vs GPU (only when the cache CLAIMS validity) ----
			auto* S = reinterpret_cast<std::uint8_t*>(engine::S_base.address());
			if (*engine::g_currentShader != nullptr) {
				if (auto* vsObj = *reinterpret_cast<std::uint8_t**>(S + 0x348)) {
					void* modelVs = *reinterpret_cast<void**>(vsObj + 0x08);
					if (modelVs && snap.vs && modelVs != snap.vs && !prevWasVanilla) {
						++canaryHits;
						if (!(loggedMask & 1u)) {
							loggedMask |= 1u;
							logger::warn("[ShadowStateVal] CANARY: entry VS {:p} != engine model {:p} (previous frame's replica leaked a stale cache)", snap.vs, modelVs);
						}
					}
				}
			}

			// ---- learned-baseline entry compare, attributed to the PREVIOUS frame's mode ----
			if (prevWasVanilla) {
				if (baselineFrames == 0) {
					std::memcpy(base, f, sizeof(base));
				} else {
					for (int i = 0; i < kFields; ++i)
						if (base[i] != f[i])
							unstable[i] = true;  // vanilla itself varies this field -> excluded
				}
				++baselineFrames;
			} else if (baselineFrames >= 8) {
				++checkedFrames;
				for (int i = 0; i < kFields; ++i) {
					if (unstable[i] || base[i] == f[i])
						continue;
					++divergences;
					const std::uint32_t bit = 1u << (8 + i);
					if (!(loggedMask & bit)) {
						loggedMask |= bit;
						logger::warn("[ShadowStateVal] entry-state '{}' diverges from vanilla baseline ({:016X} -> {:016X}) -- previous frame's replica walk leaked",
							FieldName(i), base[i], f[i]);
					}
				}
			}
			prevWasVanilla = a_curIsVanilla;
		}
	};
	StateValidator g_stateValidator;

	bool StateValidationEnabled()
	{
		static const bool s_env = [] {
			char b[8] = {};
			return GetEnvironmentVariableA("CS_SHADOW_STATE_VALIDATE", b, sizeof(b)) && b[0] && b[0] != '0';
		}();
		return s_env || ShadowThreaded::GetSingleton()->stateValidationRequested.load(std::memory_order_relaxed);
	}

	// Stage 1a gate: armed by RenderShadowmapsDetour only for the duration of the engine's shadow-map
	// walk under kOwnBeginPass, so BeginPass calls OUTSIDE the shadow driver (main scene, deferred,
	// UI) are left on the engine path. RenderShadowmaps is synchronous on the render thread, so a
	// plain flag is race-free; atomic only for tidy publication.
	std::atomic<bool> g_ownBeginPass{ false };
	std::atomic<bool> g_ownBeginPassVerify{ false };  // mode 6: run the engine-vs-replica command compare
	std::uint64_t     g_ownBeginPassCalls = 0;        // BeginPassReplica invocations this frame (render thread only)

	// BSBatchRenderer::BeginPass (SE 1.5.97 0x141308030) detour. When the gate is armed, hand the
	// call to BeginPassReplica -- the 1:1 reimplementation that owns the per-group DX11 state + the
	// pass loop + cleanup. The engine returns void* (LOBYTE = sub_141307DD0's "more groups" bool);
	// BeginPassReplica returns that byte, which we widen back to the void* the caller tests.
	struct BeginPassHook
	{
		static void* thunk(void* a1, void* a2, void* a3, void* a4, std::uint32_t a5)
		{
			if (g_ownBeginPass.load(std::memory_order_relaxed)) {
				++g_ownBeginPassCalls;
				auto* const replica = UtilityPassReplica::GetSingleton();
				const std::uint8_t r = g_ownBeginPassVerify.load(std::memory_order_relaxed) ?
				                           replica->BeginPassCompare(a1, a2, a3, a4, a5,
					                           reinterpret_cast<UtilityPassReplica::BeginPassFn>(func.address())) :
				                           replica->BeginPassReplica(a1, a2, a3, a4, a5);
				return reinterpret_cast<void*>(static_cast<std::uintptr_t>(r));
			}
			return func(a1, a2, a3, a4, a5);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct RenderShadowmapsHook
	{
		static void thunk() { ShadowThreaded::GetSingleton()->RenderShadowmapsDetour(&func); }
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct RenderShadowmapHook
	{
		static std::int32_t thunk(void* a1, std::int64_t a2, void* a3, std::int32_t a4)
		{
			return ShadowThreaded::GetSingleton()->RenderShadowmapDetour(a1, a2, a3, a4, &func);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// --- cull-vs-submission split instrumentation (CS_SHADOW_TIMING) ---
	// Scoped true only around the vanilla RenderShadowmaps walk so CullHook attributes ONLY shadow culls.
	std::atomic<bool> g_inShadowTiming{ false };
	// In-session A/B intervention flag, flipped every 128-frame window so alternating [ShadowTiming] lines
	// compare intervention-ON vs OFF at the SAME view (no cross-launch variance). Two interventions:
	//  - CS_SHADOW_SKIP_CULL=1: CullHook returns early (removes cull CPU *and* shadow-GPU work -> only the
	//    "unchanged => not on critical path" direction is clean; a speedup is ambiguous CPU-vs-GPU).
	//  - CS_SHADOW_SPIN_US=N: inject N microseconds of pure render-thread busy-spin per frame (adds CPU, ZERO
	//    GPU/render effect) -> if the frame grows by ~N us the render thread IS the frame limiter; the clean test.
	std::atomic<bool> g_intervene{ false };
	// Render-thread CPU cycles / QPC ticks spent inside NiCamera::sub_1412C1600 (the shadow CULL:
	// SetCameraData + accumulator Func37/Func42) during the current shadow walk. Accumulated by CullHook,
	// snapshotted per frame by the timing branch. The remainder of RenderShadowmaps is DX11 setup + SUBMISSION.
	ULONG64       g_cullCyc = 0;
	std::uint64_t g_cullWall = 0;
	// Diagnostic: (camera, accumulator) per shadow cull this frame. If the accumulators are all DISTINCT,
	// the per-map culls can run in parallel into their own accumulators (Phase B); if shared, need private ones.
	std::vector<std::pair<void*, void*>> g_cullMapsDiag;

	// --- kParallelCull worker(s) ---
	// A tiny persistent worker: the render thread hands it a task and (Stage 1) blocks until done. Running
	// the ENGINE cull off the render thread is the key risk to validate (it uses per-thread ScrapHeap +
	// thread-safe MemoryManager; the walk is pure CPU). Stage 2 will add async dispatch + a barrier.
	struct CullWorker
	{
		std::thread             th;
		std::mutex              m;
		std::condition_variable cv;
		std::function<void()>   task;
		bool                    hasTask = false;
		bool                    done = false;
		bool                    stop = false;
		bool                    started = false;

		void Start()
		{
			if (started)
				return;
			started = true;
			th = std::thread([this] {
				for (;;) {
					std::function<void()> t;
					{
						std::unique_lock<std::mutex> lk(m);
						cv.wait(lk, [this] { return hasTask || stop; });
						if (stop)
							return;
						t = std::move(task);
						hasTask = false;
					}
					t();
					{
						std::lock_guard<std::mutex> lk(m);
						done = true;
					}
					cv.notify_all();
				}
			});
		}
		void RunAndWait(std::function<void()> t)
		{
			std::unique_lock<std::mutex> lk(m);
			task = std::move(t);
			hasTask = true;
			done = false;
			cv.notify_all();
			cv.wait(lk, [this] { return done; });
		}
	};
	CullWorker g_cullWorker;

	// ============================ kParallelCull Stage 2 ============================
	// Parallel per-map cull + serial submit. The per-map cull (sub_1412C1600) is DISPATCHED to a worker
	// pool (no join) into its own accumulator; the concrete submit (Func43 @0x1412CAC90) is DETOURED and
	// DEFERRED (its RT/viewport captured); after the RenderShadowmaps walk we BARRIER on the pool and replay
	// the deferred submits serially with each map's camera + RT re-bound. Maps whose accumulator is REUSED
	// this frame (2/9, detected via last frame's histogram) run fully inline (they'd clobber a shared accum).
	struct CullPool
	{
		std::vector<std::thread>          threads;
		std::deque<std::function<void()>> tasks;
		std::mutex                        m;
		std::condition_variable           cv, cvDone;
		int                               active = 0;
		bool                              stop = false;
		bool                              started = false;

		void Start(std::uint32_t n)
		{
			if (started)
				return;
			started = true;
			for (std::uint32_t i = 0; i < n; ++i)
				threads.emplace_back([this] { Run(); });
		}
		void Run()
		{
			for (;;) {
				std::function<void()> t;
				{
					std::unique_lock<std::mutex> lk(m);
					cv.wait(lk, [this] { return !tasks.empty() || stop; });
					if (stop && tasks.empty())
						return;
					t = std::move(tasks.front());
					tasks.pop_front();
				}
				t();
				{
					std::lock_guard<std::mutex> lk(m);
					if (--active == 0)
						cvDone.notify_all();
				}
			}
		}
		void Submit(std::function<void()> t)
		{
			{
				std::lock_guard<std::mutex> lk(m);
				++active;
				tasks.push_back(std::move(t));
			}
			cv.notify_one();
		}
		void Wait()
		{
			std::unique_lock<std::mutex> lk(m);
			cvDone.wait(lk, [this] { return active == 0; });
		}
	};
	CullPool g_cullPool;

	// One deferred map's submit: its accumulator + camera + submit-flags + the captured render targets/viewport.
	struct DeferredSubmit
	{
		void*                      accum = nullptr;
		void*                      camera = nullptr;
		std::uint32_t              flags = 0;
		std::uint32_t              cullFlags = 0;
		winrt::com_ptr<ID3D11DepthStencilView>   dsv;
		winrt::com_ptr<ID3D11RenderTargetView>   rtvs[8];
		UINT                       numRtv = 0;
		UINT                       numVp = 0;
		D3D11_VIEWPORT             vps[16]{};
	};

	std::atomic<bool>                     g_pcActive{ false };  // inside a kParallelCull RenderShadowmaps walk
	std::atomic<int>                      g_pcFrame{ 0 };       // frame counter for gated crash-pinpoint logging
	// Flushed log to pin the crashing op (the CS log survives a hard crash only if flushed).
	inline void pclog(const char* what, void* p)
	{
		if (g_pcFrame.load(std::memory_order_relaxed) > 4)
			return;
		logger::warn("[pc] f{} {} {}", g_pcFrame.load(std::memory_order_relaxed), what, p);
		if (auto l = spdlog::default_logger())
			l->flush();
	}
	std::vector<DeferredSubmit>           g_deferredSubmits;    // deferred submits collected this frame
	bool                                  g_curMapDeferred = false;  // did the current map's cull get deferred?
	void*                                 g_curMapCamera = nullptr;  // camera captured by CullHook for this map
	std::set<void*>                       g_accumSeenThisFrame;      // accumulators deferred this frame
	std::map<void*, int>                  g_accumCountThisFrame;     // accumulator usage count this frame
	std::set<void*>                       g_accumSharedLastFrame;    // accumulators used >1x last frame -> inline
	bool                                  g_haveHistogram = false;   // first kParallelCull frame runs all inline

	// Engine calls for the serial submit phase (SE 1.5.97 module offsets).
	using SetCameraData_t = void(__fastcall*)(void*, void*, std::uint32_t);
	using Func43_t = void(__fastcall*)(void*, std::int64_t);
	using Func37_t = void(__fastcall*)(void*, void*);
	using Func42_t = void(__fastcall*)(void*, std::uint32_t);
	using UpdateViewport_t = void(__fastcall*)(void*, int, int, int);

	// Replicates NiCamera::sub_1412C1600 MINUS the walk (Func42): sets the global camera + accumulator so the
	// per-map shadow-matrix setup that follows in RenderShadowmap reads correct data. Runs INLINE (serial).
	void CullSetup(void* camera, void* accum, std::uint32_t flags)
	{
		reinterpret_cast<SetCameraData_t>(REL::Offset(0xd7bab0).address())(
			reinterpret_cast<void*>(REL::Offset(0x302c890).address()), camera, flags);
		if (flags & 0x400)
			reinterpret_cast<UpdateViewport_t>(REL::Offset(0xd69d00).address())(
				reinterpret_cast<void*>(REL::Offset(0x3028490).address()), 0, 0, 1);
		*reinterpret_cast<void**>(REL::Offset(0x31d0e68).address()) = camera;                  // unk_1431D0E68
		reinterpret_cast<void(__fastcall*)(void*)>(REL::Offset(0x12966b0).address())(accum);   // SetCurrentAccumulator
		reinterpret_cast<Func37_t>(REL::Offset(0x12ca100).address())(accum, camera);           // Func37 (stub)
	}
	// The WALK (BSShaderAccumulator::Func42 0x1412CAC20): the 98% cost. Parallel-safe -- FinishAccumulating
	// uses per-process state, NOT the global camera (verified via xrefs to unk_14302C890). Runs on the pool.
	void CullWalk(void* accum, std::uint32_t flags)
	{
		pclog("walkStart", accum);
		reinterpret_cast<Func42_t>(REL::Offset(0x12cac20).address())(accum, flags);
		pclog("walkEnd", accum);
	}

	// NiCamera::sub_1412C1600 (0x1412C1600): the per-map cull driver called from NiCamera::Render, right
	// before Func43 does the draw submission. Timing it separates cull CPU from submission CPU.
	struct CullHook
	{
		static std::int32_t thunk(void* a1, void* a2, std::uint32_t a3)
		{
			auto* orig = reinterpret_cast<std::int32_t (*)(void*, void*, std::uint32_t)>(func.address());
			if (ShadowThreaded::GetSingleton()->GetMode() == ShadowThreaded::Mode::kParallelCull) {
				// Stage 1 fallback (CS_SHADOW_CULL_SERIAL=1): run on ONE worker + join (serial, validated).
				static const bool s_serial = [] {
					char b[8] = {};
					return GetEnvironmentVariableA("CS_SHADOW_CULL_SERIAL", b, sizeof(b)) && b[0] && b[0] != '0';
				}();
				if (s_serial || !g_pcActive.load(std::memory_order_relaxed)) {
					std::int32_t ret = 0;
					g_cullWorker.RunAndWait([&] { ret = orig(a1, a2, a3); });
					return ret;
				}
				// Stage 2: a1=camera, a2=accumulator, a3=flags.
				g_accumCountThisFrame[a2]++;
				g_curMapCamera = a1;
				const bool sharedHist = g_accumSharedLastFrame.count(a2) > 0;
				const bool seenThisFrame = g_accumSeenThisFrame.count(a2) > 0;
				if (!g_haveHistogram || sharedHist || seenThisFrame) {
					// Run inline: first frame (no histogram yet), a known-shared accumulator, or a histogram-miss
					// reuse. Drain the pool before a reuse so we never write a still-in-flight accumulator.
					g_curMapDeferred = false;
					if (seenThisFrame)
						g_cullPool.Wait();
					return orig(a1, a2, a3);
				}
				// Distinct accumulator: run the SETUP inline now (SetCameraData + globals, so the shadow-matrix
				// setup that follows in RenderShadowmap is correct), DEFER the WALK (Func42) to the parallel
				// phase after the light loop, and DEFER the submit (Func43Hook). a3 = cull flags.
				g_accumSeenThisFrame.insert(a2);
				g_curMapDeferred = true;
				DeferredSubmit d;
				d.accum = a2;
				d.camera = a1;
				d.cullFlags = a3;
				g_deferredSubmits.push_back(std::move(d));
				// SETUP inline now (sets unk_14302C890 for the shadow-matrix setup that follows on the render
				// thread). DISPATCH the WALK now too -- at THIS map's light-loop position (the cull must run at
				// its map's position; Stage 1 proved that), but on a worker so it runs concurrently with the
				// other maps' walks + the render thread. The walk does NOT touch unk_14302C890 (verified), so
				// it does not race the render thread's matrix setup. The submit is deferred (Func43Hook) to
				// after the end-of-loop barrier.
				CullSetup(a1, a2, a3);
				pclog("setupDone", a2);
				{
					void* acc = a2;
					std::uint32_t cf = a3;
					g_cullPool.Submit([acc, cf] { CullWalk(acc, cf); });
				}
				return 0;
			}
			if (!g_inShadowTiming.load(std::memory_order_relaxed))
				return orig(a1, a2, a3);
			// DIAGNOSTIC (CS_SHADOW_SKIP_CULL): skip the shadow cull when the A/B intervention is ON.
			static const bool s_skipMode = [] {
				char b[8] = {};
				return GetEnvironmentVariableA("CS_SHADOW_SKIP_CULL", b, sizeof(b)) && b[0] && b[0] != '0';
			}();
			if (s_skipMode && g_intervene.load(std::memory_order_relaxed))
				return 0;
			g_cullMapsDiag.emplace_back(a1, a2);  // (camera, accumulator) for the distinctness diagnostic
			ULONG64 c0 = 0, c1 = 0;
			QueryThreadCycleTime(GetCurrentThread(), &c0);
			LARGE_INTEGER t0, t1;
			QueryPerformanceCounter(&t0);
			const std::int32_t r = orig(a1, a2, a3);
			QueryPerformanceCounter(&t1);
			QueryThreadCycleTime(GetCurrentThread(), &c1);
			g_cullCyc += c1 - c0;
			g_cullWall += static_cast<std::uint64_t>(t1.QuadPart - t0.QuadPart);
			return r;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// BSShaderAccumulator::Func43 (0x1412CAC90): the per-map SUBMIT, called right after the cull in
	// NiCamera::Render. For a DEFERRED map (its cull is running on the pool), capture the RT + viewport the
	// engine bound for this map, record them for the serial submit phase, and SKIP the submit now.
	struct Func43Hook
	{
		static void thunk(void* a1, std::int64_t a2)  // a1 = accumulator, a2 = flags
		{
			auto* orig = reinterpret_cast<Func43_t>(func.address());
			if (ShadowThreaded::GetSingleton()->GetMode() != ShadowThreaded::Mode::kParallelCull ||
				!g_pcActive.load(std::memory_order_relaxed) || !g_curMapDeferred) {
				orig(a1, a2);  // main render, inline (shared-accum) map, or Stage-1 fallback: submit now
				return;
			}
			g_curMapDeferred = false;
			if (!g_deferredSubmits.empty()) {
				auto& d = g_deferredSubmits.back();  // the record CullHook pushed for this map
				d.flags = static_cast<std::uint32_t>(a2);
				auto* ctx = globals::d3d::context;
				ID3D11RenderTargetView* rtvs[8] = {};
				ID3D11DepthStencilView* dsv = nullptr;
				ctx->OMGetRenderTargets(8, rtvs, &dsv);
				d.dsv.attach(dsv);
				d.numRtv = 0;
				for (UINT i = 0; i < 8; ++i) {
					d.rtvs[i].attach(rtvs[i]);
					if (rtvs[i])
						d.numRtv = i + 1;
				}
				d.numVp = 16;
				ctx->RSGetViewports(&d.numVp, d.vps);
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// --- shadow instancing payoff diagnostic (CS_SHADOW_INSTANCE_DIAG) ---
	// During the timed vanilla shadow render (g_inShadowTiming), record each static DrawTriShape's mesh id
	// (rendererData, startIndex, triCount). unique/total => instanceable fraction (repeated meshes = the win).
	std::vector<std::uint64_t> g_shadowDrawIds;
	std::uint64_t              g_shadowDrawTotalVerts = 0;

	struct DrawTriShapeHook
	{
		static void thunk(std::int64_t a1, std::int64_t a2, std::uint32_t a3, std::int32_t a4)
		{
			if (g_inShadowTiming.load(std::memory_order_relaxed)) {
				std::uint64_t h = static_cast<std::uint64_t>(a2) * 1099511628211ull;
				h ^= (static_cast<std::uint64_t>(a3) << 32) | static_cast<std::uint32_t>(a4);
				g_shadowDrawIds.push_back(h);
				g_shadowDrawTotalVerts += 3ull * static_cast<std::uint32_t>(a4);
			}
			reinterpret_cast<void (*)(std::int64_t, std::int64_t, std::uint32_t, std::int32_t)>(func.address())(a1, a2, a3, a4);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	winrt::com_ptr<ID3D11DeviceContext> g_deferred;
	// Concurrent fan-out: one deferred context per worker thread (index 0 aliases g_deferred).
	std::vector<winrt::com_ptr<ID3D11DeviceContext>> g_deferredPool;

	void ReplayOneMap(MapWork& mw);       // defined below (needs GlobalStateGuard); called per-map from the detour
	void RenderMapInstanced(MapWork& mw);  // defined below; per-map instanced draws on the immediate context
}

std::int32_t ShadowThreaded::RenderShadowmapDetour(void* a1, std::int64_t a2, void* a3, std::int32_t a4, void* a_original)
{
	auto callOriginal = *static_cast<std::int32_t(**)(void*, std::int64_t, void*, std::int32_t)>(a_original);
	if (!g_claiming && GetMode() != Mode::kCapture)
		return callOriginal(a1, a2, a3, a4);

	// Open this map's pass bracket, run the original (slice alloc + RT setup + NiCamera walk; the
	// covered passes flow through CaptureHook, which snapshots the clean per-map render-state block
	// at the FIRST covered pass -- while the map's DSV/viewport are still bound -- and stores each
	// covered pass, skipping the inline render when claiming).
	g_mapWorkList.emplace_back();
	g_curMap = &g_mapWorkList.back();

	const std::int32_t r = callOriginal(a1, a2, a3, a4);

	// Per-map replay: execute this map's claimed passes NOW, while its view globals are current.
	if (GetMode() == Mode::kWorkerSerial && g_curMap)
		ReplayOneMap(*g_curMap);
	else if (GetMode() == Mode::kInstance && g_curMap)
		RenderMapInstanced(*g_curMap);

	g_curMap = nullptr;
	return r;
}

void ShadowThreaded::RenderShadowmapsDetour(void* a_original)
{
	auto callOriginal = *static_cast<void (**)()>(a_original);
	const Mode m = GetMode();

	// State-validation (leak detector): entry-state compare. The snapshot here reflects the PREVIOUS
	// frame's full pipeline, so divergence from the vanilla-entry baseline == the previous frame's
	// replica walk leaked state the engine did not recover from. Inert unless armed (env or devbench).
	if (StateValidationEnabled()) {
		if (auto* ctx = *engine::g_immediateContext)
			g_stateValidator.OnEntry(ctx, m == Mode::kOff);
	}

	// Stage 1a: single-threaded full ownership at BeginPass. No deferred context, no capture/replay --
	// the engine drives the walk (per-map slice alloc + RT/DSV + clear + NiCamera traversal) exactly as
	// vanilla; only BeginPass is replaced, inline, on the immediate context. Arm the gate around the
	// walk so the replacement is scoped to the shadow driver, then disarm.
	if (m == Mode::kOwnBeginPass || m == Mode::kOwnBeginPassVerify) {
		const bool verify = (m == Mode::kOwnBeginPassVerify);
		g_claiming = false;
		g_ownBeginPassCalls = 0;
		g_ownBeginPassVerify.store(verify, std::memory_order_relaxed);
		// Regime-B draw-state capture: when CS_RE_REFLECT is set, fingerprint the shadow draws issued
		// during this walk (arm on the render thread; the DrawIndexed hook does the readback). Sampled
		// (first N draws) at this milestone -- proves the *Get*+reflection capture on real shadow draws.
		const bool drawState = vanilla::ShaderReflect::WantsCapture();
		if (drawState) {
			vanilla::DrawStateValidator::GetSingleton()->BeginRun();
			vanilla::DrawStateValidator::GetSingleton()->ArmCapture(true);
		}
		g_ownBeginPass.store(true, std::memory_order_relaxed);
		callOriginal();
		g_ownBeginPass.store(false, std::memory_order_relaxed);
		if (drawState) {
			vanilla::DrawStateValidator::GetSingleton()->ArmCapture(false);
			vanilla::DrawStateValidator::GetSingleton()->LogRunSummary(verify ? "verify" : "ownBeginPass");
		}
		if ((++frames & 0x3F) == 1) {
			if (verify) {
				// Command-level validation: for each shadow BeginPass, the replica's computed pass
				// enumeration was diffed against the engine's actual dispatch (engine renders; no double
				// run). DIVERGED=0 over the run proves the hash lookup + chain walk + v11 match the engine.
				std::uint64_t groups = 0, diverged = 0;
				UtilityPassReplica::GetSingleton()->GetBeginPassCompareStats(groups, diverged);
				logger::info("[ShadowThreaded][ownBeginPassVerify] frame {}: calls={} compared={} DIVERGED={} atlasHash={:016X}",
					frames, g_ownBeginPassCalls, groups, diverged, HashShadowAtlas(4));
			} else {
				// calls>0 confirms the seam is live; atlasHash is a within-run liveness probe (the atlas
				// is actively rendered by BeginPassReplica). Visual sign-off is the correctness gate here.
				logger::info("[ShadowThreaded][ownBeginPass] frame {}: BeginPass replica calls={} atlasHash={:016X}",
					frames, g_ownBeginPassCalls, HashShadowAtlas(4));
			}
		}
		return;
	}

	// Regime-B MT gate: arm the capture hook so each covered shadow pass runs the engine-vs-worker
	// draw-state compare (VerifyPassDrawStateThreaded). No deferred context / replay needed -- the verify
	// renders inline. Requires CS_RE_REFLECT for the reflection-masked CB comparison.
	if (m == Mode::kDrawStateVerify) {
		auto* const replica = UtilityPassReplica::GetSingleton();
		g_claiming = false;
		g_drawStateVerify.store(true, std::memory_order_relaxed);
		replica->SetShadowCaptureHook(&CaptureHook);
		callOriginal();
		replica->SetShadowCaptureHook(nullptr);
		g_drawStateVerify.store(false, std::memory_order_relaxed);
		if ((++frames & 0x3F) == 1)
			logger::info("[ShadowThreaded][drawStateVerify] frame {}: reflect={} (see [FpVerify] for pairs/diverged)",
				frames, vanilla::ShaderReflect::WantsCapture() ? 1 : 0);
		return;
	}

	if (m == Mode::kParallelCull) {
		static const bool s_serial = [] {
			char b[8] = {};
			return GetEnvironmentVariableA("CS_SHADOW_CULL_SERIAL", b, sizeof(b)) && b[0] && b[0] != '0';
		}();
		if (s_serial) {
			// Stage 1 (validated): CullHook runs each cull on ONE worker + joins. No fan-out, no defer.
			callOriginal();
			return;
		}
		// Stage 2: parallel cull + serial submit.
		LARGE_INTEGER freq, t0, t1;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&t0);
		g_deferredSubmits.clear();
		g_accumSeenThisFrame.clear();
		g_accumCountThisFrame.clear();
		g_curMapDeferred = false;
		g_pcFrame.fetch_add(1, std::memory_order_relaxed);
		pclog("loopStart", nullptr);
		g_pcActive.store(true, std::memory_order_relaxed);
		callOriginal();  // CullHook runs each map's setup inline + dispatches its walk; Func43Hook defers submits
		g_pcActive.store(false, std::memory_order_relaxed);

		// The walks were dispatched during the light loop (at each map's position). Barrier here.
		pclog("barrierBefore", nullptr);
		g_cullPool.Wait();  // every dispatched walk has filled its accumulator
		pclog("barrierAfter", nullptr);

		// Serial submit phase: re-establish each deferred map's camera + RT, then submit (original Func43).
		auto* ctx = globals::d3d::context;
		auto  setCam = reinterpret_cast<SetCameraData_t>(REL::Offset(0xd7bab0).address());
		auto  setAcc = reinterpret_cast<void(__fastcall*)(void*)>(REL::Offset(0x12966b0).address());
		auto  func43 = reinterpret_cast<Func43_t>(Func43Hook::func.address());  // trampoline = original submit
		void* camData = reinterpret_cast<void*>(REL::Offset(0x302c890).address());
		auto  camPtr = reinterpret_cast<void**>(REL::Offset(0x31d0e68).address());
		for (auto& d : g_deferredSubmits) {
			pclog("submit", d.accum);
			setCam(camData, d.camera, d.flags);
			*camPtr = d.camera;
			setAcc(d.accum);
			ID3D11RenderTargetView* rtvs[8] = {};
			for (UINT i = 0; i < d.numRtv; ++i)
				rtvs[i] = d.rtvs[i].get();
			ctx->OMSetRenderTargets(d.numRtv, rtvs, d.dsv.get());
			if (d.numVp)
				ctx->RSSetViewports(d.numVp, d.vps);
			func43(d.accum, static_cast<std::int64_t>(d.flags));
		}
		// Update the shared-accumulator histogram for next frame (light set is stable frame-to-frame).
		g_accumSharedLastFrame.clear();
		for (auto& kv : g_accumCountThisFrame)
			if (kv.second > 1)
				g_accumSharedLastFrame.insert(kv.first);
		g_haveHistogram = true;
		QueryPerformanceCounter(&t1);
		{
			static std::uint64_t s_lastEnter = 0, s_shWall = 0, s_frWall = 0, s_n = 0;
			const std::uint64_t enter = static_cast<std::uint64_t>(t0.QuadPart);
			if (s_lastEnter) {
				s_frWall += enter - s_lastEnter;
				s_shWall += static_cast<std::uint64_t>(t1.QuadPart - t0.QuadPart);
				s_n++;
			}
			s_lastEnter = enter;
			if (s_n >= 128) {
				const double f = static_cast<double>(freq.QuadPart);
				logger::info("[ParallelCullTiming] RenderShadowmaps wall {:.3f}ms/frame ({:.1f}% of {:.3f}ms frame) "
							 "deferred(parallel)={} sharedInline={} (n={})",
					1000.0 * s_shWall / s_n / f, s_frWall ? 100.0 * s_shWall / s_frWall : 0.0,
					1000.0 * s_frWall / s_n / f, g_deferredSubmits.size(), g_accumSharedLastFrame.size(), s_n);
				s_shWall = s_frWall = s_n = 0;
			}
		}
		return;
	}

	// kInstance: claim the instanceable subset per map and issue the instanced draws inline (per-map, in
	// RenderShadowmapDetour, while the map's view globals are live). No deferred context -- the draws run
	// on the immediate context so the reduced draw count is the render-thread FPS win.
	if (m == Mode::kInstance) {
		auto* const replica = UtilityPassReplica::GetSingleton();
		g_mapWorkList.clear();
		g_mapWorkList.reserve(24);
		g_curMap = nullptr;
		g_claiming = true;
		g_instanceRestrict = true;
		replica->SetShadowCaptureHook(&CaptureHook);
		callOriginal();  // per map: claims instanceable passes; RenderShadowmapDetour issues the instanced draws
		replica->SetShadowCaptureHook(nullptr);
		g_claiming = false;
		g_instanceRestrict = false;
		// Detector SELF-TEST (devbench stateval {"selftest":true}): deliberately desync the GPU
		// rasterizer state from the engine's CPU-side block model -- the exact leak class the state
		// validator exists to catch (the block still believes its raster mode, so the engine skips
		// re-binding and the main scene renders with the wrong state). With the validator armed,
		// 'raster' MUST diverge at the next frame's entry; if it does not, the detector is broken.
		// Runtime-armed ONLY: enabling this during a load screen breaks loading.
		if (stateValSelftest.load(std::memory_order_relaxed))
			(*engine::g_immediateContext)->RSSetState(nullptr);
		if ((++frames & 0x3F) == 1) {
			std::uint64_t tot = 0, uns = 0;
			for (auto& mw : g_mapWorkList) {
				tot += mw.passes.size();
				uns += mw.unsupported;
			}
			logger::info("[ShadowThreaded][instance] frame {}: maps={} instancedPasses={} inline={} atlasHash={:016X}",
				frames, g_mapWorkList.size(), tot, uns, HashShadowAtlas(4));
			DumpShadowAtlas(4, "instanced");
		}
		return;
	}

	if (m == Mode::kOff || !g_deferred) {
		// Direct wall-clock + render-thread-CPU measurement of RenderShadowmaps (CS_SHADOW_TIMING).
		// RenderShadowmaps is once-per-frame and synchronous on the render thread, so: the QPC span of
		// callOriginal() is the shadow WALL time; the QueryThreadCycleTime span is the shadow CPU cost;
		// and the deltas between consecutive entries are the whole-frame wall period / whole-frame render-
		// thread cycles. shadowCyc/frameCyc = fraction of the RENDER THREAD's CPU spent in shadows (the
		// honest answer to "does RenderShadowmaps cost a lot of CPU"), independent of stack-scan attribution.
		static const bool s_timing = [] {
			char b[8] = {};
			return GetEnvironmentVariableA("CS_SHADOW_TIMING", b, sizeof(b)) && b[0] && b[0] != '0';
		}();
		if (!s_timing) {
			callOriginal();
			// Vanilla-atlas hash for the instanced-vs-vanilla A/B (CS_SHADOW_HASH): toggle mode 0/9 on a
			// FROZEN scene; identical hashes => instanced atlas is bit-exact (residual is post/exposure, not
			// the shadow depth); differing => the instanced shadow atlas itself differs.
			if (ShadowHashEnabled() && (++frames & 0xF) == 1) {
				logger::info("[ShadowHash] mode0(vanilla) frame {}: atlasHash={:016X}", frames, HashShadowAtlas(4));
				DumpShadowAtlas(4, "vanilla");
			}
			return;
		}
		LARGE_INTEGER freq, t0, t1;
		QueryPerformanceFrequency(&freq);
		ULONG64 cy0 = 0, cy1 = 0;
		const ULONG64 cullCyc0 = g_cullCyc;
		const std::uint64_t cullWall0 = g_cullWall;
		g_cullMapsDiag.clear();  // collect THIS frame's (camera, accumulator) pairs during the walk
		g_shadowDrawIds.clear();
		g_shadowDrawTotalVerts = 0;
		g_inShadowTiming.store(true, std::memory_order_relaxed);
		QueryThreadCycleTime(GetCurrentThread(), &cy0);
		QueryPerformanceCounter(&t0);
		callOriginal();
		QueryPerformanceCounter(&t1);
		QueryThreadCycleTime(GetCurrentThread(), &cy1);
		g_inShadowTiming.store(false, std::memory_order_relaxed);
		// CPU-only limiter test (CS_SHADOW_SPIN_US=N): when the A/B intervention is ON, busy-spin the render
		// thread N microseconds -- pure CPU, ZERO GPU/render effect. If the frame period grows by ~N us the
		// render thread is the frame limiter (parallelizing the cull will raise FPS); if not, it has slack.
		static const std::uint32_t s_spinUs = [] {
			char b[16] = {};
			return (GetEnvironmentVariableA("CS_SHADOW_SPIN_US", b, sizeof(b)) && b[0]) ? static_cast<std::uint32_t>(atoi(b)) : 0u;
		}();
		if (s_spinUs && g_intervene.load(std::memory_order_relaxed)) {
			LARGE_INTEGER s0, s1;
			QueryPerformanceCounter(&s0);
			const std::int64_t spinTicks = static_cast<std::int64_t>(freq.QuadPart) * s_spinUs / 1000000;
			do {
				QueryPerformanceCounter(&s1);
			} while (s1.QuadPart - s0.QuadPart < spinTicks);
		}
		const ULONG64 cullCycFrame = g_cullCyc - cullCyc0;
		const std::uint64_t cullWallFrame = g_cullWall - cullWall0;
		static std::uint64_t s_lastEnter = 0, s_lastCy = 0;
		static std::uint64_t s_shWall = 0, s_frWall = 0, s_shCyc = 0, s_frCyc = 0, s_n = 0;
		static std::uint64_t s_cullCyc = 0, s_cullWall = 0;
		const std::uint64_t enter = static_cast<std::uint64_t>(t0.QuadPart);
		if (s_lastEnter) {
			s_frWall += enter - s_lastEnter;
			s_frCyc += cy0 - s_lastCy;
			s_shWall += static_cast<std::uint64_t>(t1.QuadPart - t0.QuadPart);
			s_shCyc += cy1 - cy0;
			s_cullCyc += cullCycFrame;
			s_cullWall += cullWallFrame;
			s_n++;
		}
		s_lastEnter = enter;
		s_lastCy = cy0;
		if (s_n >= 128) {
			const double f = static_cast<double>(freq.QuadPart);
			const double shMs = 1000.0 * s_shWall / s_n / f;
			const double frMs = 1000.0 * s_frWall / s_n / f;
			const double cpuPct = s_frCyc ? 100.0 * s_shCyc / s_frCyc : 0.0;
			const double wallPct = s_frWall ? 100.0 * s_shWall / s_frWall : 0.0;
			// Split the shadow render-thread CPU into CULL (sub_1412C1600) vs SUBMISSION+setup (remainder).
			const double cullPctOfShadow = s_shCyc ? 100.0 * s_cullCyc / s_shCyc : 0.0;
			const double cullMs = 1000.0 * s_cullWall / s_n / f;
			// A/B mode: label this window with the intervention state active for it, then flip for the next.
			// Alternating lines compare frame time with the intervention ON vs OFF at the same view.
			static const bool s_skipMode = [] {
				char b[8] = {};
				return GetEnvironmentVariableA("CS_SHADOW_SKIP_CULL", b, sizeof(b)) && b[0] && b[0] != '0';
			}();
			const bool s_abMode = s_skipMode || (s_spinUs != 0);
			const bool windowIntervened = g_intervene.load(std::memory_order_relaxed);
			const char* interv = !s_abMode ? "baseline" : (windowIntervened ? (s_skipMode ? "SKIP" : "SPIN") : "off");
			logger::info("[ShadowTiming] ab={} RenderShadowmaps: wall {:.3f}ms/frame ({:.1f}% of {:.3f}ms frame), "
						 "render-thread CPU {:.1f}% of frame | CULL {:.3f}ms ({:.1f}% of shadow CPU), "
						 "SUBMIT+setup {:.1f}%  (n={})",
				interv, shMs, wallPct, frMs, cpuPct, cullMs, cullPctOfShadow, 100.0 - cullPctOfShadow, s_n);
			if (s_abMode)
				g_intervene.store(!windowIntervened, std::memory_order_relaxed);
			// Accumulator-distinctness (this frame): how many shadow maps, how many DISTINCT accumulators/cameras.
			// distinct==total ⇒ per-map accumulators ⇒ Phase-B parallel walks into their own accumulators are safe.
			{
				std::set<void*> accs, cams;
				for (auto& p : g_cullMapsDiag) {
					cams.insert(p.first);
					accs.insert(p.second);
				}
				logger::info("[ShadowTiming] cullMaps: total={} distinctAccumulators={} distinctCameras={}",
					g_cullMapsDiag.size(), accs.size(), cams.size());
			}
			// Instancing payoff: unique static-mesh draws vs total. instanceable = 1 - unique/total.
			if (!g_shadowDrawIds.empty()) {
				std::set<std::uint64_t> uniq(g_shadowDrawIds.begin(), g_shadowDrawIds.end());
				const double instFrac = 1.0 - static_cast<double>(uniq.size()) / g_shadowDrawIds.size();
				logger::info("[ShadowInstance] staticDraws={} uniqueMeshes={} instanceable={:.1f}% totalIndices={}",
					g_shadowDrawIds.size(), uniq.size(), 100.0 * instFrac, g_shadowDrawTotalVerts);
			}
			s_shWall = s_frWall = s_shCyc = s_frCyc = s_n = 0;
			s_cullCyc = s_cullWall = 0;
		}
		return;
	}

	auto* replica = UtilityPassReplica::GetSingleton();
	auto* deferred = g_deferred.get();
	auto* immediate = *engine::g_immediateContext;

	g_mapWorkList.clear();
	g_mapWorkList.reserve(24);
	g_curMap = nullptr;
	g_claiming = (m != Mode::kCapture);  // capture observes; every replay mode claims the covered passes
	g_concurrentRestrict = (m == Mode::kConcurrent);  // restrict the concurrent claim to the safe subset
	replica->SetShadowCaptureHook(&CaptureHook);

	callOriginal();  // per-map interceptor brackets each map; covered passes stored (+claimed if replaying)

	replica->SetShadowCaptureHook(nullptr);

	if (m == Mode::kCapture) {
		// Observe-only: nothing was claimed, the engine rendered inline; just report the partition.
		g_claiming = false;
		if ((++frames & 0x3F) == 1) {
			std::uint64_t tot = 0, uns = 0;
			for (auto& mw : g_mapWorkList) { tot += mw.passes.size(); uns += mw.unsupported; }
			// VANILLA ground-truth atlas hash (engine rendered inline this mode).
			logger::info("[ShadowThreaded][vanilla] frame {}: maps={} passes={} unsupported={} atlasHash={:016X}",
				frames, g_mapWorkList.size(), tot, uns, HashShadowAtlas(4));
		}
		return;
	}

	if (m == Mode::kWorkerSerial) {
		// Per-map replay already happened inside each RenderShadowmapDetour (while its view globals
		// were current); nothing to do after the whole walk.
		g_claiming = false;
		return;
	}
	if (m == Mode::kConcurrent) {
		ReplayConcurrent();
		g_claiming = false;
		return;
	}

	// ---- Phase 0 (kSerial): serial deferred replay of the claimed passes, in map order ----
	// Snapshot the pre-shadow render-state so the immediate context (which the walk mutated) and
	// the mirror are restored after the scope, exactly as ShadowDeferred does for its single scope.
	static std::vector<std::uint8_t> s_snapshot(engine::kBlockBytes);
	std::memcpy(s_snapshot.data(), reinterpret_cast<void*>(engine::S_base.address()), engine::kBlockBytes);
	const auto  savedTechnique = *engine::g_currentTechnique;
	auto* const savedShader = *engine::g_currentShader;
	auto* const savedMaterial = *engine::g_currentMaterial;
	const auto  savedBoneCursor = *engine::g_boneCBRingCursor;
	const auto  savedDynVB = *engine::g_dynVBRingState;
	const auto  savedShadowToken = *engine::g_shadowGeomToken;

	UINT           savedVpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT savedVps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	immediate->RSGetViewports(&savedVpCount, savedVps);
	UINT       savedScCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_RECT savedScs[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	immediate->RSGetScissorRects(&savedScCount, savedScs);

	// Redirect the engine + CS context globals to the deferred context for the replay scope. The
	// replica's WsCtx()/WsBlock() then read the deferred context + the (per-map restored) engine
	// block; ReplicaRenderPassImmediately records onto the deferred context.
	auto* const savedCsCtx = globals::d3d::context;
	*engine::g_immediateContext = deferred;
	globals::d3d::context = deferred;

	BindPersistentStandalone(deferred);
	{  // seed a wide viewport/scissor; each map's block restore + forced-dirty overrides the viewport
		const D3D11_VIEWPORT vp{ 0.0f, 0.0f, 16384.0f, 16384.0f, 0.0f, 1.0f };
		const D3D11_RECT     sc{ 0, 0, 16384, 16384 };
		deferred->RSSetViewports(1, &vp);
		deferred->RSSetScissorRects(1, &sc);
	}

	g_deferredDraws.store(0, std::memory_order_relaxed);
	auto* const sflags = reinterpret_cast<std::uint32_t*>(engine::S_base.address());
	for (auto& mw : g_mapWorkList) {
		if (mw.passes.empty())
			continue;
		// Seed the map's clean render-target state, force the main dirty word so the first pass's
		// SetDirtyStates re-binds RT/viewport/depth onto the fresh deferred context, and reset the
		// technique/shader/material change-detection caches so the first replayed pass runs a full
		// BeginPass/SetupTechnique (else a stale cache-hit would skip setup and use the block's
		// stale current-VS/PS pointers). Then replay in captured order.
		std::memcpy(reinterpret_cast<void*>(engine::S_base.address()), mw.block, engine::kBlockBytes);
		sflags[0] = 0xFFFFFFFFu;
		*engine::g_currentTechnique = 0;
		*engine::g_currentShader = nullptr;
		*engine::g_currentMaterial = nullptr;
		for (const auto& cp : mw.passes)
			replica->ReplicaRenderPassImmediately(cp.pass, cp.technique, cp.alphaTest, cp.renderFlags);
	}

	// Close + execute the recording in place (RestoreContextState=FALSE per DXVK guidance).
	std::uint64_t drawsThisFrame = g_deferredDraws.load(std::memory_order_relaxed);
	{
		winrt::com_ptr<ID3D11CommandList> cl;
		if (SUCCEEDED(deferred->FinishCommandList(FALSE, cl.put())) && cl)
			immediate->ExecuteCommandList(cl.get(), FALSE);
		else
			logger::error("[ShadowThreaded] FinishCommandList failed; shadows this frame lost");
	}

	// Restore the immediate context + mirror to their pre-scope state (the execute cleared the
	// immediate context; re-establish the persistent bindings + pre-scope viewport/scissor).
	*engine::g_immediateContext = immediate;
	globals::d3d::context = savedCsCtx;
	BindPersistentStandalone(immediate);
	if (savedVpCount)
		immediate->RSSetViewports(savedVpCount, savedVps);
	if (savedScCount)
		immediate->RSSetScissorRects(savedScCount, savedScs);
	std::memcpy(reinterpret_cast<void*>(engine::S_base.address()), s_snapshot.data(), engine::kBlockBytes);
	*engine::g_currentTechnique = savedTechnique;
	*engine::g_currentShader = savedShader;
	*engine::g_currentMaterial = savedMaterial;
	*engine::g_boneCBRingCursor = savedBoneCursor;
	*engine::g_dynVBRingState = savedDynVB;
	*engine::g_shadowGeomToken = savedShadowToken;
	sflags[0] = 0xFFFFFFFFu;  // force the engine's next SetDirtyStates to re-bind onto the cleared immediate ctx

	if ((++frames & 0x3F) == 1) {
		std::uint64_t tot = 0;
		for (auto& mw : g_mapWorkList) tot += mw.passes.size();
		// SERIAL-REPLAY atlas hash: on a frozen scene this must equal the vanilla hash for the same
		// spot, proving the deferred replay's shadow depth is bit-identical to the engine's.
		logger::info("[ShadowThreaded][serial] frame {}: maps={} replayed={} deferredDraws={} atlasHash={:016X}",
			frames, g_mapWorkList.size(), tot, drawsThisFrame, HashShadowAtlas(4));
	}
	g_claiming = false;
}

namespace
{
	// Snapshot/restore the pre-shadow global render-state around a replay scope. ExecuteCommandList
	// (RestoreContextState=FALSE) clears the immediate context, so after the replay the engine's
	// block is forced fully dirty and the persistent bindings + viewport are re-established, exactly
	// as the kSerial path does. The worker paths never write the global block themselves, but the
	// walk did and the execute cleared the GPU context, so the same restore is required.
	struct GlobalStateGuard
	{
		std::vector<std::uint8_t> block;
		std::uint32_t             technique;
		void*                     shader;
		void*                     material;
		std::uint32_t             boneCursor;
		std::uint64_t             dynVB;
		std::uint32_t             shadowToken;
		ID3D11DeviceContext*      immediate;
		UINT                      vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_VIEWPORT            vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		UINT                      scCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
		D3D11_RECT                scs[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];

		explicit GlobalStateGuard(ID3D11DeviceContext* a_immediate) :
			block(engine::kBlockBytes), immediate(a_immediate)
		{
			std::memcpy(block.data(), reinterpret_cast<void*>(engine::S_base.address()), engine::kBlockBytes);
			technique = *engine::g_currentTechnique;
			shader = *engine::g_currentShader;
			material = *engine::g_currentMaterial;
			boneCursor = *engine::g_boneCBRingCursor;
			dynVB = *engine::g_dynVBRingState;
			shadowToken = *engine::g_shadowGeomToken;
			immediate->RSGetViewports(&vpCount, vps);
			immediate->RSGetScissorRects(&scCount, scs);
		}

		void Restore()
		{
			BindPersistentStandalone(immediate);
			if (vpCount)
				immediate->RSSetViewports(vpCount, vps);
			if (scCount)
				immediate->RSSetScissorRects(scCount, scs);
			std::memcpy(reinterpret_cast<void*>(engine::S_base.address()), block.data(), engine::kBlockBytes);
			*engine::g_currentTechnique = technique;
			*engine::g_currentShader = shader;
			*engine::g_currentMaterial = material;
			*engine::g_boneCBRingCursor = boneCursor;
			*engine::g_dynVBRingState = dynVB;
			*engine::g_shadowGeomToken = shadowToken;
			*reinterpret_cast<std::uint32_t*>(engine::S_base.address()) = 0xFFFFFFFFu;  // force re-bind
		}
	};

	// Seed a wide viewport/scissor on a fresh deferred context; each map's block restore + forced
	// dirty overrides the viewport per map, but a fresh context needs a non-empty starting rect.
	void SeedWideViewport(ID3D11DeviceContext* a_ctx)
	{
		const D3D11_VIEWPORT vp{ 0.0f, 0.0f, 16384.0f, 16384.0f, 0.0f, 1.0f };
		const D3D11_RECT     sc{ 0, 0, 16384, 16384 };
		a_ctx->RSSetViewports(1, &vp);
		a_ctx->RSSetScissorRects(1, &sc);
	}

	// Replay ONE map's covered passes on a single deferred context, executed IMMEDIATELY -- called
	// per-map right after that map's RenderShadowmap, WHILE its per-map view globals (shadow camera
	// 0x1431D0F88, view frustum 0x1431D0E68, render mode 0x1431D0E28, the shadow view-proj on the
	// accumulator) are still current. Deferring the replay to after the whole walk made every map's
	// setup read the LAST map's camera, so the geometry transformed off-screen and no depth landed.
	void ReplayOneMap(MapWork& mw)
	{
		if (mw.passes.empty())
			return;
		auto* deferred = g_deferred.get();
		if (!deferred)
			return;
		auto* replica = UtilityPassReplica::GetSingleton();
		auto* immediate = *engine::g_immediateContext;

		GlobalStateGuard guard(immediate);
		auto*            worker = UtilityPassReplica::MakeShadowWorker(deferred, guard.shadowToken);
		UtilityPassReplica::WorkerBeginScope(worker);
		BindPersistentStandalone(deferred);
		SeedWideViewport(deferred);
		UtilityPassReplica::WorkerSeedMap(worker, mw.block);
		for (const auto& cp : mw.passes)
			replica->ReplicaRenderPassImmediately(cp.pass, cp.technique, cp.alphaTest, cp.renderFlags);
		UtilityPassReplica::WorkerEndScope();
		static std::atomic<int> s_thr{ 0 };
		const bool          doHash = ShadowHashEnabled() && (s_thr.fetch_add(1, std::memory_order_relaxed) % 400 == 0);
		const std::uint64_t before = doHash ? HashShadowAtlas(4) : 0;
		{
			winrt::com_ptr<ID3D11CommandList> cl;
			if (SUCCEEDED(deferred->FinishCommandList(FALSE, cl.put())) && cl)
				immediate->ExecuteCommandList(cl.get(), FALSE);
			else
				logger::error("[ShadowThreaded] per-map FinishCommandList failed");
		}
		if (doHash)
			logger::info("[ShadowThreaded][permap] passes={} beforeExec={:016X} afterExec={:016X}", mw.passes.size(), before, HashShadowAtlas(4));
		UtilityPassReplica::FreeShadowWorker(worker);
		guard.Restore();
	}

	// kInstance per-map: the engine's RenderShadowmap already rendered the non-instanceable passes inline
	// and unbound the map's DSV on return, so seed the map's CAPTURED render-state block (RT/DSV/viewport
	// snapshotted at the first covered pass while the DSV was live), force the main dirty word, and reset
	// the technique caches. Then RenderShadowInstanced groups the claimed passes by (mesh, technique) and
	// issues one DrawIndexedInstanced per group on the immediate context (the render-thread draw-count cut
	// that is the FPS win). Finally restore the engine's block+caches so the next map's engine render is
	// undisturbed. Save/restore (rather than a private worker block) keeps the draws on the immediate
	// context; the block is CPU-side dirty tracking, so the force-dirty makes the engine re-bind cleanly.
	void RenderMapInstanced(MapWork& mw)
	{
		if (mw.passes.empty())
			return;
		auto* const S = reinterpret_cast<std::uint8_t*>(engine::S_base.address());
		auto* const sflags = reinterpret_cast<std::uint32_t*>(S);

		static std::vector<std::uint8_t> s_saved(engine::kBlockBytes);
		std::memcpy(s_saved.data(), S, engine::kBlockBytes);
		const auto  savedTech = *engine::g_currentTechnique;
		auto* const savedShader = *engine::g_currentShader;
		auto* const savedMaterial = *engine::g_currentMaterial;

		std::memcpy(S, mw.block, engine::kBlockBytes);
		sflags[0] = 0xFFFFFFFFu;  // force first group's SetDirtyStates to re-bind RT/DSV/viewport
		*engine::g_currentTechnique = 0;
		*engine::g_currentShader = nullptr;
		*engine::g_currentMaterial = nullptr;

		static std::vector<RE::BSRenderPass*> s_passes;
		static std::vector<std::uint32_t>     s_techs;
		s_passes.clear();
		s_techs.clear();
		s_passes.reserve(mw.passes.size());
		s_techs.reserve(mw.passes.size());
		for (const auto& cp : mw.passes) {
			s_passes.push_back(cp.pass);
			s_techs.push_back(cp.technique);
		}
		UtilityPassReplica::GetSingleton()->RenderShadowInstanced(
			s_passes.data(), s_techs.data(), static_cast<std::uint32_t>(s_passes.size()), mw.passes[0].renderFlags);

		std::memcpy(S, s_saved.data(), engine::kBlockBytes);
		*engine::g_currentTechnique = savedTech;
		*engine::g_currentShader = savedShader;
		*engine::g_currentMaterial = savedMaterial;
		sflags[0] = 0xFFFFFFFFu;  // engine's next SetDirtyStates re-binds onto whatever we left bound
	}
}

void ShadowThreaded::ReplayWorkerSerial()
{
	// Phase 1a: replay every map through ONE ShadowWorker (private block+caches+context) on the
	// render thread. No global-context redirection: the replica's Ws* accessors resolve to the
	// worker, so the global block is untouched -- this proves the private-block path renders the
	// same shadows before threads are introduced.
	auto* replica = UtilityPassReplica::GetSingleton();
	auto* immediate = *engine::g_immediateContext;
	auto* deferred = g_deferred.get();

	GlobalStateGuard guard(immediate);

	auto* worker = UtilityPassReplica::MakeShadowWorker(deferred, guard.shadowToken);
	UtilityPassReplica::WorkerBeginScope(worker);
	BindPersistentStandalone(deferred);
	SeedWideViewport(deferred);

	g_deferredDraws.store(0, std::memory_order_relaxed);
	for (auto& mw : g_mapWorkList) {
		if (mw.passes.empty())
			continue;
		UtilityPassReplica::WorkerSeedMap(worker, mw.block);
		for (const auto& cp : mw.passes)
			replica->ReplicaRenderPassImmediately(cp.pass, cp.technique, cp.alphaTest, cp.renderFlags);
	}
	UtilityPassReplica::WorkerEndScope();

	const std::uint64_t draws = g_deferredDraws.load(std::memory_order_relaxed);
	{
		winrt::com_ptr<ID3D11CommandList> cl;
		if (SUCCEEDED(deferred->FinishCommandList(FALSE, cl.put())) && cl)
			immediate->ExecuteCommandList(cl.get(), FALSE);
		else
			logger::error("[ShadowThreaded] worker-serial FinishCommandList failed; shadows lost");
	}
	UtilityPassReplica::FreeShadowWorker(worker);

	guard.Restore();

	if ((++frames & 0x3F) == 1) {
		std::uint64_t tot = 0;
		for (auto& mw : g_mapWorkList) tot += mw.passes.size();
		logger::info("[ShadowThreaded][worker-serial] frame {}: maps={} replayed={} deferredDraws={} atlasHash={:016X}",
			frames, g_mapWorkList.size(), tot, draws, HashShadowAtlas(4));
	}
}

void ShadowThreaded::ReplayConcurrent()
{
	// Phase 1b (EXPERIMENTAL -- CS_SHADOW_MT=4, off by default): fan the maps out across workerCount
	// threads, each recording its maps onto its OWN deferred context with its OWN ShadowWorker
	// (private block+caches). Maps render to independent atlas slices, so command lists may execute
	// in any order. Skinned passes stay on the serial remainder (their bone-CB/dyn-VB rings are
	// still global).
	//
	// KNOWN BLOCKER (worker-serial mode 3 is correct; this races): the byte-exact setup path still
	// mutates SHARED state that must be worker-localized before this is safe --
	//   1. FlushSetupTechniqueReplica calls the engine BeginTechnique, which writes the GLOBAL block
	//      +0x348/0x350 (current VS/PS shader objects) and VS/PS-binds; concurrent workers race, and
	//      a worker then reads its OWN block+0x348 (stale, not what BeginTechnique wrote).
	//   2. The setup stashes each stage's mapped-CB pointer + the per-pass technique flags IN the
	//      SHARED singleton BSUtilityShader object (vs+0x20, ps+0x18, VS+0x40, a1+0x90/0x94, ...),
	//      used as pass-local scratch -- shared across threads.
	//   3. The PerTechnique/Material/Geometry CBs are owned by that shared shader object; concurrent
	//      Map(WRITE_DISCARD) of the same buffer from multiple deferred contexts is unsafe.
	// Fixing needs: BeginTechnique output copied to the worker block under a mutex; the CB-pointer +
	// technique-flag scratch moved to worker-local cells; and per-worker CB buffers. See
	// docs/development/mt-shadow-plan.md. Until then mode 4 is a diagnostic that WILL crash under
	// load; mode 3 (worker-serial) is the correct, validated private-block replay.
	logger::warn("[ShadowThreaded] kConcurrent is EXPERIMENTAL: shared shader-object scratch is not "
				 "yet worker-local -- expect corruption. Use CS_SHADOW_MT=3 for the correct path.");
	auto* replica = UtilityPassReplica::GetSingleton();
	auto* immediate = *engine::g_immediateContext;

	const std::uint32_t n = std::min<std::uint32_t>(workerCount, static_cast<std::uint32_t>(g_deferredPool.size()));
	if (n == 0) {
		logger::error("[ShadowThreaded] no worker contexts; concurrent replay skipped");
		return;
	}

	GlobalStateGuard guard(immediate);

	// Partition the non-empty maps round-robin across the workers.
	std::vector<std::vector<MapWork*>> assign(n);
	{
		std::uint32_t next = 0;
		for (auto& mw : g_mapWorkList) {
			if (mw.passes.empty())
				continue;
			assign[next % n].push_back(&mw);
			++next;
		}
	}

	g_deferredDraws.store(0, std::memory_order_relaxed);
	std::vector<winrt::com_ptr<ID3D11CommandList>> lists(n);

	// CS_SHADOW_MT_CLEARTEST: worker 0 records an explicit ClearDepthStencilView(target-4 slice-0
	// read-write DSV, depth=0.5) before its passes. If the atlas hash then changes, deferred-context
	// command-list execution reaches the atlas (so the missing shadows are a draw-STATE bug); if not,
	// the CL execution itself is not landing.
	static const bool s_clearTest = [] { char b[8] = {}; return GetEnvironmentVariableA("CS_SHADOW_MT_CLEARTEST", b, sizeof(b)) && b[0] && b[0] != '0'; }();
	ID3D11DepthStencilView* testDsv = nullptr;
	if (s_clearTest) {
		auto* rtBase = *engine::g_rtPoolPtr;
		if (rtBase)
			testDsv = *reinterpret_cast<ID3D11DepthStencilView**>(rtBase + 8 * (0 + 19 * 4) + 0x1FB0);
	}

	const auto recordWorker = [&](std::uint32_t i) {
		auto* ctx = g_deferredPool[i].get();
		auto* worker = UtilityPassReplica::MakeShadowWorker(ctx, guard.shadowToken);
		UtilityPassReplica::WorkerBeginScope(worker);
		BindPersistentStandalone(ctx);
		SeedWideViewport(ctx);
		if (i == 0 && testDsv)
			ctx->ClearDepthStencilView(testDsv, D3D11_CLEAR_DEPTH, 0.5f, 0);
		for (MapWork* mw : assign[i]) {
			UtilityPassReplica::WorkerSeedMap(worker, mw->block);
			for (const auto& cp : mw->passes)
				replica->ReplicaRenderPassImmediately(cp.pass, cp.technique, cp.alphaTest, cp.renderFlags);
		}
		UtilityPassReplica::WorkerEndScope();
		if (FAILED(ctx->FinishCommandList(FALSE, lists[i].put())))
			logger::error("[ShadowThreaded] concurrent FinishCommandList failed (worker {})", i);
		UtilityPassReplica::FreeShadowWorker(worker);
	};

	// Worker 0 records inline on this thread; the rest on worker threads. Join before executing.
	std::vector<std::thread> pool;
	pool.reserve(n - 1);
	for (std::uint32_t i = 1; i < n; ++i)
		pool.emplace_back(recordWorker, i);
	recordWorker(0);
	for (auto& t : pool)
		t.join();

	// DIAGNOSTIC: hash the atlas BEFORE the worker CLs execute (walk result = serial remainder only)
	// and AFTER; if before == after the worker draws are NOT landing on target 4.
	const bool          willLog = ((frames + 1) & 0x3F) == 1;
	if (willLog) {
		// Target-4 SRV (what the game samples + HashShadowAtlas reads) vs target-4 slice-0 DSV texture
		// (what the workers draw to). If these resources DIFFER there is a per-map copy the deferred
		// draws miss; if SAME, the draws should land and the bug is elsewhere.
		auto* srv = *reinterpret_cast<ID3D11ShaderResourceView**>(engine::g_renderer.address() + 152 * 4 + 0x2040);
		void* srvRes = nullptr;
		if (srv) { winrt::com_ptr<ID3D11Resource> r; srv->GetResource(r.put()); srvRes = r.get(); }
		auto* rtBase = *engine::g_rtPoolPtr;
		const std::size_t dsIdx = 0 + 19 * 4;
		auto* dsvA = rtBase ? *reinterpret_cast<ID3D11DepthStencilView**>(rtBase + 8 * dsIdx + 0x1FB0) : nullptr;
		auto* dsvB = rtBase ? *reinterpret_cast<ID3D11DepthStencilView**>(rtBase + 8 * dsIdx + 0x1FF0) : nullptr;
		D3D11_DEPTH_STENCIL_VIEW_DESC da{}, db{};
		if (dsvA) dsvA->GetDesc(&da);
		if (dsvB) dsvB->GetDesc(&db);
		const std::uint8_t dsByte = rtBase ? *reinterpret_cast<std::uint8_t*>(rtBase + 0x22) : 0xEE;
		(void)srvRes;
		logger::info("[ShadowThreaded][dsvdiag] rtBase+0x22={} dsvA(1FB0) flags={:X} dim={} dsvB(1FF0) flags={:X} dim={} (READ_ONLY_DEPTH=1)",
			dsByte, da.Flags, static_cast<int>(da.ViewDimension), db.Flags, static_cast<int>(db.ViewDimension));
		for (auto& mw : g_mapWorkList) {
			if (mw.passes.empty())
				continue;
			const auto* vp = reinterpret_cast<const float*>(mw.block + 0x70);  // D3D11_VIEWPORT in the block
			logger::info("[ShadowThreaded][dsvdiag] map0 viewport S+0x70 = x={} y={} w={} h={} minZ={} maxZ={}",
				vp[0], vp[1], vp[2], vp[3], vp[4], vp[5]);
			break;
		}
	}
	const std::uint64_t beforeHash = willLog ? HashShadowAtlas(4) : 0;

	for (std::uint32_t i = 0; i < n; ++i)
		if (lists[i])
			immediate->ExecuteCommandList(lists[i].get(), FALSE);

	guard.Restore();

	if ((++frames & 0x3F) == 1) {
		std::uint64_t tot = 0;
		for (auto& mw : g_mapWorkList) tot += mw.passes.size();
		logger::info("[ShadowThreaded][concurrent] frame {}: workers={} maps={} replayed={} deferredDraws={} beforeExec={:016X} afterExec={:016X}",
			frames, n, g_mapWorkList.size(), tot, g_deferredDraws.load(std::memory_order_relaxed), beforeHash, HashShadowAtlas(4));
	}
}

void ShadowThreaded::InstallHooks()
{
	if (hooksInstalled)
		return;

	auto* device = globals::d3d::device;
	// One deferred context per worker (kConcurrent uses workerCount; serial modes use index 0). The
	// DrawIndexed vtable slot (idx 12) is patched once -- the deferred-context vtable is shared, so
	// the counter fires for every pool context. Count is best-effort/log-only, so a benign data race
	// on g_deferredDraws across worker threads is acceptable (relaxed atomic).
	const std::uint32_t poolSize = std::max<std::uint32_t>(1, workerCount);
	g_deferredPool.resize(poolSize);
	bool anyCtx = false;
	for (std::uint32_t i = 0; i < poolSize; ++i) {
		if (SUCCEEDED(device->CreateDeferredContext(0, g_deferredPool[i].put())) && g_deferredPool[i]) {
			Util::SetResourceName(g_deferredPool[i].get(), "ShadowThreaded::DeferredContext");
			if (!anyCtx) {
				anyCtx = true;
				auto** vtbl = *reinterpret_cast<void***>(g_deferredPool[i].get());
				g_origDeferredDrawIndexed = reinterpret_cast<DrawIndexed_t>(vtbl[12]);
				DWORD old = 0;
				if (VirtualProtect(&vtbl[12], sizeof(void*), PAGE_READWRITE, &old)) {
					vtbl[12] = reinterpret_cast<void*>(&DrawIndexedCounter);
					VirtualProtect(&vtbl[12], sizeof(void*), old, &old);
				}
			}
		}
	}
	if (anyCtx)
		g_deferred = g_deferredPool[0];
	else
		logger::error("[ShadowThreaded] CreateDeferredContext failed; replay disabled");

	stl::detour_thunk<RenderShadowmapsHook>(REL::RelocationID(100420, 0));
	stl::detour_thunk<RenderShadowmapHook>(REL::RelocationID(100820, 0));

	// BeginPass detour (SE-only; guarded by IsSE in Setup). RelocationID isn't wired for this leaf,
	// and every offset in this subsystem is already SE-1.5.97-hardcoded, so target it by module
	// offset directly -- the same DetourAttach dance stl::detour_thunk performs.
	BeginPassHook::func = REL::Offset(0x1308030).address();
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(reinterpret_cast<PVOID*>(&BeginPassHook::func), reinterpret_cast<PVOID>(BeginPassHook::thunk));
	DetourTransactionCommit();

	// kParallelCull: intercept the per-map cull (sub_1412C1600) + the per-map submit (Func43), spin up the
	// worker (Stage 1 fallback) + the fan-out pool (Stage 2).
	if (GetMode() == Mode::kParallelCull) {
		CullHook::func = REL::Offset(0x12C1600).address();
		Func43Hook::func = REL::Offset(0x12CAC90).address();
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(reinterpret_cast<PVOID*>(&CullHook::func), reinterpret_cast<PVOID>(CullHook::thunk));
		DetourAttach(reinterpret_cast<PVOID*>(&Func43Hook::func), reinterpret_cast<PVOID>(Func43Hook::thunk));
		DetourTransactionCommit();
		const std::uint32_t poolN = workerCount > 1 ? workerCount : 6;
		g_cullWorker.Start();
		g_cullPool.Start(poolN);
		logger::info("[ShadowThreaded] kParallelCull: Cull+Func43 detours installed, worker + pool({}) started", poolN);
	}

	hooksInstalled = true;
	logger::info("[ShadowThreaded] detoured RenderShadowmaps @ 0x{:X}, RenderShadowmap @ 0x{:X}, BeginPass @ 0x{:X}",
		RenderShadowmapsHook::func.address(), RenderShadowmapHook::func.address(), BeginPassHook::func.address());
}

std::array<std::uint32_t, 4> ShadowThreaded::StateValReport() const
{
	return { g_stateValidator.baselineFrames, g_stateValidator.checkedFrames,
		g_stateValidator.divergences, g_stateValidator.canaryHits };
}

void ShadowThreaded::Setup()
{
	char buf[8] = {};
	if (GetEnvironmentVariableA("CS_SHADOW_MT", buf, sizeof(buf)) && buf[0]) {
		const int v = atoi(buf);
		if (v >= 0 && v <= 9)
			mode.store(static_cast<Mode>(v), std::memory_order_relaxed);
	}
	if (char wc[8] = {}; GetEnvironmentVariableA("CS_SHADOW_MT_WORKERS", wc, sizeof(wc)) && wc[0]) {
		const int n = atoi(wc);
		if (n >= 1 && n <= 8)
			workerCount = static_cast<std::uint32_t>(n);
	}
	const bool timingOnly = [] {
		char tb[8] = {};
		return GetEnvironmentVariableA("CS_SHADOW_TIMING", tb, sizeof(tb)) && tb[0] && tb[0] != '0';
	}();
	if (!IsActive()) {
		// Measurement-only (CS_SHADOW_TIMING at mode 0): install JUST the RenderShadowmaps detour so the
		// mode-0 branch can wall-clock/CPU-time the otherwise-vanilla shadow walk. No other hooks (no
		// deferred pool, no per-map/BeginPass detours) -> callOriginal() runs pure vanilla = zero behavior
		// change; the numbers are the real cost of the unmodified shadow pipeline.
		if (timingOnly && REL::Module::IsSE() && !hooksInstalled) {
			stl::detour_thunk<RenderShadowmapsHook>(REL::RelocationID(100420, 0));
			// CullHook (NiCamera::sub_1412C1600 @ module+0x12C1600) to split cull vs submission. Same
			// DetourAttach dance as BeginPass -- RelocationID isn't wired for this leaf; SE-1.5.97 offset.
			CullHook::func = REL::Offset(0x12C1600).address();
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(reinterpret_cast<PVOID*>(&CullHook::func), reinterpret_cast<PVOID>(CullHook::thunk));
			DetourTransactionCommit();
			// Shadow-instancing payoff diagnostic: hook DrawTriShape to count repeated meshes.
			if (char ib[8] = {}; GetEnvironmentVariableA("CS_SHADOW_INSTANCE_DIAG", ib, sizeof(ib)) && ib[0] && ib[0] != '0') {
				DrawTriShapeHook::func = REL::Offset(0xD6BFE0).address();
				DetourTransactionBegin();
				DetourUpdateThread(GetCurrentThread());
				DetourAttach(reinterpret_cast<PVOID*>(&DrawTriShapeHook::func), reinterpret_cast<PVOID>(DrawTriShapeHook::thunk));
				DetourTransactionCommit();
				logger::info("[ShadowThreaded] instance-diag: DrawTriShape hook installed");
			}
			hooksInstalled = true;
			logger::info("[ShadowThreaded] timing-only RenderShadowmaps + Cull detours installed (mode 0)");
		}
		return;
	}
	if (!REL::Module::IsSE()) {
		logger::info("[ShadowThreaded] SE-only; not installing on this runtime");
		return;
	}
	// The BeginPass-ownership modes drive UtilityPassReplica directly (BeginPassReplica's uncovered
	// fallback calls the RenderPassImmediately trampoline; the verify mode observes the engine's
	// dispatch through OnRenderPassImmediately). Both require UtilityPassReplica's hooks, which are
	// otherwise gated behind CS_UTIL_RE_MODE -- bring them up here so the modes are self-contained.
	if (GetMode() == Mode::kOwnBeginPass || GetMode() == Mode::kOwnBeginPassVerify ||
		GetMode() == Mode::kDrawStateVerify || GetMode() == Mode::kInstance)
		UtilityPassReplica::GetSingleton()->EnsureInitialized();
	InstallHooks();
	logger::info("[ShadowThreaded] active, mode={}", static_cast<std::uint32_t>(GetMode()));
}
