#include "ShadowThreaded.h"

#include <cstring>
#include <vector>

#include "Globals.h"
#include "State.h"
#include "UtilityPassReplica.h"
#include "Utils/D3D.h"

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
	bool                 g_claiming = false;  // serial/threaded: claim covered passes for replay

	// UtilityPassReplica::ShadowCaptureHook. Stores each covered pass on the current map and (when
	// claiming) takes ownership so the inline render is skipped -- the replay renders it later.
	bool CaptureHook(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest,
		std::uint32_t a_renderFlags, bool a_canReplicate)
	{
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
}

std::int32_t ShadowThreaded::RenderShadowmapDetour(void* a1, std::int64_t a2, void* a3, std::int32_t a4, void* a_original)
{
	auto callOriginal = *static_cast<std::int32_t(**)(void*, std::int64_t, void*, std::int32_t)>(a_original);
	if (!g_claiming && GetMode() != Mode::kCapture)
		return callOriginal(a1, a2, a3, a4);

	// Open this map's pass bracket, run the original (slice alloc + RT setup + NiCamera walk; the
	// covered passes flow through CaptureHook -- stored, and skipped inline when claiming), then
	// snapshot the clean per-map render-state block (RT/viewport/depth target from the setup;
	// claimed passes did not mutate it) as the replay seed.
	g_mapWorkList.emplace_back();
	g_curMap = &g_mapWorkList.back();

	const std::int32_t r = callOriginal(a1, a2, a3, a4);

	MapWork& m = g_mapWorkList.back();
	std::memcpy(m.block, reinterpret_cast<void*>(engine::S_base.address()), engine::kBlockBytes);
	g_curMap = nullptr;
	return r;
}

void ShadowThreaded::RenderShadowmapsDetour(void* a_original)
{
	auto callOriginal = *static_cast<void (**)()>(a_original);
	const Mode m = GetMode();
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
	g_claiming = (m == Mode::kSerial);
	replica->SetShadowCaptureHook(&CaptureHook);

	callOriginal();  // per-map interceptor brackets each map; covered passes stored (+claimed if serial)

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

	// ---- Phase 0: serial deferred replay of the claimed passes, in map order ----
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

void ShadowThreaded::InstallHooks()
{
	if (hooksInstalled)
		return;

	auto* device = globals::d3d::device;
	if (device && SUCCEEDED(device->CreateDeferredContext(0, g_deferred.put())) && g_deferred) {
		Util::SetResourceName(g_deferred.get(), "ShadowThreaded::DeferredContext");
		// Count DrawIndexed on the deferred context (vtbl idx 12) for the replay log.
		auto** vtbl = *reinterpret_cast<void***>(g_deferred.get());
		g_origDeferredDrawIndexed = reinterpret_cast<DrawIndexed_t>(vtbl[12]);
		DWORD old = 0;
		if (VirtualProtect(&vtbl[12], sizeof(void*), PAGE_READWRITE, &old)) {
			vtbl[12] = reinterpret_cast<void*>(&DrawIndexedCounter);
			VirtualProtect(&vtbl[12], sizeof(void*), old, &old);
		}
	} else {
		logger::error("[ShadowThreaded] CreateDeferredContext failed; serial replay disabled");
	}

	stl::detour_thunk<RenderShadowmapsHook>(REL::RelocationID(100420, 0));
	stl::detour_thunk<RenderShadowmapHook>(REL::RelocationID(100820, 0));
	hooksInstalled = true;
	logger::info("[ShadowThreaded] detoured RenderShadowmaps @ 0x{:X}, RenderShadowmap @ 0x{:X}",
		RenderShadowmapsHook::func.address(), RenderShadowmapHook::func.address());
}

void ShadowThreaded::Setup()
{
	char buf[8] = {};
	if (GetEnvironmentVariableA("CS_SHADOW_MT", buf, sizeof(buf)) && buf[0]) {
		const int v = atoi(buf);
		if (v >= 0 && v <= 2)
			mode.store(static_cast<Mode>(v), std::memory_order_relaxed);
	}
	if (!IsActive())
		return;
	if (!REL::Module::IsSE()) {
		logger::info("[ShadowThreaded] SE-only; not installing on this runtime");
		return;
	}
	InstallHooks();
	logger::info("[ShadowThreaded] active, mode={}", static_cast<std::uint32_t>(GetMode()));
}
