#include "ShadowMapCacheHooks.h"

#include <cstring>
#include <vector>

#include <d3d11_1.h>

#include "Globals.h"
#include "State.h"
#include "UtilityPassReplica.h"

#include "ShadowMapCache.h"

#include <RE/B/BSRenderPass.h>

// Static-under-dynamic local-light shadow cache, built on the UtilityPassReplica seam. NO instancing:
// casters are replayed one DrawIndexed each through the engine's OWN RenderPassImmediately (RenderPassesOriginal),
// byte-for-byte the draws it would have issued inline. On a local-light map both static and dynamic casters are
// claimed (skipped inline) so we own the render order: a cached STATIC BASE is laid down (blit, or render fresh +
// capture on a static-set change), then the DYNAMICS are replayed ON TOP each frame, depth-tested against the
// base. Static furniture is captured once + blitted forever; actors/trees move every frame. Two levels bracket:
//   RenderShadowmaps 0x1412E3480 : once-per-frame driver -- arms the capture hook.
//   RenderShadowmap  0x141305610 : once-per-map body (AddrLib 100820); snapshots the clean render-state block,
//         then lays the static base + replays the dynamics on top.
//
// Shadow depth passes read ONLY per-frame globals, the pass's own light/geometry, and the 0x5D8 render-state
// block -- so the block captured mid-walk is the clean per-map render-target state the replay reseeds from.
// The engine's own depth-test state (seeded from the block) merges the dynamics with the static base, so no
// hand-picked composite direction is needed and the result matches a fully-inline render.

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
	// replay) + the claimed casters split into STATIC and DYNAMIC lists. On a local-light map both are
	// claimed (skipped inline) so RenderShadowmapDetour fully controls the render order: cached static base
	// first, then dynamics on top. On a non-local map (directional cascade) only statics are claimed;
	// dynamics render inline on the engine path as before.
	struct MapWork
	{
		std::uint8_t              block[engine::kBlockBytes];
		bool                      blockCaptured = false;
		bool                      claimDynamics = false;  // local-light map (target 4): claim dynamics too
		std::vector<CapturedPass> staticPasses;           // non-skinned, non-tree -> cached base
		std::vector<CapturedPass> dynamicPasses;          // skinned actors / TreeAnim -> rendered on top each frame
		std::uint64_t             staticSig = 0;           // signature of the static caster set (change detection)
	};

	std::vector<MapWork> g_mapWorkList;
	thread_local MapWork* g_curMap = nullptr;
	bool                 g_claiming = false;  // claim casters (skip inline) so the cache/replay owns them

	// Capture the map's clean render-state block at the FIRST claimed pass -- while the engine's RT setup is
	// still live (DSV bound to this map's atlas slice, viewport set). Any pass (static or dynamic) will do.
	inline void CaptureBlock()
	{
		if (!g_curMap->blockCaptured) {
			std::memcpy(g_curMap->block, reinterpret_cast<void*>(engine::S_base.address()), engine::kBlockBytes);
			g_curMap->blockCaptured = true;
		}
	}

	// UtilityPassReplica::ShadowCaptureHook. Sorts each caster into the current map's static / dynamic list and
	// (when claiming) takes ownership so the engine's inline draw is skipped -- RenderPasses replays it later
	// via the engine's own RenderPassImmediately. On a non-local map, dynamics are left on the engine's inline
	// path (claimDynamics == false) so cascade behavior is unchanged.
	bool CaptureHook(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest,
		std::uint32_t a_renderFlags, bool /*a_canReplicate*/)
	{
		if (!g_curMap)
			return false;
		auto*      geom = a_pass->geometry;
		const bool tree = (a_technique & (1u << 26)) != 0;  // TreeAnim sway -> dynamic
		// Skinned casters (geom+0x130 != 0) are dynamic actors (player, NPCs).
		const bool skinned = geom && *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(geom) + 0x130);
		if (tree || skinned) {
			// DYNAMIC caster: rendered fresh every frame. On a local-light map claim it so it can be replayed
			// ON TOP of the cached static base (RenderShadowmapDetour); elsewhere leave it inline.
			if (!g_curMap->claimDynamics)
				return false;
			CaptureBlock();
			g_curMap->dynamicPasses.push_back(CapturedPass{ a_pass, a_technique, a_alphaTest, a_renderFlags });
			return g_claiming;
		}
		if (!geom)
			return false;  // non-geometry utility pass: leave it on the engine's inline path
		// STATIC caster. Fold its placed-reference identity + FULL world transform into the map's change
		// signature so a static that is placed / disabled / moved / rotated re-renders the base; otherwise the
		// base is blitted forever. Addition is order-independent (robust to per-frame walk reordering) and
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
		CaptureBlock();
		g_curMap->staticPasses.push_back(CapturedPass{ a_pass, a_technique, a_alphaTest, a_renderFlags });
		return g_claiming;  // claim -> skip inline; RenderPasses replays it
	}

	// Replay a claimed caster list into the current map's slice. RenderShadowmap's original has returned, so
	// seed the map's CAPTURED render-state block (RT/DSV/viewport snapshotted at the first claimed pass while
	// the DSV was live), force the main dirty word, and reset the technique caches. RenderPassesOriginal then
	// replays each pass through the engine's own RenderPassImmediately (one DrawIndexed each -- no instancing).
	// Finally restore the engine's block + caches so the next map's engine render is undisturbed. The passes
	// are depth-tested against whatever is already in the slice, so replaying the DYNAMIC list after a static
	// base leaves the nearer-to-light depth (dynamics correctly occlude / are occluded by the cached statics).
	void RenderPasses(const std::vector<CapturedPass>& a_passes, const std::uint8_t* a_block)
	{
		if (a_passes.empty())
			return;
		auto* const S = reinterpret_cast<std::uint8_t*>(engine::S_base.address());
		auto* const sflags = reinterpret_cast<std::uint32_t*>(S);

		static std::vector<std::uint8_t> s_saved(engine::kBlockBytes);
		std::memcpy(s_saved.data(), S, engine::kBlockBytes);
		const auto  savedTech = *engine::g_currentTechnique;
		auto* const savedShader = *engine::g_currentShader;
		auto* const savedMaterial = *engine::g_currentMaterial;

		std::memcpy(S, a_block, engine::kBlockBytes);
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
		s_passes.reserve(a_passes.size());
		s_techs.reserve(a_passes.size());
		s_alphas.reserve(a_passes.size());
		s_flags.reserve(a_passes.size());
		for (const auto& cp : a_passes) {
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
	//
	// STATIC-UNDER-DYNAMIC. A local-light map's STATIC casters (furniture, walls) are cached ONCE and reused;
	// its DYNAMIC casters (actors, swaying trees) are rendered ON TOP every frame, depth-tested against the
	// static base so they occlude / are occluded correctly. Both are claimed (skipped inline) so we control the
	// order: after the engine's original clears the slice, we (a) lay down the static base -- blit the cached
	// depth, or on a static-set change render the statics fresh and copy them into the cache -- then (b) replay
	// the dynamics on top via the engine's own RenderPassImmediately. The static base is re-rendered ONLY when
	// the static signature changes (a static placed / disabled / moved), so in gameplay it is captured once and
	// blitted forever while actors keep moving. renderMode is valid post-walk (callOriginal ran Func42): 13
	// spot / 15 point. Non-local maps (directional cascades) claim only statics; their dynamics stayed inline.
	std::int32_t RenderShadowmapDetour(void* a1, std::int64_t a2, void* a3, std::int32_t a4, void* a_original)
	{
		auto callOriginal = *static_cast<std::int32_t (**)(void*, std::int64_t, void*, std::int32_t)>(a_original);

		g_mapWorkList.emplace_back();
		g_curMap = &g_mapWorkList.back();

		// Pre-walk: claim dynamics only for the local-light shadow atlas (target 4). target is set by the caller
		// before RenderShadowmap and is valid here (unlike rmode, which Func42 populates during the walk). On a
		// cascade (target != 4) dynamics render inline, so cascade behavior is unchanged.
		g_curMap->claimDynamics = *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::uint8_t*>(a2) + 84) == 4;

		const std::int32_t r = callOriginal(a1, a2, a3, a4);

		if (g_curMap) {
			auto* const         desc = reinterpret_cast<const std::uint8_t*>(a2);
			const std::uint32_t target = *reinterpret_cast<const std::uint32_t*>(desc + 84);
			const std::uint32_t slice = *reinterpret_cast<const std::uint32_t*>(desc + 88);
			void* const         accum = *reinterpret_cast<void* const*>(desc + 0x48);
			const std::uint32_t rmode = accum ? *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uint8_t*>(accum) + 0x150) : 0u;
			void* const         camera = *reinterpret_cast<void* const*>(desc + 64);
			// Cache eligibility: shadow atlas (target 4), valid slice, a camera, and a local render mode (13 spot
			// / 15 point). Directional cascades (14) are camera-relative and stay per-frame.
			const bool          localMap = target == 4 && slice < 64 && camera && (rmode == 13 || rmode == 15);
			const std::int32_t* port = reinterpret_cast<const std::int32_t*>(desc + 0xD0);
			auto* const         ctx = *engine::g_immediateContext;
			auto* const         S = reinterpret_cast<std::uint32_t*>(engine::S_base.address());
			const std::uint64_t sig = g_curMap->staticSig;  // folded over the map's static casters during the walk
			const bool          eligible = localMap && ShadowMapCache::EnsureCache();

			// (a) STATIC BASE into the (engine-cleared) slice.
			if (eligible) {
				if (ShadowMapCache::ShouldRender(camera, rmode, desc, sig)) {
					RenderPasses(g_curMap->staticPasses, g_curMap->block);  // static set changed -> render fresh
					ctx->OMSetRenderTargets(0, nullptr, nullptr);           // detach slice DSV before reading it
					ShadowMapCache::Capture(ctx, camera, slice, port);      // copy the clean static base into the cache
				} else {
					ctx->OMSetRenderTargets(0, nullptr, nullptr);  // detach slice DSV before writing it
					if (!ShadowMapCache::Blit(ctx, camera, slice, port)) {
						RenderPasses(g_curMap->staticPasses, g_curMap->block);  // no valid cache -> render + capture
						ctx->OMSetRenderTargets(0, nullptr, nullptr);
						ShadowMapCache::Capture(ctx, camera, slice, port);
					}
				}
			} else {
				// Non-local / not-yet-cacheable: replay the claimed statics (cascade dynamics stayed inline).
				RenderPasses(g_curMap->staticPasses, g_curMap->block);
			}
			// (b) DYNAMICS ON TOP, depth-tested against the static base (nearer-to-light wins). Claimed only on
			// the local atlas (claimDynamics == target 4); a no-op elsewhere, where dynamics rendered inline.
			if (g_curMap->claimDynamics)
				RenderPasses(g_curMap->dynamicPasses, g_curMap->block);
			S[0] = 0xFFFFFFFFu;  // force the engine's next SetDirtyStates to re-bind RT/DSV/viewport
		}

		g_curMap = nullptr;
		return r;
	}

	// DrawWorld::RenderShadowmaps (0x1412E3480): once-per-frame driver. Arm the capture hook, run the original
	// walk (covered passes flow through CaptureHook; RenderShadowmapDetour handles each map), then disarm.
	void RenderShadowmapsDetour(void* a_original)
	{
		auto callOriginal = *static_cast<void (**)()>(a_original);

		// Advance the cache clock at the start of the shadow phase (this IS the once-per-frame shadow driver).
		ShadowMapCache::BeginFrame();

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

void ShadowMapCacheHooks::Install()
{
	// SE-only: every RE'd offset in this subsystem is 1.5.97.
	if (!REL::Module::IsSE()) {
		logger::info("[ShadowMapCache] SE-only; not installing on this runtime");
		return;
	}

	// The replay path drives UtilityPassReplica (its RenderPassImmediately detour is the seam CaptureHook
	// rides, and its trampoline is the engine's original per-pass render). Bring it up before the shadow-map
	// detours can fire.
	UtilityPassReplica::GetSingleton()->EnsureInitialized();

	stl::detour_thunk<RenderShadowmapsHook>(REL::RelocationID(100420, 0));
	stl::detour_thunk<RenderShadowmapHook>(REL::RelocationID(100820, 0));

	logger::info("[ShadowMapCache] detoured RenderShadowmaps @ 0x{:X}, RenderShadowmap @ 0x{:X}",
		RenderShadowmapsHook::func.address(), RenderShadowmapHook::func.address());
}
