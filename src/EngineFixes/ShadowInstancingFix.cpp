#include "ShadowInstancingFix.h"

#include <cstring>
#include <vector>

#include <d3d11_1.h>

#include "Globals.h"
#include "Vanilla/UtilityPassReplica.h"

#include "ShadowMapCache.h"

#include <RE/B/BSRenderPass.h>

// Shadow-map instancing, built on the byte-exact UtilityPassReplica. Two levels of the shadow-map
// walk are bracketed:
//   RenderShadowmaps 0x1412E3480 : once-per-frame driver.
//   RenderShadowmap  0x141305610 : once-per-map body (AddrLib 100820); snapshots each map (params +
//         clean render-state block) and brackets its passes.
//
// Shadow depth passes read ONLY per-frame globals, the pass's own light/geometry, and the 0x5D8
// render-state block -- so the block captured right after a map's original is the clean per-map
// render-target state the instanced replay seeds from.

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
	thread_local MapWork* g_curMap = nullptr;
	bool                 g_claiming = false;         // claim covered passes for the instanced replay
	bool                 g_instanceRestrict = false;  // claim ONLY the instanceable subset

	// UtilityPassReplica::ShadowCaptureHook. Stores each covered pass on the current map and (when
	// claiming) takes ownership so the inline render is skipped -- the instanced replay renders it later.
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
		// WRITE_NO_OVERWRITE + a GPU query -- leave them on the engine's inline path; depth-only shadow
		// output is order-independent so mixing is safe.
		auto* geom = a_pass->geometry;
		if (geom && *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(geom) + 0x130)) {
			++g_curMap->skinnedInline;
			return false;
		}
		// kInstance admits the instanceable subset: WHOLE TRISHAPE (geom type 3), NON-alpha-test. The
		// instanced path fills World from the per-instance stream and does NOT run the alpha-test /
		// alpha-blend geometry setup, so alpha-tested casters (foliage) and SUB_INDEX stay inline.
		if (g_instanceRestrict) {
			const bool wholeTri = geom && *(reinterpret_cast<const std::uint8_t*>(geom) + 0x150) == 3;
			if (!wholeTri || a_alphaTest) {
				++g_curMap->unsupported;
				return false;  // serial remainder (alpha-test / sub-index render inline)
			}
		}
		// Capture the map's clean render-state block at the FIRST covered pass -- while the engine's
		// RT setup is still live (DSV bound to this map's atlas slice, viewport set).
		if (g_curMap->passes.empty())
			std::memcpy(g_curMap->block, reinterpret_cast<void*>(engine::S_base.address()), engine::kBlockBytes);
		g_curMap->passes.push_back(CapturedPass{ a_pass, a_technique, a_alphaTest, a_renderFlags });
		return g_claiming;  // claim -> skip inline; replay owns it
	}

	// Per-map instanced draw. RenderShadowmap's original has returned and unbound the map's DSV, so seed
	// the map's CAPTURED render-state block (RT/DSV/viewport snapshotted at the first covered pass while
	// the DSV was live), force the main dirty word, and reset the technique caches. RenderShadowInstanced
	// groups the claimed passes by (mesh, technique) and issues one DrawIndexedInstanced per group on the
	// immediate context (the render-thread draw-count cut that is the FPS win). Finally restore the
	// engine's block+caches so the next map's engine render is undisturbed.
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

	// BSShadowLight::RenderShadowmap (0x141305610, AddrLib 100820): one invocation == one shadow map.
	// Open the pass bracket, run the engine's original (slice alloc + RT setup + NiCamera walk -- the
	// covered passes flow through CaptureHook), then issue the instanced draws for this map. Local-light
	// maps additionally stagger via ShadowMapCache: render ONE unit per frame, blit the cached slice for
	// every other unit (round-robin). renderMode is valid here (callOriginal ran Func42): 13 spot / 15 point.
	std::int32_t RenderShadowmapDetour(void* a1, std::int64_t a2, void* a3, std::int32_t a4, void* a_original)
	{
		auto callOriginal = *static_cast<std::int32_t (**)(void*, std::int64_t, void*, std::int32_t)>(a_original);

		g_mapWorkList.emplace_back();
		g_curMap = &g_mapWorkList.back();

		const std::int32_t r = callOriginal(a1, a2, a3, a4);

		if (g_curMap) {
			auto* const         desc = reinterpret_cast<const std::uint8_t*>(a2);
			const std::uint32_t target = *reinterpret_cast<const std::uint32_t*>(desc + 84);
			const std::uint32_t slice = *reinterpret_cast<const std::uint32_t*>(desc + 88);
			void* const         accum = *reinterpret_cast<void* const*>(desc + 0x48);
			const std::uint32_t rmode = accum ? *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uint8_t*>(accum) + 0x150) : 0u;
			void* const         camera = *reinterpret_cast<void* const*>(desc + 64);
			// Local-light stagger eligibility: shadow atlas (target 4), valid slice, a camera, and a
			// local render mode (13 spot / 15 point). Directional cascades (14) are camera-relative and
			// stay per-frame. EnsureCache lazily allocates the private slice mirror.
			const bool eligible = target == 4 && slice < 64 && camera &&
				(rmode == 13 || rmode == 15) && ShadowMapCache::EnsureCache();
			if (!eligible) {
				RenderMapInstanced(*g_curMap);
			} else {
				const std::int32_t* port = reinterpret_cast<const std::int32_t*>(desc + 0xD0);
				auto* const         ctx = *engine::g_immediateContext;
				auto* const         S = reinterpret_cast<std::uint32_t*>(engine::S_base.address());
				if (ShadowMapCache::ShouldRender(camera, rmode, desc)) {
					RenderMapInstanced(*g_curMap);                 // this unit's turn -> render fresh + cache
					ctx->OMSetRenderTargets(0, nullptr, nullptr);  // detach slice DSV before reading it
					ShadowMapCache::Capture(ctx, camera, slice, port);
				} else {
					ctx->OMSetRenderTargets(0, nullptr, nullptr);  // detach slice DSV before writing it
					if (!ShadowMapCache::Blit(ctx, camera, slice, port)) {
						RenderMapInstanced(*g_curMap);             // no valid cache -> render + capture
						ShadowMapCache::Capture(ctx, camera, slice, port);
					}
				}
				S[0] = 0xFFFFFFFFu;  // force the engine's next SetDirtyStates to re-bind RT/DSV/viewport
			}
		}

		g_curMap = nullptr;
		return r;
	}

	// DrawWorld::RenderShadowmaps (0x1412E3480): once-per-frame driver. Arm the capture hook, run the
	// original walk (the covered passes flow through CaptureHook; RenderShadowmapDetour issues the
	// instanced draws per map), then disarm.
	void RenderShadowmapsDetour(void* a_original)
	{
		auto callOriginal = *static_cast<void (**)()>(a_original);

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
	}

	struct RenderShadowmapsHook
	{
		static void thunk() { RenderShadowmapsDetour(&func); }
		static inline REL::Relocation<decltype(thunk)> func;
	};
	struct RenderShadowmapHook
	{
		static std::int32_t thunk(void* a1, std::int64_t a2, void* a3, std::int32_t a4)
		{
			return RenderShadowmapDetour(a1, a2, a3, a4, &func);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void ShadowInstancingFix::Install()
{
	// SE-only: every RE'd offset in this subsystem is 1.5.97.
	if (!REL::Module::IsSE()) {
		logger::info("[ShadowInstancingFix] SE-only; not installing on this runtime");
		return;
	}

	// The instancing draw path drives UtilityPassReplica (its RenderPassImmediately detour is the seam
	// CaptureHook rides). Bring it up before the shadow-map detours can fire.
	UtilityPassReplica::GetSingleton()->EnsureInitialized();

	stl::detour_thunk<RenderShadowmapsHook>(REL::RelocationID(100420, 0));
	stl::detour_thunk<RenderShadowmapHook>(REL::RelocationID(100820, 0));

	logger::info("[ShadowInstancingFix] detoured RenderShadowmaps @ 0x{:X}, RenderShadowmap @ 0x{:X}",
		RenderShadowmapsHook::func.address(), RenderShadowmapHook::func.address());
}
