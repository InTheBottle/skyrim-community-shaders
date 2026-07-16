#include "UtilityPassReplica.h"

#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"

#include <algorithm>
#include <chrono>
#include <immintrin.h>
#include <intrin.h>
#include <unordered_map>
#include <winrt/base.h>

#include <RE/B/BSRenderPass.h>
#include <RE/B/BSShader.h>
#include <RE/B/BSUtilityShader.h>

// The replicated setup functions transcribe float math (alpha-test refs, split distances, world-
// matrix transforms) that Skyrim's 2021 SSE2 build computes as SEPARATE mul then add (two IEEE
// roundings). This project compiles with /arch:AVX2, under which MSVC contracts `a*b+c` into a
// single-rounded FMA -- which diverges from the engine by 1 ULP and false-fails the byte-exact
// parity gate (observed on the SetupGeometry alpha-test ref: engine 0x3E34B4B2 vs FMA 0x3E34B4B3).
// Disabling FP contraction for this TU makes every replicated multiply-add round exactly like the
// engine, so the recorded constant-buffer bytes match bit-for-bit.
#pragma fp_contract(off)

namespace
{
	// CommunityShaders.dll address range, established at EnsureInitialized via the &EngineCaller
	// module anchor.
	std::uintptr_t g_csBase = 0;
	std::uintptr_t g_csEnd = 0;

	// Call an engine function (or D3D11 vfunc) the way the engine would: a plain direct call.
	template <class R, class... Args>
	inline R EngineCall(const void* a_fn, Args... a_args)
	{
		using Fn = R (*)(Args...);
		return reinterpret_cast<Fn>(const_cast<void*>(a_fn))(a_args...);
	}

	// Vtable-indexed EngineCall.
	template <std::size_t IDX, class R, class T, class... Args>
	inline R EngineCallV(T* a_obj, Args... a_args)
	{
		auto* vt = *reinterpret_cast<void** const*>(a_obj);
		return EngineCall<R>(vt[IDX], static_cast<void*>(a_obj), a_args...);
	}

	inline bool EngineCaller(const void* a_ret)
	{
		const auto a = reinterpret_cast<std::uintptr_t>(a_ret);
		return a < g_csBase || a >= g_csEnd;  // true = not inside CommunityShaders.dll
	}

	// ---------------------------------------------------------------------------
	// RenderPassImmediately detour (SE REL::ID 100854, 1.5.97 0x141308440).
	// ---------------------------------------------------------------------------
	struct RenderPassImmediately_Hook
	{
		static void thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
		{
			UtilityPassReplica::GetSingleton()->OnRenderPassImmediately(a_pass, a_technique, a_alphaTest, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;

		static void Engine(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
		{
			func(a_pass, a_technique, a_alphaTest, a_renderFlags);
		}
	};

}

// ---------------------------------------------------------------------------------
// 1.5.97 engine internals used by the replica. Every address is from the IDA dossier
// (docs/development/utility-pass-re-dossier.md); "S" = RendererShadowState 0x143027EB0,
// "G" = BSGraphics globals 0x143025EF0. All engine-call fallbacks are STAGE-scoped:
// each stage of the RE replaces one with a replicated body, compare-validated.
// ---------------------------------------------------------------------------------
namespace engine
{
	// Batch-renderer cross-pass caches (written by the walk, shared with engine passes).
	inline REL::Relocation<std::uint32_t*> g_currentTechnique{ REL::Offset(0x3283BA4) };
	inline REL::Relocation<RE::BSShader**> g_currentShader{ REL::Offset(0x3283BA8) };
	inline REL::Relocation<void**>         g_currentMaterial{ REL::Offset(0x3490BB0) };
	inline REL::Relocation<std::uint32_t*> g_debugTechnique{ REL::Offset(0x1E0DF8C) };  // write-only
	inline REL::Relocation<std::uint8_t*>  g_useEarlyZ{ REL::Offset(0x302C8E5) };
	inline REL::Relocation<void**>         g_skyShaderInstance{ REL::Offset(0x32336C0) };

	// The 0x5D8 render-state block base (S+0x00 .. S+0x5D8): every field the utility setup path reads
	// or writes (dirty words, RT/depth/raster/blend modes, sampler and SRV caches, vertex desc,
	// current shaders, topology, camera data, blend-extra). DrawTriShape reads S+0x340/+0x358 off it.
	inline REL::Relocation<std::uint8_t*> S_base{ REL::Offset(0x3027EB0) };

	// Cross-pass shadow token written by SetupGeometry (0x14130EC70) and read+reset by
	// RestoreTechnique (0x141310300) to decide the alpha-blend dirty bit. It lives well
	// outside the RendererShadowState span, so the compare harness must snapshot it
	// separately or the replica's RestoreTechnique reads the engine window's leftover
	// value and sets a spurious OMSetBlendState.
	inline REL::Relocation<std::uint32_t*> g_shadowGeomToken{ REL::Offset(0x1E10660) };

	// BSGraphics::Renderer singleton (first arg of the draw leaves).
	inline REL::Relocation<std::uint8_t*> g_renderer{ REL::Offset(0x3028490) };

	// Engine TLS index (0x143497408): the render path stamps per-thread markers into the
	// module TLS block -- +1896 is the memory-context tag the standard path sets to 26,
	// +10752 is the accumulator the skinned dispatcher zeroes (FUN_14131f7c0).
	inline REL::Relocation<std::uint32_t*> g_tlsIndex{ REL::Offset(0x3497408) };
	inline std::uint8_t* TlsBlock()
	{
		auto* tlsArray = reinterpret_cast<std::uint8_t**>(__readgsqword(0x58));
		return tlsArray[*g_tlsIndex];
	}

	// BSSubIndexTriShape (GeometryType 8) helpers: segment-coalesce pass (CPU-only state
	// maintenance, 0x140D59430) and the "always draw whole shape" global (0x1430243B0).
	using SubIndexPreDraw_t = void (*)(void*);
	inline REL::Relocation<SubIndexPreDraw_t> SubIndexPreDraw{ REL::Offset(0xD59430) };
	inline REL::Relocation<std::uint8_t*>     g_subIndexWholeDraw{ REL::Offset(0x30243B0) };

	// Skinned-path dynamic-data upload (BSDynamicTriShape under a skin, e.g. faces):
	// map a slice of the shared dynamic ring (out: ring offset), lock/fetch the CPU-side
	// dynamic vertex data, copy, unmap, unlock. All engine helpers, addresses verified
	// against the dispatcher disasm at 0x141308A05.
	using MapSkinDyn_t = void* (*)(void*, std::uint32_t, std::int32_t*);
	inline REL::Relocation<MapSkinDyn_t> MapSkinDynamicData{ REL::Offset(0xD6C8A0) };  // 0x140D6C8A0
	using UnmapSkinDyn_t = void (*)(void*, void*);
	inline REL::Relocation<UnmapSkinDyn_t> UnmapSkinDynamicData{ REL::Offset(0xD6C9E0) };  // 0x140D6C9E0
	using DynShapeLock_t = void* (*)(void*);
	inline REL::Relocation<DynShapeLock_t> DynShapeLockData{ REL::Offset(0xC723C0) };  // 0x140C723C0
	using DynShapeUnlock_t = void (*)(void*);
	inline REL::Relocation<DynShapeUnlock_t> DynShapeUnlock{ REL::Offset(0xC72420) };  // 0x140C72420

	// The draw-struct the skinned dispatcher builds on its stack and hands to the
	// skin-instance Render vfunc (37). Layout verified against the disasm at
	// 0x141308A05 (stack frame rsp+0x30..0x6D).
	struct SkinDrawStruct
	{
		void*         boneSetter;    // +0x00  shader+0x10 (NiBoneMatrixSetterI) or null
		void*         geometry;      // +0x08
		std::uint64_t unk10;         // +0x10  = 0
		std::int32_t  singleLevel;   // +0x18  (pass+0x1E >> 7) & 1
		std::int32_t  lodIndex;      // +0x1C  pass+0x1E & 0x7F
		float         unk20;         // +0x20  = 0.0f
		std::int32_t  dynOffset[6];  // +0x24  [0] = -1; dynamic-ring offset out-param
		std::uint16_t boneMode;      // +0x3C  = 1 only on the bone-setter branch
		std::uint16_t pad3E;
	};
	static_assert(sizeof(SkinDrawStruct) == 0x40);
	static_assert(offsetof(SkinDrawStruct, dynOffset) == 0x24);
	static_assert(offsetof(SkinDrawStruct, boneMode) == 0x3C);

	// Engine helpers still called in Stage A (replaced in later stages).
	using SetDirtyStates_t = void (*)(bool);
	inline REL::Relocation<SetDirtyStates_t> SetDirtyStates{ REL::Offset(0xD705B0) };

	// --- Per-pass setup reimplementation support (FlushSetupTechnique/GeometryReplica).
	//     These reimplement BSUtilityShader::SetupTechnique/Material/Geometry ctx-parameterized
	//     so a worker can fill+bind its OWN PerTechnique/Material/Geometry CBs on its own context.
	//     The CBs are owned by the current VS/PS shader objects (*0x1430281F8 / *0x143028200 =
	//     block+0x348 / +0x350); at N=1 the shared shader CBs are passed for byte-identical parity.
	// SetupTechnique helpers + data
	inline REL::Relocation<std::uint32_t (*)(std::uint32_t)> UtilVSIndex{ REL::Offset(0x1334900) };
	inline REL::Relocation<std::uint32_t (*)(std::uint32_t)> UtilPSIndex{ REL::Offset(0x1334970) };
	inline REL::Relocation<bool (*)(RE::BSShader*, std::uint32_t, std::uint32_t, bool)> BeginTechnique{ REL::Offset(0x131FBD0) };
	inline REL::Relocation<std::uint32_t (*)(void*)> RTGetWidth{ REL::Offset(0xD74C20) };
	inline REL::Relocation<std::uint32_t (*)(void*)> RTGetHeight{ REL::Offset(0xD74C60) };
	inline REL::Relocation<std::int32_t (*)()>       GetDepthStencilTargetMain{ REL::Offset(0xD74E50) };
	inline REL::Relocation<float (*)(float, float)>  PowF{ REL::Offset(0x134BEAC) };
	inline REL::Relocation<std::uint8_t*>  g_mainRTDesc{ REL::Offset(0x302BB20) };
	inline REL::Relocation<float*>         g_utilDepthConst{ REL::Offset(0x1E0DF04) };
	inline REL::Relocation<std::uint8_t*>  g_dsvDirty{ REL::Offset(0x30284C2) };          // OUT of 0x5D8 block
	inline REL::Relocation<std::uint8_t*>  g_focusShadowEnable{ REL::Offset(0x1E0DE43) };
	inline REL::Relocation<std::uint32_t*> g_focusShadowCount{ REL::Offset(0x31D0FB8) };
	inline REL::Relocation<std::uint8_t**> g_viewCamera{ REL::Offset(0x31D0E68) };
	inline REL::Relocation<std::uint8_t**> g_shadowSceneNode{ REL::Offset(0x1E0DED0) };
	inline REL::Relocation<std::uint32_t*> g_shadowFixedCount{ REL::Offset(0x1867188) };
	inline REL::Relocation<std::uint8_t*>  g_copySplitToVS{ REL::Offset(0x1E0DE4C) };
	inline REL::Relocation<std::uint32_t*> g_shadowMode{ REL::Offset(0x1E0DE34) };
	inline REL::Relocation<std::uint32_t*> g_poissonDenom{ REL::Offset(0x3283B90) };
	inline REL::Relocation<float*>         g_poissonRadiusScale{ REL::Offset(0x1E10670) };
	inline REL::Relocation<float*>         g_fixedSplit{ REL::Offset(0x3283B78) };
	inline REL::Relocation<float*>         g_maxFocusDist{ REL::Offset(0x1E106B8) };
	inline REL::Relocation<float**>        g_focusShadowData{ REL::Offset(0x31D0FA8) };
	inline REL::Relocation<float*>         g_fadeFracStart{ REL::Offset(0x1E106A0) };
	inline REL::Relocation<float*>         g_shadowRadius{ REL::Offset(0x1E10B78) };
	inline REL::Relocation<float*>         g_shadowSign{ REL::Offset(0x1E10B7C) };
	inline REL::Relocation<float*>         g_biasBase{ REL::Offset(0x3283B7C) };
	// SetupGeometry helpers + data
	inline REL::Relocation<void*> SG_BuildMatrix{ REL::Offset(0x12C3440) };
	inline REL::Relocation<void*> SG_MatrixTranspose{ REL::Offset(0x134C1DC) };
	inline REL::Relocation<void*> SG_Vec3TransformCoord{ REL::Offset(0x134C206) };
	inline REL::Relocation<void*> SG_ShadowFill{ REL::Offset(0x130F960) };
	inline REL::Relocation<void*> SG_SetupShadowLightParams{ REL::Offset(0x130FBE0) };
	inline REL::Relocation<void*> SG_ScissorFromBBox{ REL::Offset(0xD70100) };
	inline REL::Relocation<void*> SG_ScissorApply{ REL::Offset(0xD6FCF0) };
	inline REL::Relocation<void*> SG_WorldToView{ REL::Offset(0xD42C50) };
	inline REL::Relocation<void*> SG_GetAccumulator{ REL::Offset(0x12966A0) };
	inline REL::Relocation<std::uintptr_t> SG_pCamNode{ REL::Offset(0x31D0F88) };
	inline REL::Relocation<std::uintptr_t> SG_pViewFrustumObj{ REL::Offset(0x31D0E68) };
	inline REL::Relocation<std::uintptr_t> SG_pFadeExclude{ REL::Offset(0x31D0DA8) };
	inline REL::Relocation<std::uintptr_t> SG_mode1D0E28{ REL::Offset(0x31D0E28) };
	inline REL::Relocation<std::uintptr_t> SG_flagDE4C{ REL::Offset(0x1E0DE4C) };
	inline REL::Relocation<std::uintptr_t> SG_modeDF94{ REL::Offset(0x1E0DF94) };
	inline REL::Relocation<std::uintptr_t> SG_windFadeMin{ REL::Offset(0x1E0DF70) };
	inline REL::Relocation<std::uintptr_t> SG_windFadeMax{ REL::Offset(0x1E0DF74) };
	inline REL::Relocation<std::uintptr_t> SG_stencilVal014{ REL::Offset(0x1E0E014) };
	inline REL::Relocation<std::uintptr_t> SG_c283B88{ REL::Offset(0x3283B88) };
	inline REL::Relocation<std::uintptr_t> SG_c283B7C{ REL::Offset(0x3283B7C) };
	inline REL::Relocation<std::uintptr_t> SG_recip127{ REL::Offset(0x156302C) };
	inline REL::Relocation<std::uintptr_t> SG_recip255{ REL::Offset(0x1540648) };
	inline REL::Relocation<std::uintptr_t> SG_alphaBias{ REL::Offset(0x1866724) };

	using GetNiProperty_t = RE::NiAlphaProperty* (*)(RE::BSRenderPass*);
	inline REL::Relocation<GetNiProperty_t> GetNiProperty{ REL::Offset(0x12FD8A0) };
	using GrassShadowBlacklist_t = bool (*)(std::uint32_t);
	inline REL::Relocation<GrassShadowBlacklist_t> IsGrassShadowBlacklist{ REL::Offset(0x12CCE20) };
	using SetupGeomAlphaBlend_t = void (*)(RE::BSShader*, RE::NiAlphaProperty*, RE::BSShaderProperty*, bool);
	inline REL::Relocation<SetupGeomAlphaBlend_t> SetupGeometryAlphaBlending{ REL::Offset(0x131F440) };
	using SetupAlphaTestRef_t = void (*)(RE::BSShader*, RE::NiAlphaProperty*, RE::BSShaderProperty*);
	inline REL::Relocation<SetupAlphaTestRef_t> SetupAlphaTestRef{ REL::Offset(0x131F2A0) };

	// BSGraphics::TriShape (QRendererData): +0 vertexBuffer, +8 indexBuffer, +0x10 vertexDesc.
	struct TriShapeData
	{
		ID3D11Buffer* vertexBuffer;
		ID3D11Buffer* indexBuffer;
		std::uint64_t vertexDesc;
	};
}

namespace
{

	// Render-state accessors. The multithreaded shadow-worker path was removed, so these now read
	// the engine globals directly (the N=1 render-thread state the replica always ran on).
	inline std::uint8_t*        WsBlock() { return reinterpret_cast<std::uint8_t*>(engine::S_base.address()); }
	inline ID3D11DeviceContext* WsCtx() { return globals::d3d::context; }
	inline std::uint32_t&       WsTechnique() { return *engine::g_currentTechnique; }
	inline RE::BSShader*&       WsShader() { return *engine::g_currentShader; }
	inline void*&               WsMaterial() { return *reinterpret_cast<void**>(engine::g_currentMaterial.address()); }
	inline std::uint32_t&       WsShadowToken() { return *engine::g_shadowGeomToken; }
	inline std::uint32_t        WsTechFlags(std::uint8_t* a_shader) { return *reinterpret_cast<std::uint32_t*>(a_shader + 0x90); }
	inline std::uint8_t&        WsDsvDirty() { return *engine::g_dsvDirty; }
}

// Install the RenderPassImmediately detour (the seam ShadowThreaded's instancing path rides to observe
// each utility pass and offer it to the shadow-capture hook) exactly once. Idempotent.
void UtilityPassReplica::EnsureInitialized()
{
	if (hooksInstalled)
		return;

	// Establish our own module range (used by EngineCaller); anchored on &EngineCaller.
	{
		HMODULE mod = nullptr;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&EngineCaller), &mod) &&
			mod) {
			g_csBase = reinterpret_cast<std::uintptr_t>(mod);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_csBase + dos->e_lfanew);
			g_csEnd = g_csBase + nt->OptionalHeader.SizeOfImage;
		}
	}

	InstallHooks();
}

