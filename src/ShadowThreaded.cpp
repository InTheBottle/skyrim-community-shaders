#include "ShadowThreaded.h"

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

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

	winrt::com_ptr<ID3D11DeviceContext> g_deferred;
	// Concurrent fan-out: one deferred context per worker thread (index 0 aliases g_deferred).
	std::vector<winrt::com_ptr<ID3D11DeviceContext>> g_deferredPool;

	void ReplayOneMap(MapWork& mw);  // defined below (needs GlobalStateGuard); called per-map from the detour
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

	g_curMap = nullptr;
	return r;
}

void ShadowThreaded::RenderShadowmapsDetour(void* a_original)
{
	auto callOriginal = *static_cast<void (**)()>(a_original);
	const Mode m = GetMode();

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

	if (m == Mode::kOff || !g_deferred) {
		callOriginal();
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

	hooksInstalled = true;
	logger::info("[ShadowThreaded] detoured RenderShadowmaps @ 0x{:X}, RenderShadowmap @ 0x{:X}, BeginPass @ 0x{:X}",
		RenderShadowmapsHook::func.address(), RenderShadowmapHook::func.address(), BeginPassHook::func.address());
}

void ShadowThreaded::Setup()
{
	char buf[8] = {};
	if (GetEnvironmentVariableA("CS_SHADOW_MT", buf, sizeof(buf)) && buf[0]) {
		const int v = atoi(buf);
		if (v >= 0 && v <= 7)
			mode.store(static_cast<Mode>(v), std::memory_order_relaxed);
	}
	if (char wc[8] = {}; GetEnvironmentVariableA("CS_SHADOW_MT_WORKERS", wc, sizeof(wc)) && wc[0]) {
		const int n = atoi(wc);
		if (n >= 1 && n <= 8)
			workerCount = static_cast<std::uint32_t>(n);
	}
	if (!IsActive())
		return;
	if (!REL::Module::IsSE()) {
		logger::info("[ShadowThreaded] SE-only; not installing on this runtime");
		return;
	}
	// The BeginPass-ownership modes drive UtilityPassReplica directly (BeginPassReplica's uncovered
	// fallback calls the RenderPassImmediately trampoline; the verify mode observes the engine's
	// dispatch through OnRenderPassImmediately). Both require UtilityPassReplica's hooks, which are
	// otherwise gated behind CS_UTIL_RE_MODE -- bring them up here so the modes are self-contained.
	if (GetMode() == Mode::kOwnBeginPass || GetMode() == Mode::kOwnBeginPassVerify || GetMode() == Mode::kDrawStateVerify)
		UtilityPassReplica::GetSingleton()->EnsureInitialized();
	InstallHooks();
	logger::info("[ShadowThreaded] active, mode={}", static_cast<std::uint32_t>(GetMode()));
}
