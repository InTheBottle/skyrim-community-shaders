#include "UtilityPassReplica.h"

#include "Globals.h"
#include "State.h"

#include <chrono>

#include <RE/B/BSRenderPass.h>
#include <RE/B/BSShader.h>
#include <RE/P/PlayerCharacter.h>

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

// Install the RenderPassImmediately detour (the seam the shadow cache rides to observe each utility
// pass and offer it to the shadow-capture hook) exactly once. Idempotent.
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

	// Pure code detour (RelocationID) -- installable at kPostPostLoad, before the D3D device exists.
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
	// Proceed only when a shadow-capture hook is armed (ShadowMapCacheHooks arms it only during the
	// shadow-map walk, so main-scene utility passes are unaffected). Otherwise the engine renders.
	if (!shadowCaptureHook.load(std::memory_order_acquire)) {
		RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
		return;
	}

	// Only claim in fully-SETTLED in-world rendering. Load-screen / main-menu 3D renders (the rotating
	// loading-screen model, the main-menu preview) AND the world-init window right after a save load drive
	// utility shadow passes with transiently-uninitialized shadow state -- the focus-shadow array, the
	// shadow camera node (*0x1431D0F88), the scene light list, and per-shader constant buffers are all
	// mid-init. The load->gameplay transition also flips the shadow camera/light pointers to garbage for a
	// pass or two. The engine tolerates all this; the shadow cache's claim + change-signature fold reads
	// those transients, so keep the capture path OFF until the scene is settled. No instantaneous flag is
	// race-proof, so gate on the standard CS "in world" signal (player + parent cell + no main/loading menu)
	// AND require it to have held continuously for a short settle window. Outside that, the engine draws as-is.
	auto* const player = RE::PlayerCharacter::GetSingleton();
	const bool  rawInWorld = player && player->GetParentCell() &&
	                     globals::state && !globals::state->IsMainOrLoadingMenuOpen();
	// Settle: proceed only after we've been CONTINUOUSLY in-world for kSettleMs. Track the first-in-world
	// timestamp and reset it whenever we leave the world (menu/load), so the load->gameplay transition
	// race is past before the cache claims. Unlike the old s_everUnsettled latch, this does NOT
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

	// Shadow-capture fan-out (the shadow cache). While a shadow-map walk is being captured, offer each
	// utility pass to the hook; if it claims ownership (a static caster the cache replays later), skip the
	// inline render here entirely. Otherwise fall through and let the engine render the pass.
	if (auto hook = shadowCaptureHook.load(std::memory_order_acquire)) {
		if (hook(a_pass, a_technique, a_alphaTest, a_renderFlags, CanReplicate(a_pass)))
			return;
	}

	// Not claimed (dynamic caster, or the cache is not capturing this pass): whole-pass engine render.
	RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

void UtilityPassReplica::RenderPassesOriginal(RE::BSRenderPass* const* a_passes, const std::uint32_t* a_techniques,
	const std::uint8_t* a_alphaTests, const std::uint32_t* a_renderFlags, std::uint32_t a_count)
{
	// Replay each claimed static shadow caster through the engine's ORIGINAL RenderPassImmediately -- the
	// detour trampoline (RenderPassImmediately_Hook::func), NOT the OnRenderPassImmediately detour body -- so
	// each pass draws exactly as the engine would have inline, and no pass is re-offered to the capture hook.
	// The caller has reseeded the map's captured render-state block, so the DSV/viewport are the map's.
	for (std::uint32_t i = 0; i < a_count; ++i)
		RenderPassImmediately_Hook::Engine(a_passes[i], a_techniques[i], a_alphaTests[i] != 0, a_renderFlags[i]);
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