void UtilityPassReplica::InstallHooks()
{
	if (hooksInstalled)
		return;
	// SE-only: every RE'd offset below is 1.5.97.
	if (!REL::Module::IsSE()) {
		logger::info("[UtilityPassReplica] SE-only; not installing on this runtime");
		return;
	}

	// Pure code detour (RelocationID) -- installable at kPostPostLoad, before the D3D device
	// exists. The replica's D3D resources are created lazily on the first shadow-map walk.
	stl::detour_thunk<RenderPassImmediately_Hook>(REL::RelocationID(100854, 107644));

	hooksInstalled = true;
	logger::info("[UtilityPassReplica] installed RenderPassImmediately detour");
}

void UtilityPassReplica::OnRenderPassImmediately(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
{
	// Only utility passes are in scope; everything else is always the engine's.
	const bool isUtility = a_pass && a_pass->shader &&
	                       a_pass->shader->shaderType.get() == RE::BSShader::Type::Utility;
	if (!isUtility) {
		RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
		return;
	}
	// Proceed only when a shadow-capture hook is armed (ShadowInstancingFix arms it only during the
	// shadow-map walk, so main-scene utility passes are unaffected). Otherwise the engine renders.
	if (!shadowCaptureHook.load(std::memory_order_acquire)) {
		RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
		return;
	}

	// Only replicate in fully-SETTLED in-world rendering. Load-screen / main-menu 3D renders (the
	// rotating loading-screen model, the main-menu preview) AND the world-init window right after a
	// save load drive utility shadow passes with transiently-uninitialized shadow state -- the
	// focus-shadow array, the shadow camera node (*0x1431D0F88), the scene light list, and
	// per-shader constant buffers are all mid-init. Worse, the load->gameplay transition renders
	// utility passes from TWO threads (the loading-screen renderer vs the main render thread, see
	// the ownership note below): the loading-screen thread tears down/rebuilds the shadow camera
	// while the main-thread replica is mid-compare, so the camera/light pointers flip to garbage
	// BETWEEN the engine and replica windows. The engine tolerates all this by rendering each pass
	// once; the double-rendering replica re-reads the raced transient and faults. No instantaneous
	// flag is race-proof, so gate on the standard CS "in world" signal (player + parent cell + no
	// main/loading menu) AND require it to have held continuously for a short settle window, so the
	// transition race is fully past before the replica runs. Outside that, the engine draws as-is.
	auto* const player = RE::PlayerCharacter::GetSingleton();
	const bool  rawInWorld = player && player->GetParentCell() &&
	                     globals::state && !globals::state->IsMainOrLoadingMenuOpen();
	// Settle: proceed only after we've been CONTINUOUSLY in-world for kSettleMs. Track the first-in-world
	// timestamp and reset it whenever we leave the world (menu/load), so the load->gameplay transition
	// race is past before the replica double-renders. Unlike the old s_everUnsettled latch, this does NOT
	// require a prior !rawInWorld pass to have routed through here -- that latch could stay closed forever
	// when the boot menu never drives a utility pass to this detour (observed: the capture-hook modes
	// never settled because s_everUnsettled stayed false the whole session).
	static std::atomic<std::int64_t> s_inWorldSinceMs{ 0 };
	const std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
	                               std::chrono::steady_clock::now().time_since_epoch())
	                               .count();
	if (rawInWorld) {
		std::int64_t expected = 0;
		s_inWorldSinceMs.compare_exchange_strong(expected, nowMs, std::memory_order_relaxed);  // stamp first in-world frame
	} else {
		s_inWorldSinceMs.store(0, std::memory_order_relaxed);  // left world -> restart the settle window
	}
	constexpr std::int64_t kSettleMs = 750;
	const std::int64_t     inWorldSince = s_inWorldSinceMs.load(std::memory_order_relaxed);
	const bool             settled = rawInWorld && inWorldSince != 0 && (nowMs - inWorldSince) >= kSettleMs;
	if (!settled) {
		RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
		return;
	}

	// Shadow-capture fan-out (ShadowInstancingFix). While a shadow-map walk is being captured, offer
	// each utility pass to the hook; if it claims ownership (the instanced replay renders it later),
	// skip the inline render here entirely. Otherwise fall through to the CanReplicate + engine render.
	if (auto hook = shadowCaptureHook.load(std::memory_order_acquire)) {
		if (hook(a_pass, a_technique, a_alphaTest, a_renderFlags, CanReplicate(a_pass)))
			return;
	}

	// Outside current replica coverage: whole-pass engine render.
	if (!CanReplicate(a_pass)) {
		RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
		return;
	}

	// A covered pass the capture hook declined to claim (the skinned / alpha-test / sub-index casters
	// the instanced replay leaves on the engine's serial remainder): render it inline via the engine.
	RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

bool UtilityPassReplica::CanReplicate(RE::BSRenderPass* a_pass) const
{
	// Stage A coverage: unskinned, non-custom-render, plain TRISHAPE geometry. Anything
	// else takes the engine path whole-pass (never mid-pass).
	auto* geom = a_pass->geometry;
	if (!geom)
		return false;
	const auto* geomBytes = reinterpret_cast<const std::uint8_t*>(geom);
	// STENCIL_ABOVE_WATER releases the bound PS on first use, so it stays whole-pass engine
	// until the replica owns that path.
	const std::uint32_t f = a_pass->passEnum - 0x2B;
	if ((f & 0x1200) == 0x1200)
		return false;
	if (*reinterpret_cast<void* const*>(geomBytes + 0x130)) {  // skin instance
		// Skinned coverage (Stage B): the static skin-instance Render branch. The
		// dynamic bone-setter branch (geometry vfunc 54 non-zero) routes into the full
		// Draw dispatch and stays whole-pass engine until that path is replicated.
		if (EngineCallV<54, std::uint64_t>(const_cast<RE::BSGeometry*>(geom)) != 0)
			return false;
		return true;
	}
	if (geomBytes[0x109] & 8)  // needs-custom-render
		return false;
	if (geomBytes[0x150] != 3 && geomBytes[0x150] != 8)  // TRISHAPE / SUB_INDEX_TRISHAPE
		return false;
	if (!*reinterpret_cast<void* const*>(geomBytes + 0x138))  // rendererData
		return false;
	return true;
}

void UtilityPassReplica::ReplicaRenderPassImmediately(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
{
	// ---- RenderPassImmediately body (1.5.97 0x141308440), replicated ----
	auto* shader = a_pass->shader;
	auto* geom = a_pass->geometry;

	// Technique cache: BeginPass only when the technique or shader changed. 0x5C006076
	// never caches (the engine forces re-setup for that sentinel technique).
	const bool cached = WsTechnique() == a_technique &&
	                    a_technique != 0x5C006076 &&
	                    shader == WsShader();
	if (!cached) {
		*engine::g_debugTechnique = a_technique;
		// BeginPass (0x1413086C0): RestoreTechnique on the outgoing shader, clear the
		// caches, SetupTechnique on the incoming one, then re-stamp the caches.
		if (auto* prev = WsShader())
			EngineCallV<3, void>(prev, WsTechnique());  // RestoreTechnique
		WsShader() = nullptr;
		WsTechnique() = 0;
		WsMaterial() = nullptr;
		if (!EngineCallV<2, bool>(shader, a_technique))  // SetupTechnique
			return;                                      // engine bails the whole pass on setup failure
		WsShader() = shader;
		WsTechnique() = a_technique;
	}

	// Material change detection (cache at 0x143490BB0).
	void* material = a_pass->shaderProperty ? *reinterpret_cast<void* const*>(
												  reinterpret_cast<const std::uint8_t*>(a_pass->shaderProperty) + 0x78) :
	                                          nullptr;
	if (material != WsMaterial()) {
		if (material)
			EngineCallV<4, void>(shader, material);  // SetupMaterial
		WsMaterial() = material;
	}

	// ucCurrentMeshLODLevel: the walk stamps the pass's LOD index onto the geometry.
	auto* geomBytes = reinterpret_cast<std::uint8_t*>(geom);
	geomBytes[0x108] = static_cast<std::uint8_t>(a_pass->LODMode.index);

	// ---- geometry dispatch (RenderPassImmediately tail, 1.5.97 0x1413084C5) ----
	if (*reinterpret_cast<void**>(geomBytes + 0x130)) {
		// SKINNED pass: the bone-palette upload runs through the engine's skin Render vfunc.
		ReplicaRenderSkinned(a_pass, a_alphaTest, a_renderFlags);
		return;
	}

	// ---- _Standard path (0x1413088C0): TLS tag -> ShaderSetup -> Draw -> Restore ----
	// The standard path stamps memory-context tag 26 into the module TLS block for the
	// duration of the draw and restores the previous tag after (no D3D11 effect; kept
	// for faithful replication of engine-visible state).
	auto* const         tlsTag = reinterpret_cast<std::uint32_t*>(engine::TlsBlock() + 1896);
	const std::uint32_t savedTlsTag = *tlsTag;
	*tlsTag = 26;

	const bool alphaTest = a_alphaTest || *engine::g_useEarlyZ != 0;

	// ShaderSetup (0x141309F80): alpha-blend + alpha-test-ref setup, then SetupGeometry.
	if (shader != *reinterpret_cast<RE::BSShader**>(engine::g_skyShaderInstance.address())) {
		if ((a_renderFlags & 4) && !EngineCall<bool>(reinterpret_cast<void*>(engine::IsGrassShadowBlacklist.address()), a_pass->passEnum)) {
			auto* alphaProp = EngineCall<RE::NiAlphaProperty*>(reinterpret_cast<void*>(engine::GetNiProperty.address()), a_pass);
			EngineCall<void>(reinterpret_cast<void*>(engine::SetupGeometryAlphaBlending.address()), shader,
				alphaProp, a_pass->shaderProperty, alphaTest);
		}
		if (alphaTest) {
			if (auto* alphaProp = EngineCall<RE::NiAlphaProperty*>(reinterpret_cast<void*>(engine::GetNiProperty.address()), a_pass))
				EngineCall<void>(reinterpret_cast<void*>(engine::SetupAlphaTestRef.address()), shader, alphaProp, a_pass->shaderProperty);
		}
	}
	EngineCallV<6, void>(shader, a_pass, a_renderFlags);  // SetupGeometry

	// ---- Draw dispatch (0x141307160) on GeometryType (geom+0x150) ----
	auto* rd = *reinterpret_cast<engine::TriShapeData**>(geomBytes + 0x138);
	const std::uint16_t wholeTriCount = *reinterpret_cast<const std::uint16_t*>(geomBytes + 0x158);

	switch (geomBytes[0x150]) {
	case 3:  // TRISHAPE -> DrawTriShape whole
		DrawTriShapeReplica(rd, 0, wholeTriCount);
		break;
	case 8:  // SUB_INDEX_TRISHAPE (case 7): segment-coalesce, then whole or per-segment
		EngineCall<void>(reinterpret_cast<void*>(engine::SubIndexPreDraw.address()), static_cast<void*>(geom));
		if (*engine::g_subIndexWholeDraw) {
			DrawTriShapeReplica(rd, 0, wholeTriCount);
		} else {
			// drawAll byte geom+0x171; active-segment count geom+0x168; segment array
			// geom+0x160, stride 0x14: +0x00 startIndex, +0x0C numTris, +0x10 enabled.
			const bool          drawAll = geomBytes[0x171] != 0;
			const std::uint32_t count = drawAll ? 1u : *reinterpret_cast<const std::uint32_t*>(geomBytes + 0x168);
			const auto*         seg = *reinterpret_cast<const std::uint8_t* const*>(geomBytes + 0x160);
			for (std::uint32_t i = 0; i < count; ++i, seg += 0x14) {
				if (!seg[0x10])
					continue;
				const std::uint32_t numTris = drawAll ? wholeTriCount : *reinterpret_cast<const std::uint32_t*>(seg + 0x0C);
				const std::uint32_t start = drawAll ? 0u : *reinterpret_cast<const std::uint32_t*>(seg + 0x00);
				DrawTriShapeReplica(rd, start, numTris);
			}
		}
		break;
	}

	EngineCallV<7, void>(shader, a_pass, a_renderFlags);  // RestoreGeometry
	*tlsTag = savedTlsTag;
}

namespace
{
// The anonymous namespace opened above stays open through the next two definitions; it closes
// after FlushSetupGeometryReplica, before the UtilityPassReplica class-method definitions.

// Load-screen transient guard. During load-screen shadow rendering (a BSShadowParabolicLight
// pass under the MistMenu) some shader PerTechnique/PerGeometry CBs are momentarily unmapped
// (their buffer pointer is null) while the pass still runs. The engine tolerates this because it
// renders each pass exactly once; the compare/replace replica re-renders the pass and can read
// the transient. Routing a CB write to a per-thread scratch when the mapped pointer is null keeps
// the replica from faulting. In steady in-world rendering every CB the replica fills IS mapped
// (non-null), so the scratch is never used and the parity gate stays byte-exact. Sized for the
// largest register offset (4 * 255 + a 16-byte movups tail).
inline std::uint8_t* CbOrScratch(std::uint8_t* mapped)
{
	alignas(16) static thread_local std::uint8_t s_scratch[1088];
	return mapped ? mapped : s_scratch;
}

// Canonical user-space pointer test. The in-world settle latch keeps the replica off during load,
// but the load->gameplay boundary still leaves some shadow objects reachable-yet-uninitialized for
// a pass or two -- e.g. a valid shadow light (v30) whose light._ptr is momentarily a garbage,
// non-canonical value. Guarding an object deref on this test skips only those transient reads; in
// established gameplay every such pointer is a real heap object, so the guard never fires and the
// parity gate stays byte-exact.
inline bool IsCanonicalPtr(const void* p)
{
	const auto v = reinterpret_cast<std::uintptr_t>(p);
	return v >= 0x10000 && v < 0x0000800000000000ull;
}

// BSUtilityShader::SetupTechnique (vf2, 1.5.97 0x14130DF90) reimplemented against a caller-
// supplied context + the two PerTechnique constant buffers + the 0x5D8 state block S. Runs the
// technique->VS/PS index lookups + BeginTechnique (which VS/PS-binds, routed via EngineCall so
// the recorder captures it), maps/fills/binds the PerTechnique CBs (slot 0) with the shadow /
// depth / split / poisson / focus constants at the shader-declared byte offsets. At N=1 vsCB/psCB
// are null and the shader-owned CBs (vs+0x18 / ps+0x10) are resolved internally, so it is
// byte-identical. Returns BeginTechnique's result; the caller must bail the whole pass on false.
bool FlushSetupTechniqueReplica(ID3D11DeviceContext* ctx, ID3D11Buffer* vsCB, ID3D11Buffer* psCB,
                                std::uint8_t* S, RE::BSShader* a1, std::int32_t a2)
{
	// ---- 0x5D8 render-state block accessors (S = 0x143027EB0 at N=1) ----
	const auto u32    = [&](std::size_t o) { return *reinterpret_cast<std::uint32_t*>(S + o); };
	const auto setU32 = [&](std::size_t o, std::uint32_t v) { *reinterpret_cast<std::uint32_t*>(S + o) = v; };
	const auto orU32  = [&](std::size_t o, std::uint32_t v) { *reinterpret_cast<std::uint32_t*>(S + o) |= v; };
	const auto u8b    = [&](std::size_t o) { return *reinterpret_cast<std::uint8_t*>(S + o); };
	const auto setU8  = [&](std::size_t o, std::uint8_t v) { *reinterpret_cast<std::uint8_t*>(S + o) = v; };
	const auto f32S   = [&](std::size_t o) { return *reinterpret_cast<float*>(S + o); };
	const auto qw     = [&](std::size_t o) { return *reinterpret_cast<std::uint64_t*>(S + o); };
	const auto setQw  = [&](std::size_t o, std::uint64_t v) { *reinterpret_cast<std::uint64_t*>(S + o) = v; };
	const auto wf     = [](std::uint8_t* base, std::size_t byteOff, float v) { *reinterpret_cast<float*>(base + byteOff) = v; };

	const std::int32_t v2 = a2 - 0x2B;

	// Technique -> VS/PS shader-index lookups (pure CPU, no context calls) then BeginTechnique
	// (0x14131FBD0) which internally VSSetShader/PSSetShader-binds -- routed via EngineCall so
	// its (possibly tail-called) binds carry the out-of-module return address and are captured.
	const std::uint32_t vsIndex = EngineCall<std::uint32_t>(reinterpret_cast<void*>(engine::UtilVSIndex.address()), static_cast<std::uint32_t>(v2));   // FUN_141334900
	const std::uint32_t psIndex = EngineCall<std::uint32_t>(reinterpret_cast<void*>(engine::UtilPSIndex.address()), static_cast<std::uint32_t>(v2));   // FUN_141334970

	const bool v6 = ((v2 & 0x14000) == 0x14000) ||
	                (((v2 & 0x20004000) != 0x4000) && ((v2 & 0x1E02000) != 0x2000)) ||
	                ((v2 & 0x80) != 0) ||
	                ((v2 & 0x14000) == 0x10000);

	// BeginTechnique (0x14131FBD0) internally VS/PS-binds on the engine's global context slot and
	// writes the global VS/PS shader-object slots (block+0x348/0x350); read those back for the fills.
	const bool    v59 = EngineCall<bool>(reinterpret_cast<void*>(engine::BeginTechnique.address()), a1, vsIndex, psIndex, !v6);
	std::uint8_t* gvs = *reinterpret_cast<std::uint8_t**>(engine::S_base.address() + 0x348);
	std::uint8_t* gps = *reinterpret_cast<std::uint8_t**>(engine::S_base.address() + 0x350);
	if (!v59)
		return false;

	// Propagate BeginTechnique's resolved VS/PS shader objects into the caller's block S.
	auto* vs = gvs;
	auto* ps = gps;
	*reinterpret_cast<std::uint8_t**>(S + 0x348) = vs;
	*reinterpret_cast<std::uint8_t**>(S + 0x350) = ps;

	// PerTechnique CBs to Map/fill/bind: passed private copies, or the shader-owned CBs at N=1.
	ID3D11Buffer*            vsBuf = vsCB ? vsCB : *reinterpret_cast<ID3D11Buffer**>(vs + 0x18);
	D3D11_MAPPED_SUBRESOURCE mapped{};  // == var_C8 (reused as the powf scratch far below)
	// Keep the mapped pointers in LOCALS: the engine stashes them on the SHARED vs/ps shader object
	// (+0x20 / +0x18), which two workers would clobber. The stashes below are kept for engine parity
	// but the fills read the locals, so a worker never depends on the shared field.
	void* pDataVSraw = nullptr;
	void* pDataPSraw = nullptr;

	if (vsBuf) {
		EngineCallV<14, HRESULT>(ctx, vsBuf, 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped);  // Map VS CB
		pDataVSraw = mapped.pData;
		*reinterpret_cast<void**>(vs + 0x20) = mapped.pData;                              // shader+0x20 = pData (parity)
	}
	ID3D11Buffer* psBuf = nullptr;
	if (v6) {
		psBuf = psCB ? psCB : *reinterpret_cast<ID3D11Buffer**>(ps + 0x10);
		if (psBuf) {
			EngineCallV<14, HRESULT>(ctx, psBuf, 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped);  // Map PS CB
			pDataPSraw = mapped.pData;
			*reinterpret_cast<void**>(ps + 0x18) = mapped.pData;                              // shader+0x18 = pData (parity)
		}
	}

	// Technique flags on the shader object (BSShader+0x90 / +0x94); the engine consumer reads them.
	*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(a1) + 0x90) = static_cast<std::uint32_t>(v2);
	*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(a1) + 0x94) = static_cast<std::uint32_t>(v2 & 0x7F);

	// The engine reloads [shader+0x20]/[shader+0x18] lazily at each fill site; hoisting here is
	// equivalent because pDataVS is consumed only in VS fills (vs always present) and pDataPS
	// only in PS fills (ps present). A vertex-only technique (v6 false) leaves ps null, so the
	// deref must be guarded. CbOrScratch also absorbs the load-screen transient where the CB is
	// momentarily unmapped (see the note above); in steady state both are the mapped pointers.
	auto* pDataVS = CbOrScratch(vs ? reinterpret_cast<std::uint8_t*>(pDataVSraw) : nullptr);
	auto* pDataPS = CbOrScratch(ps ? reinterpret_cast<std::uint8_t*>(pDataPSraw) : nullptr);

	// -- Block 1: VS PerTechnique depth constants (offset vs[0x53]) --
	if ((v2 & 0x1E00100) == 0x100) {
		const std::size_t o = 4u * vs[0x53];
		const float*      c = engine::g_utilDepthConst.get();  // xmmword_141E0DF04 (4 floats)
		wf(pDataVS, o + 0, c[0] - f32S(0x35C));                // - unk_14302820C (block-local)
		wf(pDataVS, o + 4, c[1] - f32S(0x360));                // - qword_143028210 (block-local, dword)
		wf(pDataVS, o + 8, c[2] - 15.0f);                      // - dword_1415E29B4
		wf(pDataVS, o + 12, c[3] - 15.0f);
	}

	if (v2 & 0x1E00000) {
		// ---- shadow-cascade setup (writes PS CB + block dirty bits) ----
		std::uint8_t  v16 = u8b(0xB4);   // unk_143027F64 (byte)
		std::uint32_t flags = u32(0x00); // dword_143027EB0 (cached, threaded exactly as the asm)
		if (v16) {
			v16 = 0;
			flags |= 0x100;
			setU8(0xB4, 0);
			setU32(0x00, flags);
		}

		const std::uint8_t off47 = ps[0x47];  // ShadowSampleParam base (PS CB)

		if (v2 & 0x2000) {
			if (v16) {
				flags |= 0x100;
				setU8(0xB4, 0);
				setU32(0x00, flags);
			}
			if (v2 & 0x200000) {
				if (u32(0x88) != 0) {  // unk_143027F38
					const std::uint32_t alt = flags | 4u;
					std::uint32_t       nf = flags & ~4u;
					if (u32(0x8C) != 0)  // unk_143027F3C
						nf = alt;
					setU32(0x88, 0);
					setU32(0x00, nf);
				}
			} else {
				if (u32(0x88) != 5) {
					const bool eq = (u32(0x8C) == 5);
					setU32(0x88, 5);
					flags = eq ? (flags & ~4u) : (flags | 4u);
					setU32(0x00, flags);
				}
			}

			// 1/width, 1/height into PS CB at ps[0x4A]; .z/.w = 0.
			auto* const         rtDesc = reinterpret_cast<void*>(engine::g_mainRTDesc.get());  // &unk_14302BB20
			const std::size_t   o4A = 4u * ps[0x4A];
			const std::uint32_t w = EngineCall<std::uint32_t>(reinterpret_cast<void*>(engine::RTGetWidth.address()), rtDesc);   // FUN_140D74C20
			wf(pDataPS, o4A + 0, 1.0f / static_cast<float>(w));
			const std::uint32_t h = EngineCall<std::uint32_t>(reinterpret_cast<void*>(engine::RTGetHeight.address()), rtDesc);  // FUN_140D74C60
			*reinterpret_cast<std::uint64_t*>(pDataPS + o4A + 8) = 0;
			wf(pDataPS, o4A + 4, 1.0f / static_cast<float>(h));

			// Depth-target SRV cache -> RT dirty (bit0) + PS-SRV masks. rtPool base = &g_renderer.
			auto* const        rtPool = reinterpret_cast<std::uint8_t*>(engine::g_renderer.address());  // 0x143028490
			const std::int32_t dsIdx = EngineCall<std::int32_t>(reinterpret_cast<void*>(engine::GetDepthStencilTargetMain.address()));
			const std::uint64_t depthSRV = dsIdx == -1 ? 0ull :
				*reinterpret_cast<std::uint64_t*>(rtPool + 152u * static_cast<std::size_t>(dsIdx) + 0x2040);
			if (qw(0x150) != depthSRV) {  // unk_143028000
				orU32(0x04, 4);           // unk_143027EB4
				orU32(0x00, 1);
				setQw(0x150, depthSRV);
				WsDsvDirty() = 1;  // unk_1430284C2 (OUT of block); worker cell under MT
			}
			std::uint32_t v27 = u32(0x08);  // unk_143027EB8
			if (u32(0xC4) != 0) {           // unk_143027F74
				v27 |= 4;
				setU32(0xC4, 0);
				setU32(0x08, v27);
			}
			if (u32(0x104) != 0) {  // unk_143027FB4
				setU32(0x104, 0);
				setU32(0x08, v27 | 4);
			}
			if (*engine::g_focusShadowEnable /*byte_141E0DE43*/ && *engine::g_focusShadowCount != 0) {
				const std::int32_t  dsIdx2 = EngineCall<std::int32_t>(reinterpret_cast<void*>(engine::GetDepthStencilTargetMain.address()));
				const std::uint64_t stencilSRV = dsIdx2 == -1 ? 0ull :
					*reinterpret_cast<std::uint64_t*>(rtPool + 152u * static_cast<std::size_t>(dsIdx2) + 0x2048);
				if (qw(0x168) != stencilSRV) {  // unk_143028018
					orU32(0x04, 0x20);
					orU32(0x00, 1);
					setQw(0x168, stencilSRV);
					WsDsvDirty() = 1;  // worker cell under MT
				}
			}
			if (u32(0xA0) != 1) {  // dword_143027F50 -> raster dirty (bit6)
				orU32(0x00, 0x40);
				setU32(0xA0, 1);
			}
		}

		// ---- camera-relative split distances into PS CB ----
		auto* const cam = *engine::g_viewCamera;  // unk_1431D0E68
		const float camNear = *reinterpret_cast<float*>(cam + 0x160);
		const float camFar = *reinterpret_cast<float*>(cam + 0x164);

		if (v2 & 0x200000) {
			auto* const        ssn = *engine::g_shadowSceneNode;                     // shadowSceneNode
			auto* const        sunDL = *reinterpret_cast<std::uint8_t**>(ssn + 0x210);  // sunShadowDirLight
			auto* const        endArr = pDataPS + 4u * ps[0x4B];                     // EndSplitDistances
			auto* const        startArr = pDataPS + 4u * ps[0x4C];                   // StartSplitDistances
			const std::int32_t count = *reinterpret_cast<std::int32_t*>(sunDL + 0x140);  // shadowMapCount
			const float        farNear = camFar * camNear;                          // hoisted (matches asm)
			const float        farMinusNear = camFar - camNear;
			for (std::int32_t i = 0; i < count; ++i) {
				const float end = *reinterpret_cast<float*>(sunDL + 4 * i + 0x5A4);
				wf(endArr, 4u * i, (end * camFar - farNear) / (farMinusNear * end));
				const float start = *reinterpret_cast<float*>(sunDL + 4 * i + 0x598);
				wf(startArr, 4u * i, (start * camFar - farNear) / (start * farMinusNear));
			}
			const std::int32_t lastIdx = count - 1;
			*reinterpret_cast<std::uint32_t*>(endArr + 8) = *reinterpret_cast<std::uint32_t*>(endArr + 4 * lastIdx);
			wf(endArr, 12, static_cast<float>(count));
			wf(startArr, 12, static_cast<float>(*engine::g_shadowFixedCount));  // dword_141867188
			if (*engine::g_copySplitToVS /*byte_141E0DE4C*/) {
				const std::size_t ov = 4u * vs[0x53];
				*reinterpret_cast<std::uint32_t*>(pDataVS + ov + 0) = *reinterpret_cast<std::uint32_t*>(endArr + 0);
				*reinterpret_cast<std::uint32_t*>(pDataVS + ov + 4) = *reinterpret_cast<std::uint32_t*>(endArr + 4);
				*reinterpret_cast<std::uint32_t*>(pDataVS + ov + 8) = *reinterpret_cast<std::uint32_t*>(endArr + 8);
				*reinterpret_cast<std::uint32_t*>(pDataVS + ov + 12) = 0;
			}
			if (static_cast<std::uint32_t>(*engine::g_shadowMode - 2u) <= 1u) {  // dword_141E0DE34
				const float denom = static_cast<float>(*engine::g_poissonDenom);  // unk_143283B90
				const float poisson = *engine::g_poissonRadiusScale;              // fPoissonRadiusScale_141E10670
				wf(pDataPS, 4u * off47 + 8, poisson / denom);
				wf(pDataPS, 4u * off47 + 12, poisson / denom);
			}
		} else {
			wf(pDataPS, 4u * ps[0x4B], *engine::g_fixedSplit);                                 // unk_143283B78
			wf(pDataPS, 4u * ps[0x4B] + 4, static_cast<float>(*engine::g_shadowFixedCount));   // dword_141867188
			wf(pDataPS, 4u * ps[0x4C] + 12, static_cast<float>(*engine::g_shadowFixedCount));
			if (u32(0xA4) != 1) {  // unk_143027F54 -> bit12
				orU32(0x00, 0x1000);
				setU32(0xA4, 1);
			}
			if (static_cast<std::uint32_t>(*engine::g_shadowMode - 2u) <= 1u) {
				const float denom = static_cast<float>(*engine::g_poissonDenom);
				const float poisson = *engine::g_poissonRadiusScale;
				const float v42 = poisson / denom;
				wf(pDataPS, 4u * off47 + 8, v42);
				wf(pDataPS, 4u * off47 + 12, poisson * v42);
			}
		}

		// ---- focus-shadow fade weights into PS CB (offset ps[0x4D]) ----
		auto* const focusArr = pDataPS + 4u * ps[0x4D];
		const std::uint32_t n = *engine::g_focusShadowCount;                                    // unk_1431D0FB8 (._used)
		const float*        fdata = *engine::g_focusShadowData;                                 // *(unk_1431D0FA8) heap array (stride 16B)
		// The engine reads fdata guarded only by (focusArr && n>0), relying on the invariant
		// n>0 => fdata valid. That holds in steady state but is transiently violated during
		// load-screen shadow rendering (a BSShadowParabolicLight pass sees n>0 with fdata a
		// non-canonical pointer, mid-init). Guard on a canonical user-space pointer so the load
		// transient is skipped; in steady state fdata is always a valid heap pointer, so this
		// never changes behavior and stays byte-exact under the parity gate.
		const bool fdataValid = fdata && reinterpret_cast<std::uintptr_t>(fdata) < 0x0000800000000000ull;
		if (focusArr && fdataValid) {
			const float        maxDistSq = (*engine::g_maxFocusDist) * (*engine::g_maxFocusDist);  // fMaxFocusShadowMapDistance
			for (std::uint32_t i = 0; i < n; ++i) {
				const float thresh = *engine::g_fadeFracStart * maxDistSq;  // fFadingFracStart_141E106A0
				const float d = fdata[4 * i];
				const float wv = (d >= thresh) ? (maxDistSq - d) / (maxDistSq - thresh) : 1.0f;
				wf(focusArr, 4u * i, wv);
			}
		}
	} else {
		// (v2 & 0x1E00000)==0: only the biased-depth PS constants (powf-interpolated pair).
		if ((v2 & 0x40000) && v6) {
			float*      scratch = reinterpret_cast<float*>(&mapped);  // reuses var_C8 (D3D11_MAPPED_SUBRESOURCE)
			const float base = *engine::g_biasBase;                   // MEMORY[0x143283B7C]
			const float v53 = base * 0.2f;
			const float v54 = base - 5.0f;
			for (int i = 0; i < 2; ++i) {
				const float t = (static_cast<float>(i) + 1.0f) * 0.5f;
				const float p = EngineCall<float>(reinterpret_cast<void*>(engine::PowF.address()), v53, t);  // thunk_powf_14134BEAC
				scratch[i] = (p * 5.0f * 0.5f) + ((v54 * t + 5.0f) * 0.5f);
			}
			std::memcpy(pDataPS + 4u * ps[0x4B], &mapped, 16);  // movups: 2 computed + 2 map-struct tail bytes
		}
	}

	// ---- tail: blend enable + shadow-radius (VS CB) + alpha-CB dirty ----
	if ((v2 & 0x20004000) == 0x4000) {
		if (u32(0xA8) != 0) {  // unk_143027F58 -> blend dirty (bit7)
			orU32(0x00, 0x80);
			setU32(0xA8, 0);
		}
		if (v2 & 0x10000) {
			const std::size_t o = 4u * vs[0x54];
			wf(pDataVS, o + 0, 1.0f / *engine::g_shadowRadius);  // ShadowRadiusMaybe_141E10B78
			wf(pDataVS, o + 4, *engine::g_shadowSign);           // ShadowSign_141E10B7C
		}
	}
	if (v2 & 0x100000) {
		if (u8b(0xB4) != 0) {  // unk_143027F64 -> alpha-CB dirty (bit8)
			orU32(0x00, 0x100);
			setU8(0xB4, 0);
		}
	}

	// ---- Unmap + bind the PerTechnique CBs to slot 0 ----
	if (vsBuf)
		EngineCallV<15, void>(ctx, vsBuf, 0u);  // Unmap VS CB
	if (v6) {
		if (psBuf)
			EngineCallV<15, void>(ctx, psBuf, 0u);   // Unmap PS CB
		EngineCallV<7, void>(ctx, 0u, 1u, &vsBuf);   // VSSetConstantBuffers(slot0, 1, &vsBuf)
		EngineCallV<16, void>(ctx, 0u, 1u, &psBuf);  // PSSetConstantBuffers(slot0, 1, &psBuf)
	} else {
		EngineCallV<7, void>(ctx, 0u, 1u, &vsBuf);   // VSSetConstantBuffers(slot0, 1, &vsBuf)
	}

	return v59;
}

// BSUtilityShader::SetupGeometry (vf6, 1.5.97 0x14130EC70) reimplemented against a caller-supplied
// context + the two PerGeometry constant buffers + the 0x5D8 state block S. Fills the PerGeometry
// VS CB (slot 2) and PS CB (slot 2) from the pass geometry / shader-property / shadow-light data.
// The register indices come from the shared VS/PS shader objects (block+0x348 / +0x350); only the
// buffers are private. Every D3D11 call routes through EngineCallV so the compare recorder captures
// it identically; the pure-CPU engine helpers (matrix build/transpose, shadow fill, scissor mutate,
// accumulator/property accessors) are EngineCall'd and issue no context call. At N=1 vsCB/psCB are
// the shader-owned CBs (VS+0x38 / PS+0x30) so the fill is byte-identical.
void FlushSetupGeometryReplica(ID3D11DeviceContext* ctx, ID3D11Buffer* vsCB, ID3D11Buffer* psCB,
                               std::uint8_t* S, RE::BSShader* a_shader, RE::BSRenderPass* a_pass)
{
	using u8v = std::uint8_t;
	using u32 = std::uint32_t;
	using i32 = std::int32_t;

	auto* shader = reinterpret_cast<u8v*>(a_shader);
	auto* pass   = reinterpret_cast<u8v*>(a_pass);

	const auto F  = [](u8v* p, std::size_t o) -> float { return *reinterpret_cast<float*>(p + o); };
	const auto U  = [](u8v* p, std::size_t o) -> u32 { return *reinterpret_cast<u32*>(p + o); };
	const auto PP = [](u8v* p, std::size_t o) -> u8v* { return *reinterpret_cast<u8v**>(p + o); };
	const auto RELf = [](std::uintptr_t a) -> float { return *reinterpret_cast<float*>(a); };

	// Current VS/PS shader objects (the register-index source), from the state block.
	u8v* VS = *reinterpret_cast<u8v**>(S + 0x348);   // *0x1430281F8
	u8v* PS = *reinterpret_cast<u8v**>(S + 0x350);   // *0x143028200

	u8v* v9 = PS ? PS + 0x30 : nullptr;              // PS CB slot object (SetupShadowLightParameters arg3)

	// ---- Map the two PerGeometry CBs (WRITE_DISCARD, subresource 0). ----
	D3D11_MAPPED_SUBRESOURCE mapped{};
	u8v* vsMapped = nullptr;
	u8v* psMapped = nullptr;
	if (vsCB) {
		EngineCallV<14, HRESULT>(ctx, reinterpret_cast<ID3D11Resource*>(vsCB), 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped);  // Map
		vsMapped = reinterpret_cast<u8v*>(mapped.pData);
		if (VS)
			*reinterpret_cast<void**>(VS + 0x40) = vsMapped;  // engine v6[1]: store map ptr on the shader
	}
	if (PS && psCB) {
		EngineCallV<14, HRESULT>(ctx, reinterpret_cast<ID3D11Resource*>(psCB), 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped);  // Map
		psMapped = reinterpret_cast<u8v*>(mapped.pData);
		// BSRenderPass::SetupShadowLightParameters (and other EngineCall'd fill helpers) write the
		// PS PerGeometry CB via *(ps+0x38) -- the map pointer the engine stashes on the shader object
		// (v9->_refCount). The replica maps its OWN buffer, so it must store the map pointer here or
		// the helper writes the stale engine-window buffer (missing the shadow-light matrix + bias).
		*reinterpret_cast<void**>(PS + 0x38) = psMapped;
	}

	// CB destinations: mapped + 4 * (register-index byte on the shader object). CbOrScratch
	// absorbs the load-screen transient where a CB is momentarily unmapped (null base); in
	// steady state vsMapped/psMapped are the real mapped pointers so this is byte-exact.
	const auto vcb = [&](std::size_t shOff) -> u8v* { return CbOrScratch(vsMapped) + 4u * static_cast<std::size_t>(VS[shOff]); };
	const auto pcb = [&](std::size_t shOff) -> u8v* { return CbOrScratch(psMapped) + 4u * static_cast<std::size_t>(PS[shOff]); };

	auto& DW = *reinterpret_cast<u32*>(S + 0);       // dirty word (MEMORY[0x143027EB0], block+0)

	u8v* geom           = *reinterpret_cast<u8v**>(pass + 0x10);  // a2->geometry
	u8v* shaderProperty = *reinterpret_cast<u8v**>(pass + 0x08);  // a2->shaderProperty
	u8v* v88            = nullptr;                                 // effect-shader data (alpha-test-ref source)

	const u32 tech = WsTechFlags(shader);       // technique flag (a1->_pad_20[112]); worker cell under MT

	// ================= world matrix -> VS CB[VS+0x50] (unless tech & 4) =================
	if ((tech & 4) == 0) {
		alignas(16) float m44[16];
		if (EngineCallV<16, void*>(geom)) {          // AsParticlesGeom (geom vfunc 16)
			// Modified NiTransform: same rotate/scale, translate = world * modelBound.center.
			alignas(16) float mt[16];
			u8v*  gw = geom + 0x7C;                    // world (NiTransform)
			float r[9]; std::memcpy(r, gw, 36);        // rotate 3x3
			std::memcpy(mt, r, 36);
			const float sc = F(gw, 0x30);              // world.scale (geom+0xAC)
			const float cx = F(geom, 0x110), cy = F(geom, 0x114), cz = F(geom, 0x118);  // modelBound.center
			mt[9]  = F(gw, 0x24) + sc * (r[0] * cx + r[1] * cy + r[2] * cz);  // translate.x + s*(rot*center)
			mt[10] = F(gw, 0x28) + sc * (r[3] * cx + r[4] * cy + r[5] * cz);
			mt[11] = F(gw, 0x2C) + sc * (r[6] * cx + r[7] * cy + r[8] * cz);
			mt[12] = sc;
			EngineCall<void>(reinterpret_cast<void*>(engine::SG_BuildMatrix.address()), static_cast<void*>(m44), static_cast<const void*>(mt));   // FUN_1412c3440
		} else {
			EngineCall<void>(reinterpret_cast<void*>(engine::SG_BuildMatrix.address()), static_cast<void*>(m44), static_cast<const void*>(geom + 0x7C));  // FUN_1412c3440(&world)
		}
		EngineCall<void>(reinterpret_cast<void*>(engine::SG_MatrixTranspose.address()), static_cast<void*>(vcb(0x50)), static_cast<const void*>(m44));  // D3DXMatrixTranspose
	}

	if (tech & 0x1E00000) {
		// ============================ SHADOW PATH ============================
		u8v** sceneLights = pass[0x1F] ? *reinterpret_cast<u8v***>(pass + 0x38) : nullptr;  // numLights ? a2->sceneLights : 0
		u8v*  v30 = *sceneLights;                                                           // first shadow light

		EngineCall<void>(reinterpret_cast<void*>(engine::SG_ShadowFill.address()), static_cast<void*>(pass), 0);                 // FUN_14130f960 (pure CPU)
		// SG_SetupShadowLightParams reads ONLY arg3+8 (the PS PerGeometry CB base) and writes the CB.
		// v9 = PS+0x30, whose +8 = PS+0x38 = psMapped -> byte-exact.
		EngineCall<void>(reinterpret_cast<void*>(engine::SG_SetupShadowLightParams.address()), static_cast<void*>(pass), 0, static_cast<void*>(v9));  // 0x14130fbe0 (pure CPU)

		// lodFade constant -> VS CB[VS+0x55].x and PS CB[PS+0x48].z (raw dword copy)
		const u32 v32 = v30[0x63] ? *reinterpret_cast<u32*>(engine::SG_c283B88.address()) : 1287568416u;  // dword_141667CD0
		*reinterpret_cast<u32*>(vcb(0x55))       = v32;
		*reinterpret_cast<u32*>(pcb(0x48) + 8)   = v32;

		// falloff -> PS CB[PS+0x48].x
		if (tech & 0x1800000) {
			u8v* lgtR = PP(v30, 0x48);
			if (IsCanonicalPtr(lgtR))
				*reinterpret_cast<u32*>(pcb(0x48)) = U(lgtR, 0x128);   // v30->light->radius.x
		} else if (tech & 0x400000) {
			*reinterpret_cast<u32*>(pcb(0x48)) = U(v30, 0x568);            // v30->falloff
		}

		if (!(tech & 0x200000)) {
			// spot/point sub-branch. FUN_140d70100 builds a D3D11_RECT and calls RSSetScissorRects.
			EngineCall<void>(reinterpret_cast<void*>(engine::SG_ScissorFromBBox.address()),
				reinterpret_cast<void*>(engine::g_renderer.address()),
				U(v30, 0x544), U(v30, 0x550), U(v30, 0x548), U(v30, 0x54C));   // FUN_140d70100(scissor,left,bottom,right,top)

			// camera-relative view-depth falloff -> VS CB[VS+0x55].z. cam (*0x1431D0F88), the light
			// (v30->light._ptr) and the view frustum (*0x1431D0E68) are all engine objects that can
			// be momentarily non-canonical at the load boundary; skip the read when any is.
			u8v* cam = *reinterpret_cast<u8v**>(engine::SG_pCamNode.address());        // *0x1431D0F88
			u8v* lgt = PP(v30, 0x48);
			u8v* vf  = *reinterpret_cast<u8v**>(engine::SG_pViewFrustumObj.address());  // *0x1431D0E68
			if (IsCanonicalPtr(cam) && IsCanonicalPtr(lgt) && IsCanonicalPtr(vf)) {
				// Bit-exact SSE-scalar transcription of the falloff at 0x14130F02A-F0F7. Plain C
				// diverged by 1 ULP because MSVC auto-vectorizes/reassociates the dot product; the
				// _ss intrinsics pin each op to the engine's exact subss/mulss/addss/divss sequence.
				const auto    ld = [](float f) { return _mm_set_ss(f); };
				const __m128  dx = _mm_sub_ss(ld(F(lgt, 0xA0)), ld(F(cam, 0xA0)));
				const __m128  dy = _mm_sub_ss(ld(F(lgt, 0xA4)), ld(F(cam, 0xA4)));
				const __m128  dz = _mm_sub_ss(ld(F(lgt, 0xA8)), ld(F(cam, 0xA8)));
				__m128        distv = _mm_mul_ss(ld(F(cam, 0x88)), dy);                  // Data[3]*dy
				distv = _mm_add_ss(distv, _mm_mul_ss(ld(F(cam, 0x7C)), dx));             // + Data[0]*dx
				distv = _mm_add_ss(distv, _mm_mul_ss(ld(F(cam, 0x94)), dz));             // + Data[6]*dz
				distv = _mm_add_ss(distv, ld(F(lgt, 0x128)));                            // + light->radius.x
				__m128        v36 = ld(v30[0x63] ? RELf(engine::SG_c283B7C.address()) : 10000.0f);
				if (_mm_comige_ss(v36, distv))                                           // v36 >= distv ? distv : v36
					v36 = distv;
				const __m128  farv = ld(F(vf, 0x164));
				const __m128  nearv = ld(F(vf, 0x160));
				const __m128  invr = _mm_div_ss(_mm_set_ss(1.0f), _mm_sub_ss(farv, nearv));  // 1/(Far-Near)
				const __m128  t1 = _mm_mul_ss(_mm_mul_ss(farv, v36), invr);
				const __m128  t2 = _mm_mul_ss(_mm_mul_ss(farv, nearv), invr);
				float         v37 = _mm_cvtss_f32(_mm_div_ss(_mm_sub_ss(t1, t2), v36));
				v37 = (v37 <= 1.0f) ? ((v37 < 0.0f) ? 0.0f : v37) : 1.0f;
				*reinterpret_cast<float*>(vcb(0x55) + 8) = v37;
			}
		}

		if (pass[0x1C] == 10) {  // accumulationHint == 10: paired fade token S+0x90/0x94
			u8v* fn = PP(shaderProperty, 0x60);
			const float fade = (pass[0x1E] & 0x80) ? F(fn, 0x14C) : F(fn, 0x130);
			const u8v   kb   = static_cast<u8v>(static_cast<i32>(fade * 31.0f));  // cvttss2si (truncate) -> low byte
			if (U(S, 0x90) != 11u || U(S, 0x94) != static_cast<u32>(kb)) {
				DW |= 8;
				*reinterpret_cast<u32*>(S + 0x90) = 11;
				*reinterpret_cast<u32*>(S + 0x94) = kb;
			}
		}
	} else if ((tech & 0x20004000) != 0x4000) {
		// ======================= MATERIAL / EFFECT PATH =======================
		if (tech & 0x100000) {
			v88 = PP(shaderProperty, 0x68);            // effectData
			u8v* blockOut = v88 ? PP(v88, 0x20) : nullptr;
			if (v88 && blockOut) {
				u8v* rd = PP(blockOut, 0x48);
				std::uintptr_t srv = rd ? *reinterpret_cast<std::uintptr_t*>(rd + 0x10) : 0;  // texture SRV
				if (*reinterpret_cast<std::uintptr_t*>(S + 0x140) != srv) {   // qword_143027FF0
					*reinterpret_cast<u32*>(S + 4) |= 1;                      // unk_143027EB4
					*reinterpret_cast<std::uintptr_t*>(S + 0x140) = srv;
				}
				// sampler/addressing state (S+8 accumulator; S+0xBC,0xFC address modes)
				u32 eb8 = U(S, 8);                                            // unk_143027EB8
				if (U(S, 0xBC) != 3) { eb8 |= 1; *reinterpret_cast<u32*>(S + 0xBC) = 3; *reinterpret_cast<u32*>(S + 8) = eb8; }
				if (U(S, 0xFC) != 3) { eb8 |= 1; *reinterpret_cast<u32*>(S + 0xFC) = 3; *reinterpret_cast<u32*>(S + 8) = eb8; }
				if (U(S, 0xA0) != 1) { DW |= 0x40; *reinterpret_cast<u32*>(S + 0xA0) = 1; }   // dword_143027F50
				if (U(S, 0x88) != 3) {                                        // unk_143027F38 (+ cmov on S+0x8C)
					*reinterpret_cast<u32*>(S + 0x88) = 3;
					const u32 with4 = DW | 4, without4 = DW & ~4u;
					DW = (U(S, 0x8C) != 3) ? with4 : without4;
				}
				const u32 tok = U(S, 0xB0);                                   // unk_143027F60
				WsShadowToken() = tok;                                        // dword_141E10660 (worker-private cache)
				if (tok) { DW |= 0x80; *reinterpret_cast<u32*>(S + 0xB0) = 0; }
			}
		}

		if ((tech & 0x1E00100) == 0x100) {                                   // world matrix -> VS CB[VS+0x50]
			alignas(16) float m44b[16];
			EngineCall<void>(reinterpret_cast<void*>(engine::SG_BuildMatrix.address()), static_cast<void*>(m44b), static_cast<const void*>(geom + 0x7C));  // FUN_1412c3440
			EngineCall<void>(reinterpret_cast<void*>(engine::SG_MatrixTranspose.address()), static_cast<void*>(vcb(0x50)), static_cast<const void*>(m44b));  // transpose
		}

		if ((tech & 0x1200) == 0x1200) {
			// stencil sub-branch. NOTE: FUN_140d6fcf0 MUTATES the renderer scissor global (0x143028490).
			if (U(S, 0x90) != 1u || U(S, 0x94) != 0xFFu) { DW |= 8; *reinterpret_cast<u32*>(S + 0x90) = 1; *reinterpret_cast<u32*>(S + 0x94) = 0xFF; }
			if (U(S, 0xB0)) { DW |= 0x80; *reinterpret_cast<u32*>(S + 0xB0) = 0; }
			if (U(S, 0x88) != 0) {
				*reinterpret_cast<u32*>(S + 0x88) = 0;
				const u32 with4 = DW | 4, without4 = DW & ~4u;
				DW = (U(S, 0x8C) != 0) ? with4 : without4;
			}
			*reinterpret_cast<u32*>(vcb(0x57)) = *reinterpret_cast<u32*>(engine::SG_stencilVal014.address());  // dword_141E0E014 -> VS CB[VS+0x57]
			EngineCall<void>(reinterpret_cast<void*>(engine::SG_ScissorApply.address()),
				reinterpret_cast<void*>(engine::g_renderer.address()), static_cast<void*>(PS));  // FUN_140d6fcf0(scissor, PS)
		} else {
			if (tech & 0x200) {
				u8v* d52 = vcb(0x52);
				if (tech & 0x400) {
					alignas(16) float mvw[16];
					EngineCall<void>(reinterpret_cast<void*>(engine::SG_WorldToView.address()), static_cast<void*>(geom + 0x7C), 0, static_cast<void*>(mvw));  // FUN_140d42c50
					u8v* accum = reinterpret_cast<u8v*>(EngineCall<void*>(reinterpret_cast<void*>(engine::SG_GetAccumulator.address())));                       // GetCurrentAccumulator
					EngineCall<void>(reinterpret_cast<void*>(engine::SG_Vec3TransformCoord.address()), static_cast<void*>(d52), static_cast<const void*>(accum + 0x16C), static_cast<const void*>(mvw));  // D3DXVec3TransformCoord
				}
				*reinterpret_cast<u32*>(d52 + 0xC) = U(shaderProperty, 0x104);
			}

			if (PS) {
				if (tech & 0x20000) {
					// S+0xB0 conditional clear (spatial-fade gate)
					if (*reinterpret_cast<u8v*>(engine::SG_flagDE4C.address()) &&
						static_cast<u32>(*reinterpret_cast<u32*>(engine::SG_mode1D0E28.address()) - 16u) <= 1u &&
						U(S, 0xB0)) {
						DW |= 0x80; *reinterpret_cast<u32*>(S + 0xB0) = 0;
					}
					float extraParam = static_cast<float>(static_cast<u32>(pass[0x1D]));
					if (*reinterpret_cast<i32*>(engine::SG_modeDF94.address()) == 10) {   // decal-alpha hash lookup
						u8v* fadeNode = PP(shaderProperty, 0x60);
						u8v* accum    = reinterpret_cast<u8v*>(EngineCall<void*>(reinterpret_cast<void*>(engine::SG_GetAccumulator.address())));
						u8v* map      = accum + 0xD0;
						const u32 idx = EngineCallV<1, u32>(map, static_cast<void*>(fadeNode));         // (map)->vtbl[1](map, fadeNode)
						u8v* node     = *reinterpret_cast<u8v**>(*reinterpret_cast<std::uintptr_t*>(map + 0x10) + 8ull * idx);
						i32  found    = 0;
						while (node) {
							if (EngineCallV<2, bool>(map, static_cast<void*>(fadeNode), *reinterpret_cast<void**>(node + 8))) {  // (map)->vtbl[2](map, fadeNode, key)
								found = *reinterpret_cast<i32*>(node + 0x10);
								break;
							}
							node = *reinterpret_cast<u8v**>(node);
						}
						extraParam = (static_cast<float>(static_cast<u32>(found)) * 255.0f) / static_cast<float>(U(accum, 0xF0));
					}
					const float B = 0.0078125f;                          // 1/128
					float v61 = (128.0f - extraParam) * B; v61 = (v61 <= 1.0f) ? ((v61 < 0.0f) ? 0.0f : v61) : 1.0f;
					const float v62 = extraParam - 128.0f;
					float v63 = (128.0f - std::fabs(v62)) * B; v63 = (v63 <= 1.0f) ? ((v63 < 0.0f) ? 0.0f : v63) : 1.0f;
					float v64 = v62 * RELf(engine::SG_recip127.address()); v64 = (v64 <= 1.0f) ? ((v64 < 0.0f) ? 0.0f : v64) : 1.0f;
					u8v* d42 = pcb(0x42);
					*reinterpret_cast<float*>(d42 + 0)   = v64;
					*reinterpret_cast<float*>(d42 + 4)   = v61;
					*reinterpret_cast<float*>(d42 + 8)   = v63;
					*reinterpret_cast<u32*>(d42 + 0xC)   = 0x3F800000u;   // 1.0
				} else if (tech & 0x80000) {
					u8v* accum = reinterpret_cast<u8v*>(EngineCall<void*>(reinterpret_cast<void*>(engine::SG_GetAccumulator.address())));
					u8v* d42 = pcb(0x42);
					*reinterpret_cast<u32*>(d42 + 0xC) = 0x3F800000u;     // 1.0
					*reinterpret_cast<u32*>(d42 + 0)   = U(accum, 0x118); // color.r
					*reinterpret_cast<u32*>(d42 + 8)   = U(accum, 0x120); // color.b
					*reinterpret_cast<u32*>(d42 + 4)   = U(accum, 0x11C); // color.g
				}
			}
		}
	}

	// =========================== wind branch (tech & 0x4000000) ===========================
	if (tech & 0x4000000) {
		u8v* d56 = vcb(0x56);
		u8v* fadeNode = PP(shaderProperty, 0x60);
		u8v* leaf = nullptr;
		float v74 = 0.0f;
		if (fadeNode != *reinterpret_cast<u8v**>(engine::SG_pFadeExclude.address()) && fadeNode) {
			leaf = reinterpret_cast<u8v*>(EngineCallV<63, void*>(fadeNode));  // AsLeafAnimNode (vfunc 63)
			if (leaf)
				v74 = F(leaf, 0x164) * 6.0f;
		}
		*reinterpret_cast<float*>(d56 + 0) = v74;
		*reinterpret_cast<u32*>(d56 + 4)   = U(*reinterpret_cast<u8v**>(engine::g_shadowSceneNode.address()), 0x304);  // windMagnitude
		float v75 = 0.0f;
		if (leaf) {
			float x = F(leaf, 0x158);
			i32   i = *reinterpret_cast<i32*>(&x);
			i = 0x5F3759DF - (i >> 1);
			float y = *reinterpret_cast<float*>(&i);
			v75 = (1.5f - (x * 0.5f) * y * y) * y * x;   // x * rsqrt(x)
		}
		const float wmin = RELf(engine::SG_windFadeMin.address());
		const float wmax = RELf(engine::SG_windFadeMax.address());
		float v76 = leaf ? F(leaf, 0x15C) : 1.0f;
		const float t = (v75 - wmin) / (wmax - wmin);
		const float w = (1.0f - t) * v76;
		const float v27 = (w >= 0.0f) ? w : 0.0f;
		if (!(v76 < v27))
			v76 = v27;
		*reinterpret_cast<float*>(d56 + 8) = v76;
		*reinterpret_cast<u32*>(d56 + 0xC) = leaf ? U(leaf, 0x160) : 0x3F800000u;  // 1.0 default
	}

	// ================= alpha / fade PS-CB fill + unmap + bind =================
	u8v* niProp = reinterpret_cast<u8v*>(EngineCall<RE::NiAlphaProperty*>(reinterpret_cast<void*>(engine::GetNiProperty.address()), a_pass));  // BSRenderPass::GetNiProperty

	if (PS) {
		bool doFill;
		if (niProp && (*reinterpret_cast<u8v*>(niProp + 0x30) & 1))     doFill = true;
		else if (tech & 0x80)                                          doFill = true;
		else if ((tech & 0x14000) != 0x10000)                          doFill = false;  // -> straight to bind
		else                                                           doFill = true;

		if (doFill) {
			u8v* d40 = pcb(0x40);
			if (tech & 0x20000000) {                                    // shaderProperty color -> PS CB[PS+0x44]
				u8v* col = PP(shaderProperty, 0x88);
				u8v* d44 = pcb(0x44);
				if (col) {
					*reinterpret_cast<u32*>(d44 + 0) = U(col, 0);
					*reinterpret_cast<u32*>(d44 + 4) = U(col, 4);
					*reinterpret_cast<u32*>(d44 + 8) = U(col, 8);
				} else {
					*reinterpret_cast<u32*>(d44 + 0) = 0x3F800000u;
					*reinterpret_cast<u32*>(d44 + 4) = 0x3F800000u;
					*reinterpret_cast<u32*>(d44 + 8) = 0x3F800000u;
				}
				*reinterpret_cast<u32*>(d44 + 0xC) = U(shaderProperty, 0x30);  // alpha
			}
			// alpha-test ref -> PS CB[PS+0x40].x
			if (v88) {
				*reinterpret_cast<float*>(d40) = static_cast<float>(static_cast<u32>(v88[0x7C])) * RELf(engine::SG_recip255.address());
			} else if (niProp) {
				if (*reinterpret_cast<u8v*>(niProp + 0x30) & 1) {
					*reinterpret_cast<u32*>(d40) = 0x3F7EFEFFu;
				} else {
					const u8v mode = niProp[0x32];
					const float v85 = static_cast<float>(static_cast<u32>(mode)) * RELf(engine::SG_recip255.address()) + RELf(engine::SG_alphaBias.address());
					*reinterpret_cast<float*>(d40) = v85;
					if (mode == 4)
						*reinterpret_cast<float*>(d40) = v85 + RELf(engine::SG_recip255.address());
				}
			}
			// fade -> PS CB[PS+0x40].w
			if ((tech & 0x14000) == 0x10000) {
				const std::uint64_t impl = *reinterpret_cast<std::uint64_t*>(shaderProperty + 0x38);
				if ((impl & 0x4000ull) || (impl & 0x400000000000ull)) {
					*reinterpret_cast<u32*>(d40 + 0xC) = U(shaderProperty, 0x108);
				} else {
					u8v* fn = PP(shaderProperty, 0x60);
					*reinterpret_cast<float*>(d40 + 0xC) = F(fn, 0x130) * F(shaderProperty, 0x30);  // currentFade * alpha
				}
				if (pass[0x1E] & 0x80) {
					u8v* fn = PP(shaderProperty, 0x60);
					*reinterpret_cast<float*>(d40 + 0xC) = F(fn, 0x14C) * F(d40, 0xC);
				}
			}
		}

		if (vsCB) EngineCallV<15, void>(ctx, reinterpret_cast<ID3D11Resource*>(vsCB), 0u);  // Unmap VS CB
		if (psCB) EngineCallV<15, void>(ctx, reinterpret_cast<ID3D11Resource*>(psCB), 0u);  // Unmap PS CB
		ID3D11Buffer* vb = vsCB; EngineCallV<7,  void>(ctx, 2u, 1u, &vb);   // VSSetConstantBuffers slot 2
		ID3D11Buffer* pb = psCB; EngineCallV<16, void>(ctx, 2u, 1u, &pb);   // PSSetConstantBuffers slot 2
		return;
	}

	// ---- no pixel shader: unmap + bind VS only ----
	if (vsCB) EngineCallV<15, void>(ctx, reinterpret_cast<ID3D11Resource*>(vsCB), 0u);  // Unmap VS CB
	{ ID3D11Buffer* vb = vsCB; EngineCallV<7, void>(ctx, 2u, 1u, &vb); }                 // VSSetConstantBuffers slot 2
}
}  // anonymous namespace (spans CbOrScratch/IsCanonicalPtr + the two FlushSetup*Replica reimpls)

