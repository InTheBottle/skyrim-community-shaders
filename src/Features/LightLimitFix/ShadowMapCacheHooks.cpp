#include "ShadowMapCacheHooks.h"

#include <cstring>
#include <unordered_map>
#include <vector>

#include <d3d11_1.h>

#include "Globals.h"
#include "State.h"
#include "UtilityPassReplica.h"

#include "ShadowMapCache.h"

#include <RE/B/BSFadeNode.h>
#include <RE/B/BSGeometry.h>
#include <RE/B/BSRenderPass.h>
#include <RE/B/BSShaderProperty.h>
#include <RE/B/BSXFlags.h>
#include <RE/N/NiNode.h>

// Static-under-dynamic local-light shadow cache, built on the UtilityPassReplica seam. NO instancing. On a
// local-light map, DYNAMIC casters (skinned actors, wind-animated trees, havok/animated/scriptable objects,
// anything that has moved) render INLINE on the engine's own path -- so their live pose/position is always
// correct. Only the completely-STATIC casters are claimed (skipped inline); they are rendered once through the
// engine's OWN RenderPassImmediately (RenderPassesOriginal) and captured into a private depth layer, then that
// cached static depth is COMPOSITED under the live dynamics each frame (a fullscreen SV_Depth min-depth pass).
// So static furniture is drawn once and reused forever while actors keep moving. The base regenerates only when
// the static set changes; that (rare) capture frame suppresses the dynamics so the copied base is clean static.
// Two levels bracket:
//   RenderShadowmaps 0x1412E3480 : once-per-frame driver -- arms the capture hook.
//   RenderShadowmap  0x141305610 : once-per-map body (AddrLib 100820); composites / captures the static base.
//
// Shadow depth passes read ONLY per-frame globals, the pass's own light/geometry, and the 0x5D8 render-state
// block -- so the block captured mid-walk is the clean per-map render-target state the static render reseeds from.

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

	// One shadow map's replay unit: the clean per-map render-state block (RT/viewport/depth, seeds the replay)
	// + the claimed COMPLETELY-STATIC casters. On a local-light map the statics are claimed (skipped inline) so
	// they can be cached; dynamic casters (actors, swaying trees, MOVING statics) render INLINE on the engine's
	// own path -- their live pose/position is correct, and the cached static depth is composited UNDER them
	// post-walk. On a capture frame the dynamics are instead suppressed so the captured base is clean static.
	struct MapWork
	{
		std::uint8_t              block[engine::kBlockBytes];
		bool                      blockCaptured = false;
		bool                      isLocal = false;      // local-light shadow atlas (target 4): claim + cache statics
		bool                      captureMode = false;  // this frame: suppress dynamics to capture a clean static base
		bool                      classChanged = false;  // a caster's static/dynamic verdict flipped -> re-capture
		std::vector<CapturedPass> staticPasses;         // completely-static casters -> cached base
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

	// Per-caster classification history, keyed by geometry pointer. Stores the FIXED capability verdict (computed
	// once), the per-frame transform for movement detection, and the last dynamic/static verdict so a FLIP
	// (something started / stopped moving) can invalidate the cached base -- while cull-set churn (a caster that
	// merely enters / leaves a frame's walk without moving) does NOT.
	struct CasterHist
	{
		std::uint64_t xform = 0;
		std::uint32_t frame = 0;
		bool          moved = false;
		bool          classified = false;      // capability computed yet?
		bool          dynamicCapable = false;  // fixed: can this object animate / havok / be scripted?
		bool          verdict = false;         // this frame's dynamic verdict (reused across maps in one frame)
		bool          verdictKnown = false;    // has a prior-frame verdict to compare against?
		bool          flipped = false;         // verdict changed vs the previous frame it was walked
	};
	std::unordered_map<const void*, CasterHist> g_casterHist;
	std::uint32_t                               g_frameCounter = 0;

	// Diagnostics (rate-limited log): per-frame classifier + action tallies to see why the cache misbehaves.
	std::uint32_t g_dbgStatic = 0, g_dbgDynamic = 0, g_dbgCapability = 0, g_dbgMoved = 0, g_dbgFlip = 0;
	std::uint32_t g_dbgMaps = 0, g_dbgCapture = 0, g_dbgComposite = 0, g_dbgFull = 0;

	inline std::uint64_t XformHash(const RE::BSGeometry* a_geom)
	{
		std::uint64_t h = 0xcbf29ce484222325ull;
		std::uint32_t b[13];  // NiTransform: rotate 3x3 + translate 3 + scale
		std::memcpy(b, &a_geom->world, sizeof(b));
		for (std::uint32_t v : b)
			h = (h ^ v) * 0x100000001b3ull;
		return h;
	}

	// Fixed CAPABILITY test (computed once per geometry): can this object EVER animate / have havok / be script-
	// toggled? Reuses Skylighting's static classifier -- the skinned shader flag + the BSX flags on the owning
	// FadeNode (kDynamic / kRagdoll / kAddon / kNeedsTransformUpdate / kBreakable / kLights / ...). This makes a
	// physics object AT REST, a closed door, a script-disable-able static, an animated static, etc. all count as
	// dynamic even before they first move -- so they render live inline and are never baked into the cached base.
	bool ComputeDynamicCapable(RE::BSRenderPass* a_pass, RE::BSGeometry* a_geom)
	{
		if (auto* prop = a_pass->shaderProperty) {
			using enum RE::BSShaderProperty::EShaderPropertyFlag;
			if (prop->flags.any(kSkinned))
				return true;
		}
		RE::BSFadeNode* fadeNode = nullptr;
		for (RE::NiNode* p = a_geom->parent; p && !fadeNode; p = p->parent)
			fadeNode = p->AsFadeNode();
		if (fadeNode) {
			if (auto* extra = fadeNode->GetExtraData("BSX")) {
				const auto      v = static_cast<std::int32_t>(static_cast<RE::BSXFlags*>(extra)->value);
				using Flag = RE::BSXFlags::Flag;
				constexpr std::int32_t kDyn =
					static_cast<std::int32_t>(Flag::kRagdoll) |
					static_cast<std::int32_t>(Flag::kEditorMarker) |
					static_cast<std::int32_t>(Flag::kDynamic) |
					static_cast<std::int32_t>(Flag::kAddon) |
					static_cast<std::int32_t>(Flag::kNeedsTransformUpdate) |
					static_cast<std::int32_t>(Flag::kMagicShaderParticles) |
					static_cast<std::int32_t>(Flag::kLights) |
					static_cast<std::int32_t>(Flag::kBreakable) |
					static_cast<std::int32_t>(Flag::kSearchedBreakable);
				if (v & kDyn)
					return true;
			}
		}
		return false;
	}

	// A caster is DYNAMIC (never cached; rendered live inline each frame) if it is skinned, wind-animated
	// (TreeAnim), CAPABLE of animating / havok / being scripted (BSX flags), OR has moved since last frame. Only
	// completely-static casters form the cached base. When a caster's verdict FLIPS (a static starts / stops
	// moving), it flags the current map so the base is re-captured -- but a caster merely entering / leaving a
	// frame's shadow-cull walk WITHOUT moving does NOT flip, so cull-set churn never triggers a re-capture.
	bool IsDynamicCaster(RE::BSRenderPass* a_pass, RE::BSGeometry* a_geom, std::uint32_t a_technique)
	{
		if (a_technique & (1u << 26))  // TreeAnim wind
			return true;
		if (a_geom && *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(a_geom) + 0x130))  // skinned
			return true;
		if (!a_geom)
			return false;
		auto& h = g_casterHist[a_geom];
		if (!h.classified) {
			h.classified = true;
			h.dynamicCapable = ComputeDynamicCapable(a_pass, a_geom);
		}
		if (h.frame != g_frameCounter) {  // first walk this frame -- recompute movement + verdict + flip
			bool verdict;
			if (h.dynamicCapable) {
				verdict = true;
				++g_dbgCapability;
			} else {
				const std::uint64_t xf = XformHash(a_geom);
				h.moved = h.xform != xf;
				h.xform = xf;
				verdict = h.moved;
				if (verdict)
					++g_dbgMoved;
			}
			h.flipped = h.verdictKnown && h.verdict != verdict;  // started / stopped being dynamic
			if (h.flipped)
				++g_dbgFlip;
			h.verdict = verdict;
			h.verdictKnown = true;
			h.frame = g_frameCounter;
		}
		// Flag EVERY map the flipped caster appears in this frame (its base must drop / add the caster).
		if (h.flipped && g_curMap)
			g_curMap->classChanged = true;
		return h.verdict;
	}

	// UtilityPassReplica::ShadowCaptureHook. On a local-light map, claims the COMPLETELY-STATIC casters (skip
	// inline) so they can be cached, and leaves DYNAMIC casters (skinned actors, swaying trees, MOVING statics)
	// on the engine's INLINE path so their live pose/position renders correctly -- the cached static depth is
	// composited UNDER them post-walk. On a CAPTURE frame the dynamics are instead claimed-and-suppressed so the
	// captured base is clean static. Non-local maps (cascades) claim nothing -> the engine renders everything.
	bool CaptureHook(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest,
		std::uint32_t a_renderFlags, bool /*a_canReplicate*/)
	{
		if (!g_curMap || !g_curMap->isLocal)
			return false;  // cascades / non-local: the engine renders every caster inline (unchanged)
		auto* geom = a_pass->geometry;
		if (IsDynamicCaster(a_pass, geom, a_technique)) {
			++g_dbgDynamic;
			if (g_curMap->captureMode)
				return g_claiming;  // capture frame: claim + skip so the captured base is clean static (no inline draw)
			return false;           // normal frame: render inline via the engine (correct LIVE pose/position)
		}
		if (!geom)
			return false;  // non-geometry utility pass: leave it on the engine's inline path
		++g_dbgStatic;
		// COMPLETELY STATIC caster -> claim it (skip inline) so RenderPasses can render it into the cached base.
		// Invalidation is driven by verdict FLIPS (IsDynamicCaster), not by folding the caster set, so cull churn
		// does not trigger re-captures.
		CaptureBlock();
		g_curMap->staticPasses.push_back(CapturedPass{ a_pass, a_technique, a_alphaTest, a_renderFlags });
		return g_claiming;  // claim -> skip inline; RenderPasses renders it into the cached base
	}

	// Render a claimed STATIC caster list into the current map's slice. RenderShadowmap's original has returned,
	// so seed the map's CAPTURED render-state block (RT/DSV/viewport snapshotted at the first claimed pass while
	// the DSV was live), force the main dirty word, and reset the technique caches. RenderPassesOriginal then
	// draws each pass through the engine's own RenderPassImmediately (one DrawIndexed each -- no instancing).
	// Finally restore the engine's block + caches so the next map's engine render is undisturbed. Used to render
	// the static base fresh (capture / full-render); dynamics are NEVER routed here -- they render live inline.
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
	// STATIC-UNDER-DYNAMIC. A local-light map's completely-STATIC casters (furniture, walls) are cached ONCE and
	// reused; its DYNAMIC casters (actors, swaying trees, MOVING statics) render INLINE on the engine's own path
	// each frame -- so their live pose/position is always correct -- and the cached static depth is composited
	// UNDER them. Only the statics are claimed (skipped inline). Post-walk, per map:
	//   Composite   : static set unchanged -> the slice already holds the live inline dynamics; composite the
	//                 cached static depth under them (the win: statics cost nothing).
	//   StaticCapture: a capture frame -- dynamics were suppressed this frame, so the slice is clean static ->
	//                 render the statics + copy them into the cache. (Rare: only when the static set changed.)
	//   RenderFull  : new light / no cache yet -> render the statics over the inline dynamics for a correct
	//                 display while a capture is scheduled for next frame.
	// The static base regenerates ONLY when the static signature changes (a static placed / disabled / moved),
	// so in gameplay it is captured once and reused forever while actors keep moving. renderMode is valid
	// post-walk (Func42 ran): 13 spot / 15 point. Non-local maps (cascades) claim nothing -> engine renders all.
	std::int32_t RenderShadowmapDetour(void* a1, std::int64_t a2, void* a3, std::int32_t a4, void* a_original)
	{
		auto callOriginal = *static_cast<std::int32_t (**)(void*, std::int64_t, void*, std::int32_t)>(a_original);

		g_mapWorkList.emplace_back();
		g_curMap = &g_mapWorkList.back();

		// Pre-walk: is this the local-light shadow atlas (target 4)? target is set by the caller before
		// RenderShadowmap and is valid here (unlike rmode, which Func42 populates during the walk). captureMode =
		// this light was flagged last frame to (re)build its static base -> suppress dynamics so the capture is
		// clean static. camera (desc+64) is stale-stable pre-walk -- correct for the persistent-light key.
		{
			auto* const preDesc = reinterpret_cast<const std::uint8_t*>(a2);
			void* const preCam = *reinterpret_cast<void* const*>(preDesc + 64);
			g_curMap->isLocal = *reinterpret_cast<const std::uint32_t*>(preDesc + 84) == 4;
			g_curMap->captureMode = g_curMap->isLocal && preCam && ShadowMapCache::NeedsStaticCapture(preCam);
		}

		const std::int32_t r = callOriginal(a1, a2, a3, a4);

		if (g_curMap && g_curMap->isLocal) {
			auto* const         desc = reinterpret_cast<const std::uint8_t*>(a2);
			const std::uint32_t target = *reinterpret_cast<const std::uint32_t*>(desc + 84);
			const std::uint32_t slice = *reinterpret_cast<const std::uint32_t*>(desc + 88);
			void* const         accum = *reinterpret_cast<void* const*>(desc + 0x48);
			const std::uint32_t rmode = accum ? *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<std::uint8_t*>(accum) + 0x150) : 0u;
			void* const         camera = *reinterpret_cast<void* const*>(desc + 64);
			const bool          localMap = target == 4 && slice < 64 && camera && (rmode == 13 || rmode == 15);
			const std::int32_t* port = reinterpret_cast<const std::int32_t*>(desc + 0xD0);
			auto* const         ctx = *engine::g_immediateContext;
			auto* const         S = reinterpret_cast<std::uint32_t*>(engine::S_base.address());

			if (localMap && ShadowMapCache::EnsureCache()) {
				++g_dbgMaps;
				switch (ShadowMapCache::Decide(camera, rmode, g_curMap->classChanged, g_curMap->captureMode)) {
				case ShadowMapCache::Action::StaticCapture:
					++g_dbgCapture;
					RenderPasses(g_curMap->staticPasses, g_curMap->block);  // slice is static-only -> render statics
					ctx->OMSetRenderTargets(0, nullptr, nullptr);           // detach slice DSV before reading it
					ShadowMapCache::Capture(ctx, camera, slice, port);      // copy the clean static base into the cache
					break;
				case ShadowMapCache::Action::Composite:
					++g_dbgComposite;
					if (!ShadowMapCache::Composite(ctx, camera, slice, port))
						RenderPasses(g_curMap->staticPasses, g_curMap->block);  // composite not ready -> full render
					break;
				default:  // RenderFull
					++g_dbgFull;
					RenderPasses(g_curMap->staticPasses, g_curMap->block);  // statics over the inline dynamics
					break;
				}
			} else {
				// isLocal but not eligible yet (rmode transient / cache not ready): statics were claimed -> render.
				RenderPasses(g_curMap->staticPasses, g_curMap->block);
			}
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

		// Advance the movement-detection clock and periodically evict casters not seen for a while (left view).
		++g_frameCounter;
		if ((g_frameCounter & 255u) == 0u) {
			for (auto it = g_casterHist.begin(); it != g_casterHist.end();) {
				if (g_frameCounter - it->second.frame > 300u)
					it = g_casterHist.erase(it);
				else
					++it;
			}
		}
		// Diagnostics: dump the per-frame classifier + action tallies every ~2s, then reset them.
		if (g_frameCounter % 120u == 0u) {
			logger::info("[ShadowMapCache][dbg] localMaps={} static={} dynamic={} (cap={} moved={} flip={}) | capture={} composite={} full={}",
				g_dbgMaps, g_dbgStatic, g_dbgDynamic, g_dbgCapability, g_dbgMoved, g_dbgFlip, g_dbgCapture, g_dbgComposite, g_dbgFull);
		}
		g_dbgMaps = g_dbgStatic = g_dbgDynamic = g_dbgCapability = g_dbgMoved = g_dbgFlip = 0;
		g_dbgCapture = g_dbgComposite = g_dbgFull = 0;

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
