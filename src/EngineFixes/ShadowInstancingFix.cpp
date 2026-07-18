#include "ShadowInstancingFix.h"

#include <cstring>
#include <vector>

#include <d3d11_1.h>

#include "Globals.h"
#include "State.h"
#include "UtilityPassReplica.h"

#include "ShadowMapCache.h"

#include <RE/B/BSRenderPass.h>

// Static local-light shadow cache, built on the byte-exact UtilityPassReplica seam. NO instancing:
// static shadow casters are replayed one DrawIndexed each through the engine's OWN RenderPassImmediately
// (RenderPassesOriginal), byte-for-byte the draws it would have issued inline. The cache's only job is to
// SKIP that static replay on frames where the static caster set has not changed -- it blits the previously
// captured depth slice instead. Two levels of the shadow-map walk are bracketed:
//   RenderShadowmaps 0x1412E3480 : once-per-frame driver -- arms the capture hook.
//   RenderShadowmap  0x141305610 : once-per-map body (AddrLib 100820); snapshots each map (params + clean
//         render-state block) and, per map, either blits the cached static slice or replays the statics.
//
// Shadow depth passes read ONLY per-frame globals, the pass's own light/geometry, and the 0x5D8 render-state
// block -- so the block captured right after a map's original is the clean per-map render-target state the
// replay reseeds from. Depth-only shadow output is order-independent, so replaying claimed statics after the
// engine rendered this map's inline dynamics yields the same slice as a fully-inline render.

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

	// One shadow map's replay unit: the clean per-map render-state block (RT/viewport/depth, seeds the
	// replay) + the ordered claimed-static list. Dynamic casters (skinned actors / swaying trees) render
	// inline on the engine path during the walk and are only counted -- their presence makes the map
	// non-cacheable (a blit would freeze their animated shadow).
	struct MapWork
	{
		std::uint8_t              block[engine::kBlockBytes];
		std::vector<CapturedPass> passes;
		std::uint64_t             skinnedInline = 0;  // skinned casters rendered inline -> map is DYNAMIC
		std::uint64_t             treeInline = 0;     // TreeAnim casters (sway) rendered inline -> map is DYNAMIC
		std::uint64_t             staticSig = 0;      // signature of the claimed static caster set (change detection)
	};

	std::vector<MapWork> g_mapWorkList;
	thread_local MapWork* g_curMap = nullptr;
	bool                 g_claiming = false;  // claim static casters (skip inline) so the cache/replay owns them

	// UtilityPassReplica::ShadowCaptureHook. Records each STATIC caster on the current map and (when claiming)
	// takes ownership so the engine's inline draw is skipped -- RenderMapOriginal replays it later via the
	// engine's own RenderPassImmediately. Dynamic casters (skinned / TreeAnim) are counted and left inline.
	bool CaptureHook(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest,
		std::uint32_t a_renderFlags, bool /*a_canReplicate*/)
	{
		if (!g_curMap)
			return false;
		auto*      geom = a_pass->geometry;
		const bool tree = (a_technique & (1u << 26)) != 0;
		// Skinned casters (geom+0x130 != 0) are dynamic actors (player, NPCs).
		const bool skinned = geom && *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(geom) + 0x130);
		// DYNAMIC casters render inline every frame AND make the map non-cacheable. TreeAnim casters (technique
		// bit 1u<<26) sway every frame; skinned casters animate. Count them (a cached blit would freeze the
		// animated shadow) and leave them on the engine's inline path.
		if (tree) {
			++g_curMap->treeInline;
			return false;
		}
		if (skinned) {
			++g_curMap->skinnedInline;
			return false;
		}
		if (!geom)
			return false;  // non-geometry utility pass: leave it on the engine's inline path
		// STATIC caster. Fold its placed-reference identity + FULL world transform into the map's change
		// signature so a static that is placed / disabled / moved / rotated re-renders the slice; otherwise the
		// slice is blitted forever. Addition is order-independent (robust to per-frame walk reordering) and
		// non-cancelling (identical co-located sub-shapes do not XOR away).
		std::uint64_t h = 0xcbf29ce484222325ull;
		auto          mix = [&](std::uint64_t v) { h = (h ^ v) * 0x100000001b3ull; };
		mix(reinterpret_cast<std::uint64_t>(geom));                 // per-sub-shape identity (co-located siblings differ)
		mix(reinterpret_cast<std::uint64_t>(geom->GetUserData()));  // placed-reference identity
		std::uint32_t b[13];                                        // NiTransform: rotate 3x3 + translate 3 + scale
		std::memcpy(b, &geom->world, sizeof(b));
		for (std::uint32_t v : b)
			mix(v);
		g_curMap->staticSig += h;
		// Capture the map's clean render-state block at the FIRST claimed static -- while the engine's RT setup
		// is still live (DSV bound to this map's atlas slice, viewport set).
		if (g_curMap->passes.empty())
			std::memcpy(g_curMap->block, reinterpret_cast<void*>(engine::S_base.address()), engine::kBlockBytes);
		g_curMap->passes.push_back(CapturedPass{ a_pass, a_technique, a_alphaTest, a_renderFlags });
		return g_claiming;  // claim -> skip inline; RenderMapOriginal replays it
	}

	// Per-map static replay. RenderShadowmap's original has returned and unbound the map's DSV, so seed the
	// map's CAPTURED render-state block (RT/DSV/viewport snapshotted at the first claimed static while the DSV
	// was live), force the main dirty word, and reset the technique caches. RenderPassesOriginal then replays
	// each claimed static through the engine's own RenderPassImmediately (one DrawIndexed each -- no instancing).
	// Finally restore the engine's block + caches so the next map's engine render is undisturbed.
	void RenderMapOriginal(MapWork& mw)
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
		sflags[0] = 0xFFFFFFFFu;  // force first pass's SetDirtyStates to re-bind RT/DSV/viewport
		*engine::g_currentTechnique = 0;
		*engine::g_currentShader = nullptr;
		*engine::g_currentMaterial = nullptr;

		static std::vector<RE::BSRenderPass*> s_passes;
		static std::vector<std::uint32_t>     s_techs;
		static std::vector<std::uint8_t>      s_alphas;
		static std::vector<std::uint32_t>     s_flags;
		s_passes.clear();
		s_techs.clear();
		s_alphas.clear();
		s_flags.clear();
		s_passes.reserve(mw.passes.size());
		s_techs.reserve(mw.passes.size());
		s_alphas.reserve(mw.passes.size());
		s_flags.reserve(mw.passes.size());
		for (const auto& cp : mw.passes) {
			s_passes.push_back(cp.pass);
			s_techs.push_back(cp.technique);
			s_alphas.push_back(cp.alphaTest ? 1u : 0u);
			s_flags.push_back(cp.renderFlags);
		}
		UtilityPassReplica::GetSingleton()->RenderPassesOriginal(
			s_passes.data(), s_techs.data(), s_alphas.data(), s_flags.data(), static_cast<std::uint32_t>(s_passes.size()));

		std::memcpy(S, s_saved.data(), engine::kBlockBytes);
		*engine::g_currentTechnique = savedTech;
		*engine::g_currentShader = savedShader;
		*engine::g_currentMaterial = savedMaterial;
		sflags[0] = 0xFFFFFFFFu;  // engine's next SetDirtyStates re-binds onto whatever we left bound
	}

	// BSShadowLight::RenderShadowmap (0x141305610, AddrLib 100820): one invocation == one shadow map.
	// Run the engine's original (slice alloc + RT setup + NiCamera walk -- the covered passes flow through
	// CaptureHook, static casters claimed + skipped inline), then either BLIT the cached static slice (static
	// set unchanged) or replay the claimed statics fresh + capture. Local-light maps that drew a dynamic
	// caster this frame are never cached (rendered fully fresh). renderMode is valid here (callOriginal ran
	// Func42): 13 spot / 15 point.
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
			// Local-light cache eligibility: shadow atlas (target 4), valid slice, a camera, and a local render
			// mode (13 spot / 15 point). Directional cascades (14) are camera-relative and stay per-frame.
			//
			// STATIC/DYNAMIC SPLIT: a map that drew any DYNAMIC caster this frame -- a skinned actor OR a swaying
			// tree -- is rendered fully fresh (statics replayed over the inline dynamics) and never cached, or
			// those shadows would freeze. Only STATIC-ONLY local lights cache; the cached slice then holds clean
			// static-only depth (light-space, camera-independent) and is re-rendered ONLY when the static caster
			// set's signature changes -- otherwise it is blitted forever.
			const bool          localMap = target == 4 && slice < 64 && camera && (rmode == 13 || rmode == 15);
			const std::int32_t* port = reinterpret_cast<const std::int32_t*>(desc + 0xD0);
			auto* const         ctx = *engine::g_immediateContext;
			auto* const         S = reinterpret_cast<std::uint32_t*>(engine::S_base.address());
			const std::uint64_t sig = g_curMap->staticSig;  // folded over the map's static casters during the walk
			const bool          hasDynamic = g_curMap->skinnedInline > 0 || g_curMap->treeInline > 0;
			const bool          eligible = localMap && !hasDynamic && ShadowMapCache::EnsureCache();

			if (!eligible) {
				RenderMapOriginal(*g_curMap);  // replay claimed statics (over inline dynamics, if any)
			} else {
				if (ShadowMapCache::ShouldRender(camera, rmode, desc, sig)) {
					RenderMapOriginal(*g_curMap);                  // static set changed -> render fresh + cache
					ctx->OMSetRenderTargets(0, nullptr, nullptr);  // detach slice DSV before reading it
					ShadowMapCache::Capture(ctx, camera, slice, port);
				} else {
					ctx->OMSetRenderTargets(0, nullptr, nullptr);  // detach slice DSV before writing it
					if (!ShadowMapCache::Blit(ctx, camera, slice, port)) {
						RenderMapOriginal(*g_curMap);              // no valid cache -> render + capture
						ShadowMapCache::Capture(ctx, camera, slice, port);
					}
				}
				S[0] = 0xFFFFFFFFu;  // force the engine's next SetDirtyStates to re-bind RT/DSV/viewport
			}
		}

		g_curMap = nullptr;
		return r;
	}

	// DrawWorld::RenderShadowmaps (0x1412E3480): once-per-frame driver. Arm the capture hook, run the original
	// walk (covered passes flow through CaptureHook; RenderShadowmapDetour handles each map), then disarm.
	void RenderShadowmapsDetour(void* a_original)
	{
		auto callOriginal = *static_cast<void (**)()>(a_original);

		auto* const replica = UtilityPassReplica::GetSingleton();
		g_mapWorkList.clear();
		g_mapWorkList.reserve(24);
		g_curMap = nullptr;
		g_claiming = true;
		replica->SetShadowCaptureHook(&CaptureHook);
		callOriginal();  // per map: claims static passes; RenderShadowmapDetour blits or replays
		replica->SetShadowCaptureHook(nullptr);
		g_claiming = false;
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

	// The replay path drives UtilityPassReplica (its RenderPassImmediately detour is the seam CaptureHook
	// rides, and its trampoline is the engine's original per-pass render). Bring it up before the shadow-map
	// detours can fire.
	UtilityPassReplica::GetSingleton()->EnsureInitialized();

	stl::detour_thunk<RenderShadowmapsHook>(REL::RelocationID(100420, 0));
	stl::detour_thunk<RenderShadowmapHook>(REL::RelocationID(100820, 0));

	logger::info("[ShadowInstancingFix] detoured RenderShadowmaps @ 0x{:X}, RenderShadowmap @ 0x{:X}",
		RenderShadowmapsHook::func.address(), RenderShadowmapHook::func.address());
}