void UtilityPassReplica::DrawTriShapeReplica(void* a_rendererData, std::uint32_t a_startIndex, std::uint32_t a_triCount)
{
	// BSGraphics::Renderer::DrawTriShape (0x140D6BFE0), replicated exactly: vertex-desc
	// and topology change detection into the dirty word, state flush, then unconditional
	// IB/VB binds and the indexed draw (the engine does NOT cache the IB/VB binds here).
	auto* rd = static_cast<engine::TriShapeData*>(a_rendererData);
	// vertexDesc (S+0x340), state flags (S+0), topology (S+0x358) live in the engine render-state block.
	auto* const   S = WsBlock();
	auto&         vertexDesc = *reinterpret_cast<std::uint64_t*>(S + 0x340);
	auto&         stateFlags = *reinterpret_cast<std::uint32_t*>(S + 0);
	auto&         topology = *reinterpret_cast<std::uint32_t*>(S + 0x358);
	if (vertexDesc != rd->vertexDesc) {
		vertexDesc = rd->vertexDesc;
		stateFlags |= 0x400;  // DIRTY_VERTEX_DESC
	}
	if (topology != 4 /*D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST*/) {
		topology = 4;
		stateFlags |= 0x800;  // DIRTY_PRIMITIVE_TOPO
	}
	auto* ctx = WsCtx();
	EngineCall<void>(reinterpret_cast<void*>(engine::SetDirtyStates.address()), false);

	const UINT stride = static_cast<UINT>((4 * rd->vertexDesc) & 0x3C);
	const UINT offset = 0;
	EngineCallV<19, void>(ctx, rd->indexBuffer, DXGI_FORMAT_R16_UINT, 0u);   // IASetIndexBuffer
	EngineCallV<18, void>(ctx, 0u, 1u, &rd->vertexBuffer, &stride, &offset); // IASetVertexBuffers
	EngineCallV<12, void>(ctx, 3u * a_triCount, a_startIndex, 0);            // DrawIndexed
}

