#include "ShadowInstancingFix.h"

#include <cstring>
#include <vector>

#include <d3d11_1.h>

#include "Globals.h"
#include "State.h"
#include "UtilityPassReplica.h"

#include "ShadowMapCache.h"

#include <RE/B/BSRenderPass.h>
#include <RE/B/BSShaderProperty.h>
#include <RE/P/PlayerCharacter.h>

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
		std::uint64_t             treeInline = 0;     // TreeAnim casters: DYNAMIC (sway) -> map is not cacheable
		std::uint64_t             staticSig = 0;      // signature of the claimed static caster set (change detection)
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
		auto*      geom = a_pass->geometry;
		const bool tree = (a_technique & (1u << 26)) != 0;
		// Skinned casters (geom+0x130 != 0) drive the bone-CB + dynamic-VB rings, which map with
		// WRITE_NO_OVERWRITE + a GPU query -- leave them on the engine's inline path; depth-only shadow output
		// is order-independent so mixing is safe.
		const bool skinned = geom && *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(geom) + 0x130);
		// TreeAnim casters (technique bit 1u<<26) sway every frame -> DYNAMIC. Count regardless of claimability so
		// a tree-lit local light is never cache-eligible; else the whole-slice blit freezes the tree's animated
		// shadow (the exact freeze the static/dynamic split must avoid). Trees still render inline serially.
		if (tree)
			++g_curMap->treeInline;
		// Fold EVERY static caster (not skinned, not tree) into the map's static signature -- whether it is
		// claimed for the instanced replay or rendered inline (alpha-test foliage / sub-index). It ends up in the
		// cached slice, so it must be in the change signature: placed-reference identity (stable) XOR world pos.
		// XOR is order-independent (robust to per-frame caster-walk reordering). When a static is placed /
		// disabled / moved the signature changes and the light re-renders once; otherwise it is blitted forever.
		if (geom && !skinned && !tree) {
			std::uint64_t h = 0xcbf29ce484222325ull;
			auto          mix = [&](std::uint64_t v) { h = (h ^ v) * 0x100000001b3ull; };
			mix(reinterpret_cast<std::uint64_t>(geom));                 // per-sub-shape identity (co-located siblings differ)
			mix(reinterpret_cast<std::uint64_t>(geom->GetUserData()));  // placed-reference identity
			// Fold the FULL world transform (NiTransform: rotate 3x3 + translate 3 + scale = 13 floats) so a static
			// that ROTATES/scales in place -- Dwemer gear, windmill, waterwheel: constant translate, changing
			// rotation -- changes the signature and re-renders instead of freezing its cached shadow.
			std::uint32_t b[13];
			std::memcpy(b, &geom->world, sizeof(b));
			for (std::uint32_t v : b)
				mix(v);
			g_curMap->staticSig += h;  // order-independent AND non-cancelling (XOR self-cancels identical folds)
		}
		if (!a_canReplicate) {
			++g_curMap->unsupported;
			return false;  // uncovered -> engine renders inline (serial remainder)
		}
		if (skinned) {
			++g_curMap->skinnedInline;
			return false;
		}
		// kInstance admits the instanceable subset: WHOLE TRISHAPE (geom type 3), NON-alpha-test. The
		// instanced path fills World from the per-instance stream and does NOT run the alpha-test /
		// alpha-blend geometry setup, so alpha-tested casters (foliage) and SUB_INDEX stay inline.
		if (g_instanceRestrict) {
			const bool wholeTri = geom && *(reinterpret_cast<const std::uint8_t*>(geom) + 0x150) == 3;
			// TreeAnim casters read per-object wind (TreeParams) from b2, which the instanced path does
			// NOT fill -- every instance would inherit passes[0]'s wind -> wrong animated depth. Keep them
			// serial (technique bit 1u<<26 == ShaderCache UtilityShaderFlags::TreeAnim). Latent for shadows
			// (no such caster observed) but load-bearing once the z-prepass / occlusion brackets reuse this.
			const bool treeAnim = (a_technique & (1u << 26)) != 0;
			if (!wholeTri || a_alphaTest || treeAnim) {
				++g_curMap->unsupported;
				return false;  // serial remainder (alpha-test / tree-anim / sub-index render inline)
			}
		}
		// Capture the map's clean render-state block at the FIRST covered pass -- while the engine's
		// RT setup is still live (DSV bound to this map's atlas slice, viewport set).
		if (g_curMap->passes.empty())
			std::memcpy(g_curMap->block, reinterpret_cast<void*>(engine::S_base.address()), engine::kBlockBytes);
		g_curMap->passes.push_back(CapturedPass{ a_pass, a_technique, a_alphaTest, a_renderFlags });
		return g_claiming;  // claim -> skip inline; replay owns it
	}

	// Z-prepass (Main_RenderDepth, RelID 100421) capture hook. Same instanceable classification as
	// CaptureHook, plus two leading rejects the z-prepass needs: (G2) the terrain family -- owned by
	// TerrainBlending, which double-renders landscape with a per-draw DSV toggle, so it must NEVER be
	// claimed -- and (G4) OFFSET_DEPTH decals, whose per-object polygon offset can't vary within one
	// DrawIndexedInstanced. Standalone so the shipped shadow CaptureHook stays byte-exact.
	bool DepthCaptureHook(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest,
		std::uint32_t a_renderFlags, bool a_canReplicate)
	{
		if (!g_curMap)
			return false;
		if (!a_canReplicate) {
			++g_curMap->unsupported;
			return false;
		}
		// G2: terrain-family reject FIRST (largest excluded class).
		if (auto* sp = a_pass->shaderProperty) {
			using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
			if (sp->flags.any(Flag::kMultiTextureLandscape, Flag::kLODLandscape, Flag::kNoLODLandBlend)) {
				++g_curMap->unsupported;
				return false;
			}
		}
		// G4: decal reject (DepthWriteDecals technique bit 1u<<17) -- polygon-offset depth, order-sensitive.
		if ((a_technique & (1u << 17)) != 0) {
			++g_curMap->unsupported;
			return false;
		}
		// Skinned -> engine inline (bone-CB / dynamic-VB rings).
		auto* geom = a_pass->geometry;
		if (geom && *reinterpret_cast<void* const*>(reinterpret_cast<const std::uint8_t*>(geom) + 0x130)) {
			++g_curMap->skinnedInline;
			return false;
		}
		// Whole-TRISHAPE, non-alpha-test, non-TreeAnim only (identical restrict to the shadow path).
		const bool wholeTri = geom && *(reinterpret_cast<const std::uint8_t*>(geom) + 0x150) == 3;
		const bool treeAnim = (a_technique & (1u << 26)) != 0;
		if (!wholeTri || a_alphaTest || treeAnim) {
			++g_curMap->unsupported;
			return false;
		}
		// Snapshot the clean render-state block at the first claimed pass, while kMAIN DSV + viewport live.
		if (g_curMap->passes.empty())
			std::memcpy(g_curMap->block, reinterpret_cast<void*>(engine::S_base.address()), engine::kBlockBytes);
		g_curMap->passes.push_back(CapturedPass{ a_pass, a_technique, a_alphaTest, a_renderFlags });
		return g_claiming;
	}

	// FP32 depth-instancing host: the same captured-block save/reseed/force-dirty/restore dance as
	// RenderMapInstanced, but emits via RenderDepthInstancedFP32 (byte-exact FP32 World + R32 IL). The
	// z-prepass is a single depth map (kMAIN), so one MapWork -- no atlas slice, no ShadowMapCache.
	void RenderMapInstancedFP32(MapWork& mw)
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
		sflags[0] = 0xFFFFFFFFu;
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
		UtilityPassReplica::GetSingleton()->RenderDepthInstancedFP32(
			s_passes.data(), s_techs.data(), static_cast<std::uint32_t>(s_passes.size()), mw.passes[0].renderFlags);

		std::memcpy(S, s_saved.data(), engine::kBlockBytes);
		*engine::g_currentTechnique = savedTech;
		*engine::g_currentShader = savedShader;
		*engine::g_currentMaterial = savedMaterial;
		sflags[0] = 0xFFFFFFFFu;
	}

	// Z-prepass instancing is byte-exact + terrain-safe + visually inert (validated: frozen serial-vs-instanced
	// A/B == TAA-jitter baseline, correct in motion). BUT a direct render-thread CPU measurement of the depth
	// pass showed it is a **CPU LOSS of ~360us/frame** (instanced 940us vs serial 580us), NOT a saving: the
	// instanceable statics here are unique solids (rocks/cliffs/buildings) with LOW duplication, so 664 casters
	// collapse into many groups (little draw-call reduction) while the per-pass capture + per-group emit
	// overhead is paid on all of them. Instancing only pays at a HIGH collapse ratio -- the same mesh stamped
	// many times, i.e. TREE FOLIAGE. Enabling this whole-TRISHAPE-solid path pays for the win requires:
	// (1) admitting alpha-test foliage (per-group base-texture + alpha-ref bind; design ready) and (2) a
	// min-instance-count threshold so only high-duplication groups instance. Until that lands this stays
	// default OFF -- the detour is not installed below, zero overhead, zero effect.
	constexpr bool kEnableDepthPrepassInstancing = false;

	// Main_RenderDepth (RelID 100421) detour = the z-prepass instanced bracket. Runs INSIDE the function
	// (so before TerrainBlending's outer-wrapper BlendPrepassDepths, and while b12/CameraViewProj still holds
	// the main camera). Arm DepthCaptureHook + restrict, run the engine depth body (statics claimed + skipped
	// inline, terrain/skinned/alpha/decal render inline), disarm, emit the claimed statics FP32-instanced.
	void Main_RenderDepthDetour(bool a1, bool a2, void* a_original)
	{
		auto callOriginal = *static_cast<void (**)(bool, bool)>(a_original);

		auto* const player = RE::PlayerCharacter::GetSingleton();
		const bool  inWorld = player && player->GetParentCell() &&
			globals::state && !globals::state->IsMainOrLoadingMenuOpen();
		if (!inWorld) {
			callOriginal(a1, a2);
			return;
		}

		static MapWork s_depthWork;
		s_depthWork.passes.clear();
		s_depthWork.unsupported = 0;
		s_depthWork.skinnedInline = 0;

		auto* const replica = UtilityPassReplica::GetSingleton();
		g_curMap = &s_depthWork;
		g_claiming = true;
		g_instanceRestrict = true;
		replica->SetShadowCaptureHook(&DepthCaptureHook);

		callOriginal(a1, a2);

		replica->SetShadowCaptureHook(nullptr);
		g_claiming = false;
		g_instanceRestrict = false;
		g_curMap = nullptr;

		RenderMapInstancedFP32(s_depthWork);
	}

	struct Main_RenderDepthHook
	{
		static void thunk(bool a1, bool a2) { Main_RenderDepthDetour(a1, a2, &func); }
		static inline REL::Relocation<decltype(thunk)> func;
	};

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
			//
			// STATIC/DYNAMIC SPLIT: a map that drew any DYNAMIC caster this frame -- a skinned actor (player,
			// NPC) OR a swaying tree (TreeAnim) -- must never be cached or reused, or those shadows freeze.
			// callOriginal already rendered this frame's dynamic casters inline into the slice, so for such a
			// map we just RenderMapInstanced the fresh static on top -> a fully fresh, correct map. Only
			// STATIC-ONLY local lights cache; the cached slice then holds clean static-only depth (light-space,
			// camera-independent) and is re-rendered ONLY when the static caster set's signature changes (a
			// static placed / disabled / moved) -- otherwise it is blitted forever.
			const bool hasDynamic = g_curMap->skinnedInline > 0 || g_curMap->treeInline > 0;
			const bool eligible = target == 4 && slice < 64 && camera &&
				(rmode == 13 || rmode == 15) && !hasDynamic && ShadowMapCache::EnsureCache();
			if (!eligible) {
				RenderMapInstanced(*g_curMap);
			} else {
				const std::uint64_t sig = g_curMap->staticSig;  // folded over the map's static casters during the walk
				const std::int32_t* port = reinterpret_cast<const std::int32_t*>(desc + 0xD0);
				auto* const         ctx = *engine::g_immediateContext;
				auto* const         S = reinterpret_cast<std::uint32_t*>(engine::S_base.address());
				if (ShadowMapCache::ShouldRender(camera, rmode, desc, sig)) {
					RenderMapInstanced(*g_curMap);                 // this unit's turn -> render fresh + cache
					ctx->OMSetRenderTargets(0, nullptr, nullptr);  // detach slice DSV before reading it
					ShadowMapCache::Capture(ctx, camera, slice, port);
				} else if (ShadowMapCache::CompositeEnabled()) {
					// Phase 1 prototype (dev-flag): SV_Depth composite of the cached static slice (binds its own
					// DSV) instead of the raw CopyPort blit -- validated byte-exact vs blit on static-only maps.
					if (!ShadowMapCache::Composite(ctx, camera, slice, port)) {
						RenderMapInstanced(*g_curMap);
						ctx->OMSetRenderTargets(0, nullptr, nullptr);
						ShadowMapCache::Capture(ctx, camera, slice, port);
					}
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
	// Z-prepass instanced bracket: byte-exact but a net FPS loss on this GPU-bound rig, so NOT installed by
	// default (see kEnableDepthPrepassInstancing). When off, Main_RenderDepth runs untouched -> zero overhead.
	if (kEnableDepthPrepassInstancing)
		stl::detour_thunk<Main_RenderDepthHook>(REL::RelocationID(100421, 0));

	logger::info("[ShadowInstancingFix] detoured RenderShadowmaps @ 0x{:X}, RenderShadowmap @ 0x{:X}",
		RenderShadowmapsHook::func.address(), RenderShadowmapHook::func.address());
}