// ===================================================================================================
// Shadow instancing (the FPS lever): render a set of captured static shadow passes as one
// DrawIndexedInstanced per unique (mesh, technique) group instead of one DrawIndexed per object.
//
// Design (see docs/development/utility-pass-re.md + the shadow-full-ownership memo):
//  * Each object's World matrix is byte-identical to the b2 PerGeometry World the engine builds for
//    that object (SG_BuildMatrix on geom+0x7C, then D3DXMatrixTranspose). It is packed into a
//    per-instance vertex stream as four R16G16B16A16_FLOAT rows (32 bytes) -- exactly the engine's
//    grass-instance layout (VA_INSTANCEDATA, attr 9) that the INSTANCED Utility VS consumes at
//    TEXCOORD4-7 (slot 1, PER_INSTANCE).
//  * The instanced input layout is created by the engine's own ILCreate, keyed by
//    (renderStateVertexDesc & VS.vertexDesc): setting renderStateVertexDesc = meshDesc | VA_INSTANCEDATA
//    and binding the INSTANCED VS (whose vertexDesc carries VA_INSTANCEDATA with 0xF wildcard offset
//    nibbles) yields POSITION at slot 0 (mesh's real offset) + the 4 instance rows at slot 1.
//  * Per-group state reuse: run BeginTechnique (FlushSetupTechniqueReplica) so the engine binds the
//    depth PS + blend/depth/raster + PerTechnique CB, then OVERRIDE only the bound VS with the
//    INSTANCED permutation. b2 (PerGeometry) is intentionally NOT filled -- World comes from the
//    instance stream. Only whole-TRISHAPE (geom+0x150==3), non-skinned, non-alpha-test passes are
//    instanced here; everything else stays engine-rendered inline.
//
// MUST be called on the render thread while the shadow map's RT/DSV/viewport are still bound. On exit
// the technique/material caches are invalidated so the engine re-establishes its own VS on the next
// pass (otherwise a cache hit would draw an engine pass through the still-bound INSTANCED VS).

void UtilityPassReplica::RenderShadowInstanced(RE::BSRenderPass* const* a_passes, const std::uint32_t* a_techniques,
	std::uint32_t a_count, std::uint32_t a_renderFlags)
{
	(void)a_renderFlags;
	if (a_count == 0)
		return;
	auto* ctx = globals::d3d::context;  // render thread -> immediate context
	auto* S = reinterpret_cast<std::uint8_t*>(engine::S_base.address());

	// NOTE: the engine's SetupGeometry builds b2 World = transpose(SG_BuildMatrix(geom+0x7C)) with NO
	// CameraPosAdjust subtraction (UtilityPassReplica.cpp ~2338-2342); our instance pack does the same, so
	// instWorld already equals b2 byte-exact. The shadow VS uses absolute World + the absolute per-light
	// CameraViewProj -- do NOT make instWorld light-relative (that breaks the match).

	// ---- group by (rendererData, technique): same mesh + technique => one instanced draw ----
	struct Group
	{
		engine::TriShapeData*            rd;
		std::uint32_t                    tech;
		std::uint8_t                     fade;      // group fade key (uniform within the group)
		std::uint32_t                    baseInst;  // start slot in the shared instance VB
		std::vector<RE::BSRenderPass*>   passes;
	};
	static std::vector<Group>                             groups;    // persistent: reuse pass-vector allocations
	static std::unordered_map<std::uint64_t, std::size_t> index;
	for (auto& g : groups)
		g.passes.clear();
	std::size_t liveGroups = 0;  // groups[0..liveGroups) are this map's
	index.clear();
	if (groups.capacity() < a_count)
		groups.reserve(a_count);
	index.reserve(a_count);
	for (std::uint32_t i = 0; i < a_count; ++i) {
		auto* geom = reinterpret_cast<std::uint8_t*>(a_passes[i]->geometry);
		if (geom[0x150] != 3) {  // whole-TRISHAPE only (sub-index / skinned excluded)
			continue;
		}
		auto* rd = *reinterpret_cast<engine::TriShapeData**>(geom + 0x138);
		if (!rd) {
			continue;
		}

		// Per-object shadow FADE (distance crossfade): SetupGeometry, for accumulationHint==10 casters,
		// selects depth-stencil variant 11 (a stencil-dither fade state) with stencil-ref = fade*31. That
		// ref is per-object and lives in the pipeline state, so it CANNOT vary within one DrawIndexedInstanced
		// -- every instance inherits passes[0]'s fade. Mixing fade levels in a group would dither-drop the
		// whole batch's depth (ref<31 => sparse depth => under-occlusion => interiors flood with light).
		// Fold the fade byte into the group key so each instanced draw is uniform-fade (fade==31 solids --
		// the vast majority -- still batch together; the thin fading shell forms its own small groups).
		std::uint8_t fadeKey = 0xFFu;  // sentinel: non-fading caster (accumulationHint != 10 => solid, ref 255)
		{
			auto* pb = reinterpret_cast<std::uint8_t*>(a_passes[i]);
			if (pb[0x1C] == 10u) {  // accumulationHint
				auto* sp = *reinterpret_cast<std::uint8_t**>(pb + 0x08);  // shaderProperty
				auto* fn = sp ? *reinterpret_cast<std::uint8_t**>(sp + 0x60) : nullptr;
				if (fn) {
					const float fade = (pb[0x1E] & 0x80) ? *reinterpret_cast<float*>(fn + 0x14C) :
					                                       *reinterpret_cast<float*>(fn + 0x130);
					fadeKey = static_cast<std::uint8_t>(static_cast<int>(fade * 31.0f));
				}
			}
		}
		const std::uint64_t k = reinterpret_cast<std::uint64_t>(rd) ^
		                        (static_cast<std::uint64_t>(a_techniques[i]) << 1) ^
		                        (static_cast<std::uint64_t>(fadeKey) << 40);
		auto                it = index.find(k);
		std::size_t         gi;
		if (it == index.end()) {
			gi = liveGroups++;
			index.emplace(k, gi);
			if (gi < groups.size()) {  // recycle slot (passes already cleared; inner vector capacity kept)
				groups[gi].rd = rd;
				groups[gi].tech = a_techniques[i];
				groups[gi].fade = fadeKey;
			} else {
				groups.push_back(Group{ rd, a_techniques[i], fadeKey, 0, {} });
			}
		} else {
			gi = it->second;
		}
		groups[gi].passes.push_back(a_passes[i]);
	}
	if (liveGroups == 0)
		return;

	// Sort (indices, not Groups -- keeps recycled inner vectors in place) by (tech, fade) so the draw loop
	// runs technique setup ONCE per run instead of per group (was ~1.7k FlushSetupTechniqueReplica/frame).
	static std::vector<std::uint32_t> order;
	order.resize(liveGroups);
	for (std::uint32_t i = 0; i < liveGroups; ++i)
		order[i] = i;
	std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
		if (groups[a].tech != groups[b].tech)
			return groups[a].tech < groups[b].tech;
		return groups[a].fade < groups[b].fade;
	});

	// ---- instance VB: one dynamic buffer sized for ALL of this map's instances (32 bytes each), filled
	// with ONE Map(DISCARD); each group draws from its baseInst via DrawIndexedInstanced's
	// StartInstanceLocation (was one Map per group = ~1.7k DISCARD slices/frame of pure overhead) ----
	static winrt::com_ptr<ID3D11Buffer> s_instVB;
	static std::uint32_t                s_instCap = 0;
	std::uint32_t                       totalInst = 0;
	// Flat geom list in fill order (object k -> geometry, out slot = k*32). Lets the per-object matrix
	// build (SG_BuildMatrix + F16C pack) run as a flat parallel-for on worker threads (view-independent:
	// SG_BuildMatrix reads the object's world transform only; each object writes its own disjoint slot).
	for (std::uint32_t oi = 0; oi < liveGroups; ++oi) {
		groups[order[oi]].baseInst = totalInst;
		totalInst += static_cast<std::uint32_t>(groups[order[oi]].passes.size());
	}
	if (totalInst > s_instCap) {
		std::uint32_t newCap = s_instCap ? s_instCap : 4096u;
		while (newCap < totalInst)
			newCap *= 2u;
		s_instVB = nullptr;
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = newCap * 32u;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(globals::d3d::device->CreateBuffer(&bd, nullptr, s_instVB.put())))
			return;
		s_instCap = newCap;
		Util::SetResourceName(s_instVB.get(), "ShadowInstance::InstanceVB");
	}

	// Fill the whole map's instance stream in one Map(DISCARD). Per instance: engine SG_BuildMatrix (exact
	// world build incl. quirks), then SSE transpose + F16C pack (_mm_cvtps_ph, round-to-nearest-even ==
	// XMConvertFloatToHalf) -- replaces the SG_MatrixTranspose engine call + 16 scalar half-converts per
	// instance (~100k software conversions/frame, the dominant CPU overhead of the old fill).
	{
		D3D11_MAPPED_SUBRESOURCE mappedAll{};
		if (FAILED(ctx->Map(s_instVB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedAll))) {
			return;
		}
		auto* outBase = reinterpret_cast<std::uint8_t*>(mappedAll.pData);
		// SG_BuildMatrix (FUN_1412c3440) subtracts a per-frame camera position adjust from the translation
		// (0 during shadow render, but read it to stay byte-exact). Constant across the map -> read once.
		const float adjX = *reinterpret_cast<const float*>(REL::Offset(0x302820C).address());
		const float adjY = *reinterpret_cast<const float*>(REL::Offset(0x3028210).address());
		const float adjZ = *reinterpret_cast<const float*>(REL::Offset(0x3028214).address());
		for (std::uint32_t oi = 0; oi < liveGroups; ++oi) {
			auto&      g = groups[order[oi]];
			auto*      out = outBase + static_cast<std::size_t>(g.baseInst) * 32u;
			const auto n = static_cast<std::uint32_t>(g.passes.size());
			for (std::uint32_t i = 0; i < n; ++i, out += 32) {
				auto* geom = reinterpret_cast<std::uint8_t*>(g.passes[i]->geometry);
				// Inlined SG_BuildMatrix: World = scale * rotation (transposed 3x3) + (translate - camAdjust).
				// Replaces the per-instance engine call (~1200/frame, the dominant fill cost). All casters
				// here are whole-TRISHAPE (geom[0x150]==3, filtered above), so the engine's AsParticlesGeom
				// branch never runs -- that per-instance virtual call was dead on this path and is dropped.
				const float*      x = reinterpret_cast<const float*>(geom + 0x7C);  // NiTransform: rot[9] trans[3] scale
				const float       s = x[12];
				alignas(16) float m44[16];
				m44[0] = s * x[0];  m44[1] = s * x[3];  m44[2] = s * x[6];   m44[3] = 0.0f;
				m44[4] = s * x[1];  m44[5] = s * x[4];  m44[6] = s * x[7];   m44[7] = 0.0f;
				m44[8] = s * x[2];  m44[9] = s * x[5];  m44[10] = s * x[8];  m44[11] = 0.0f;
				m44[12] = x[9] - adjX;  m44[13] = x[10] - adjY;  m44[14] = x[11] - adjZ;  m44[15] = 1.0f;
				__m128 r0 = _mm_load_ps(m44 + 0), r1 = _mm_load_ps(m44 + 4), r2 = _mm_load_ps(m44 + 8), r3 = _mm_load_ps(m44 + 12);
				_MM_TRANSPOSE4_PS(r0, r1, r2, r3);  // == SG_MatrixTranspose
				_mm_storel_epi64(reinterpret_cast<__m128i*>(out + 0), _mm_cvtps_ph(r0, _MM_FROUND_TO_NEAREST_INT));
				_mm_storel_epi64(reinterpret_cast<__m128i*>(out + 8), _mm_cvtps_ph(r1, _MM_FROUND_TO_NEAREST_INT));
				_mm_storel_epi64(reinterpret_cast<__m128i*>(out + 16), _mm_cvtps_ph(r2, _MM_FROUND_TO_NEAREST_INT));
				_mm_storel_epi64(reinterpret_cast<__m128i*>(out + 24), _mm_cvtps_ph(r3, _MM_FROUND_TO_NEAREST_INT));
			}
		}
		ctx->Unmap(s_instVB.get(), 0);
	}

	// The INSTANCED Utility VS is fetched PER GROUP below (each group's exact permutation, not a hardcoded
	// one): the engine's shadow VS for a group can be plain RENDER_SHADOWMAP, the parabolic point-light
	// variant (RenderShadowmapPb), the clamped variant, or carry vertex-format / tree-wind-animation flags.
	// Instancing all of them with one plain VS drops/mis-projects the non-plain passes (point lights, etc.).

	// VA_INSTANCEDATA (attr 9) SLOT-1 presence bit ONLY. The IL key = (S+0x340) & instVS.vertexDesc feeds
	// engine ILCreate (0x140D70F90), whose VA_INSTANCEDATA branch decodes bit(44+9)=bit53 (slot-0 presence)
	// BEFORE bit(54+9)=bit63 (slot-1 presence): if bit53 is set it pins the 4 R16G16B16A16 instance elements
	// to InputSlot 0 (the MESH VB) with AlignedByteOffset = ((key>>38)&0x3C), so the 0xF offset nibble makes
	// them 60/68/76/84 -- garbage World read from the wrong buffer, plus an illegal per-vertex(POSITION) +
	// per-instance(TEXCOORD4-7) mix on one slot. Setting ONLY bit63 (no bit53, no offset nibble) gives
	// InputSlot 1 (matching s_instVB bound at slot 1) at offsets 0/8/16/24 -- exactly the 32B FP16 stream.
	// (Do NOT use the full ShaderCache::AddAttribute encoding here; that is right for a mesh-owned attribute
	// but wrong for an engine-injected per-instance slot-1 stream.)
	constexpr std::uint64_t kInstBits = (1ull << (54 + 9));

	// INSTANCED flag bit (ShaderCache::UtilityShaderFlags::Instanced). ORed onto each group's real descriptor.
	constexpr std::uint32_t kInstancedFlag = 1u << 30;

	// Draw loop over the (tech, fade)-sorted groups. Per-(tech,fade) RUN setup is hoisted -- BeginTechnique,
	// instanced-VS fetch, alpha/geometry setup, VS override happen ONCE per run (was per group: ~1.7k
	// FlushSetupTechniqueReplica + VSSetShader + SetupGeometry per frame). Groups inside a run issue only
	// their mesh binds + one DrawIndexedInstanced reading from their baseInst slot of the shared instance
	// stream. RestoreGeometry closes each run (resets the shadow-fade depth-stencil token, uniform per run
	// because fade is part of the sort key).
	std::uint32_t                 curTech = 0xFFFFFFFFu;
	std::uint8_t                  curFade = 0;
	bool                          runLive = false;      // hoisted state valid; instanced draws may proceed
	bool                          runFallback = false;  // this (tech,fade) run renders per-pass (VS compiling)
	RE::BSGraphics::VertexShader* instVS = nullptr;
	RE::BSShader*                 runShader = nullptr;
	RE::BSRenderPass*             runPass0 = nullptr;
	const auto closeRun = [&]() {
		if (runLive && runShader && runPass0)
			EngineCallV<7, void>(runShader, runPass0, a_renderFlags);  // RestoreGeometry (fade token reset)
		runLive = false;
	};
	for (std::uint32_t oi = 0; oi < liveGroups; ++oi) {
		auto&               g = groups[order[oi]];
		const std::uint32_t n = static_cast<std::uint32_t>(g.passes.size());
		if (n == 0)
			continue;
		auto*               shader = g.passes[0]->shader;
		auto*               geom0 = reinterpret_cast<std::uint8_t*>(g.passes[0]->geometry);
		auto*               rd = g.rd;
		const std::uint16_t triCount = *reinterpret_cast<const std::uint16_t*>(geom0 + 0x158);

		if (!runLive && !runFallback || g.tech != curTech || g.fade != curFade) {
			closeRun();
			curTech = g.tech;
			curFade = g.fade;
			runFallback = false;

			// 1. BeginTechnique: engine binds depth PS + blend/depth/raster + PerTechnique CB (and its VS,
			//    which we override below). Reset the caches so a fresh SetupTechnique runs.
			WsShader() = nullptr;
			WsTechnique() = 0;
			WsMaterial() = nullptr;
			if (!FlushSetupTechniqueReplica(ctx, nullptr, nullptr, S, shader, static_cast<std::int32_t>(g.tech))) {
				runFallback = true;  // no valid technique state: render this run's groups per-pass
				instVS = nullptr;
			} else {
				// 2. Fetch the INSTANCED VS for THIS run's EXACT permutation (id | INSTANCED preserves the
				//    parabolic / clamped / wind / vertex-format variant). Lookup-first; a miss queues an async
				//    compile and the run renders per-pass until the instanced VS is ready.
				auto* boundVS = *reinterpret_cast<RE::BSGraphics::VertexShader**>(S + 0x348);
				instVS = boundVS ? ShaderCache::Instance().GetVertexShader(*shader, boundVS->id | kInstancedFlag) : nullptr;
				if (!instVS || !instVS->shader) {
					runFallback = true;
				} else {
					// Per-geometry setup once per run, on the run's first pass, while the engine shadow VS is
					// bound: establishes (1) the shadow-FADE depth-stencil token (uniform per run -- fade is in
					// the sort key) and (2) the per-LIGHT scissor (SG_ScissorFromBBox; per light == per map, so
					// uniform across every group here). Also writes b2 World (IGNORED -- the instanced VS reads
					// World from the stream) and the ShadowFadeParam.z falloff (SHADOWMASK-only; harmless).
					{
						auto* vsSh = *reinterpret_cast<std::uint8_t**>(S + 0x348);
						auto* psSh = *reinterpret_cast<std::uint8_t**>(S + 0x350);
						FlushSetupGeometryReplica(ctx,
							vsSh ? *reinterpret_cast<ID3D11Buffer**>(vsSh + 0x38) : nullptr,
							psSh ? *reinterpret_cast<ID3D11Buffer**>(psSh + 0x30) : nullptr,
							S, shader, g.passes[0]);
					}

					// Override the bound VS with the INSTANCED permutation; the b2 CB stays bound at slot 2.
					EngineCallV<11, void>(ctx, reinterpret_cast<ID3D11VertexShader*>(instVS->shader), nullptr, 0u);  // VSSetShader
					*reinterpret_cast<void**>(S + 0x348) = instVS;

					runShader = shader;
					runPass0 = g.passes[0];
					runLive = true;
				}
			}
		}

		if (runFallback) {
			// Per-pass fallback (technique failed or instanced VS still compiling). ReplicaRenderPassImmediately
			// does its own full per-pass setup and clobbers the hoisted state, so the run stays non-live and any
			// following (tech,fade) change re-runs the full setup above.
			for (std::uint32_t i = 0; i < n; ++i)
				ReplicaRenderPassImmediately(g.passes[i], g.tech, false, a_renderFlags);
			continue;
		}

		// 3. renderStateVertexDesc = meshDesc | VA_INSTANCEDATA (slot-1 presence bit only); IL + topology dirty.
		auto& vertexDesc = *reinterpret_cast<std::uint64_t*>(S + 0x340);
		auto& stateFlags = *reinterpret_cast<std::uint32_t*>(S + 0);
		auto& topology = *reinterpret_cast<std::uint32_t*>(S + 0x358);
		vertexDesc = rd->vertexDesc | kInstBits;
		stateFlags |= 0x400;  // DIRTY_VERTEX_DESC -> ILCreate(meshDesc & instVS.vertexDesc)
		if (topology != 4) {
			topology = 4;  // TRIANGLELIST
			stateFlags |= 0x800;
		}

		// 4. Flush the input layout + pending render state (engine SetDirtyStates; N=1 == global block).
		//    The per-LIGHT scissor set by the run's SetupGeometry survives this -- do NOT widen it.
		EngineCall<void>(reinterpret_cast<void*>(engine::SetDirtyStates.address()), false);

		// 5. Bind mesh VB (slot 0) + shared instance VB (slot 1) and draw this group's slice of the
		//    instance stream via StartInstanceLocation (no per-group Map, no per-group VB re-fill).
		const UINT    meshStride = static_cast<UINT>((4 * rd->vertexDesc) & 0x3C);
		ID3D11Buffer* vbs[2] = { rd->vertexBuffer, s_instVB.get() };
		UINT          strides[2] = { meshStride, 32u };
		UINT          offsets[2] = { 0u, 0u };
		EngineCallV<19, void>(ctx, rd->indexBuffer, DXGI_FORMAT_R16_UINT, 0u);          // IASetIndexBuffer
		EngineCallV<18, void>(ctx, 0u, 2u, vbs, strides, offsets);                      // IASetVertexBuffers
		EngineCallV<20, void>(ctx, 3u * triCount, n, 0u, 0, g.baseInst);                // DrawIndexedInstanced
	}
	closeRun();

	// Invalidate the technique/material caches so the engine re-binds its OWN VS on the next pass
	// instead of hitting the cache and drawing through the still-bound INSTANCED VS.
	WsShader() = nullptr;
	WsTechnique() = 0;
	WsMaterial() = nullptr;

}

void UtilityPassReplica::ReplicaRenderSkinned(RE::BSRenderPass* a_pass, bool a_alphaTest, std::uint32_t a_renderFlags)
{
	// ---- skinned path (0x141308970), static skin-instance branch ----
	// CanReplicate already excluded the dynamic bone-setter branch (geometry vfunc 54).
	// Notable deltas vs the standard path: the per-thread dynamic accumulator at
	// TLS+10752 is zeroed on entry (FUN_14131f7c0), the alpha-test flag is used RAW
	// (no early-Z OR), and the draw goes through the skin instance's Render vfunc (37),
	// which loops partitions, uploads bone palettes via the shader's bone-setter
	// interface, and issues the per-partition draws -- all engine code, shared with the
	// passes the engine still renders itself.
	auto* shader = a_pass->shader;
	auto* geom = a_pass->geometry;
	auto* geomBytes = reinterpret_cast<std::uint8_t*>(geom);
	auto* skin = *reinterpret_cast<void**>(geomBytes + 0x130);

	*reinterpret_cast<std::uint32_t*>(engine::TlsBlock() + 10752) = 0;

	// ShaderSetup (0x141309F80) with the RAW alpha-test flag.
	if (shader != *reinterpret_cast<RE::BSShader**>(engine::g_skyShaderInstance.address())) {
		if ((a_renderFlags & 4) && !EngineCall<bool>(reinterpret_cast<void*>(engine::IsGrassShadowBlacklist.address()), a_pass->passEnum))
			EngineCall<void>(reinterpret_cast<void*>(engine::SetupGeometryAlphaBlending.address()), shader,
				EngineCall<RE::NiAlphaProperty*>(reinterpret_cast<void*>(engine::GetNiProperty.address()), a_pass),
				a_pass->shaderProperty, a_alphaTest);
		if (a_alphaTest) {
			if (auto* alphaProp = EngineCall<RE::NiAlphaProperty*>(reinterpret_cast<void*>(engine::GetNiProperty.address()), a_pass))
				EngineCall<void>(reinterpret_cast<void*>(engine::SetupAlphaTestRef.address()), shader, alphaProp, a_pass->shaderProperty);
		}
	}
	EngineCallV<6, void>(shader, a_pass, a_renderFlags);  // SetupGeometry

	// Draw-struct layout verified against the dispatcher disasm at 0x141308A05.
	const auto lodByte = reinterpret_cast<const std::uint8_t*>(a_pass)[0x1E];
	engine::SkinDrawStruct s{};
	s.boneSetter = shader ? reinterpret_cast<std::uint8_t*>(shader) + 0x10 : nullptr;
	s.geometry = geom;
	s.singleLevel = (lodByte >> 7) & 1;
	s.lodIndex = lodByte & 0x7F;
	s.dynOffset[0] = -1;

	// Dynamic-shape sub-block (skinned BSDynamicTriShape, e.g. faces): upload the
	// CPU-side dynamic vertex data into the shared dynamic ring; the ring offset lands
	// in s.dynOffset[0] and the partition draw consumes it.
	if (void* dyn = EngineCallV<12, void*>(geom)) {  // AsBSDynamicTriShape
		auto* dynBytes = reinterpret_cast<std::uint8_t*>(dyn);
		const auto size = *reinterpret_cast<const std::uint32_t*>(dynBytes + 0x170);
		void* dst = EngineCall<void*>(reinterpret_cast<void*>(engine::MapSkinDynamicData.address()),
			reinterpret_cast<void*>(engine::g_renderer.address()), size, static_cast<std::int32_t*>(s.dynOffset));
		if (dst) {
			const void* src = EngineCall<void*>(reinterpret_cast<void*>(engine::DynShapeLockData.address()), dyn);
			std::memcpy(dst, src, size);
			EngineCall<void>(reinterpret_cast<void*>(engine::UnmapSkinDynamicData.address()),
				reinterpret_cast<void*>(engine::g_renderer.address()), dst);
			EngineCall<void>(reinterpret_cast<void*>(engine::DynShapeUnlock.address()), dyn);
		}
	}

	EngineCallV<37, void>(skin, static_cast<void*>(&s));  // NiSkinInstance/BSDismember Render

	EngineCallV<7, void>(shader, a_pass, a_renderFlags);  // RestoreGeometry
}

