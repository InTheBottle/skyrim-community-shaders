#include "ShadowThreaded.h"

#include <vector>

#include "Globals.h"
#include "UtilityPassReplica.h"

// M1/M2 (capture + per-map partition). The shadow-map walk is bracketed at two levels:
//   - RenderShadowmaps 0x1412E3480 : once-per-frame driver -> arm the replica capture hook.
//   - RenderShadowmap  0x141305610 : once-per-map body     -> a PLANNING interceptor that
//         snapshots the map's params (camera/accumulator/target/slice/renderFlags) BY VALUE into
//         an ordered mapWorkList and brackets its passes. The engine still renders inline this
//         stage (observe-only), so this changes no output -- it proves the fan-out seam sees the
//         exact per-map partition a worker will later replay. See docs/development/mt-shadow-plan.md.
//
// RenderShadowmap 0x141305610 (BSShadowLight per-map body), from the 1.5.97 decompile:
//   int32 __fastcall(BSShadowLight* a1, desc* a2, int32* a3, int32 a4=renderFlags)
//   *(a2+64)=camera  *(a2+72)=accumulator  *(a2+84)=DSV target (lazily allocated if -1)
//   *(a2+88)=slice   -- the slice alloc from dword_141E10538 runs INSIDE, so target/slice are
//   only final AFTER the original returns; the render is NiCamera::RenderPreAndPostResolveDepth.

namespace
{
	// One shadow map's replay unit, captured by value at the planning interceptor. `camera` and
	// `accumulator` are engine object pointers (persist for the frame); target/slice identify the
	// depth atlas region; renderFlags is the a4 the engine passed. `passes` is the ordered covered-
	// pass list bracketed for this map (M2 groups; M3 workers replay it).
	struct MapWork
	{
		void*         camera = nullptr;
		void*         accumulator = nullptr;
		std::int32_t  target = -1;
		std::int32_t  slice = -1;
		std::int32_t  renderFlags = 0;
		std::uint64_t passes = 0;       // covered passes bracketed for this map
		std::uint64_t unsupported = 0;  // uncovered passes (serial-engine remainder) in this map
	};

	std::vector<MapWork> g_mapWorkList;  // ordered by engine map iteration; render thread only
	MapWork*             g_curMap = nullptr;
	bool                 g_capturing = false;

	// UtilityPassReplica::ShadowCaptureHook. Observe-only: attribute the pass to the current map
	// and decline ownership so the inline render is unchanged.
	bool CaptureHookM2(RE::BSRenderPass* /*a_pass*/, std::uint32_t /*a_technique*/, bool /*a_alphaTest*/,
		std::uint32_t /*a_renderFlags*/, bool a_canReplicate)
	{
		if (g_curMap) {
			if (a_canReplicate)
				++g_curMap->passes;
			else
				++g_curMap->unsupported;
		}
		return false;
	}

	// DrawWorld::RenderShadowmaps 0x1412E3480 (AddrLib 100420) -- once-per-frame shadow driver.
	struct RenderShadowmapsHook
	{
		static void thunk() { ShadowThreaded::GetSingleton()->RenderShadowmapsDetour(&func); }
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// BSShadowLight::RenderShadowmap 0x141305610 (AddrLib 100820) -- once-per-map body.
	struct RenderShadowmapHook
	{
		static std::int32_t thunk(void* a1, std::int64_t a2, void* a3, std::int32_t a4)
		{
			return ShadowThreaded::GetSingleton()->RenderShadowmapDetour(a1, a2, a3, a4, &func);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

std::int32_t ShadowThreaded::RenderShadowmapDetour(void* a1, std::int64_t a2, void* a3, std::int32_t a4, void* a_original)
{
	auto callOriginal = *static_cast<std::int32_t(**)(void*, std::int64_t, void*, std::int32_t)>(a_original);
	if (!g_capturing)
		return callOriginal(a1, a2, a3, a4);

	// Snapshot the pre-render params by value, open the map's pass bracket, run the original (the
	// slice alloc + NiCamera walk dispatches this map's passes through CaptureHookM2), then finalize
	// the target/slice the engine just resolved.
	const auto desc = reinterpret_cast<std::uint8_t*>(a2);
	g_mapWorkList.push_back(MapWork{
		*reinterpret_cast<void**>(desc + 64),   // camera
		*reinterpret_cast<void**>(desc + 72),   // accumulator
		-1, -1, a4, 0, 0 });
	g_curMap = &g_mapWorkList.back();

	const std::int32_t r = callOriginal(a1, a2, a3, a4);

	// g_curMap may be stale if push_back reallocated during the walk (it cannot -- no nested map),
	// but re-index defensively.
	MapWork& m = g_mapWorkList.back();
	m.target = *reinterpret_cast<std::int32_t*>(desc + 84);
	m.slice = *reinterpret_cast<std::int32_t*>(desc + 88);
	g_curMap = nullptr;
	return r;
}

void ShadowThreaded::RenderShadowmapsDetour(void* a_original)
{
	auto callOriginal = *static_cast<void (**)()>(a_original);
	if (!IsActive()) {
		callOriginal();
		return;
	}

	auto* replica = UtilityPassReplica::GetSingleton();

	g_mapWorkList.clear();
	g_mapWorkList.reserve(24);
	g_curMap = nullptr;
	g_capturing = true;
	replica->SetShadowCaptureHook(&CaptureHookM2);

	callOriginal();  // per-map interceptor brackets each map; covered passes flow through the hook

	replica->SetShadowCaptureHook(nullptr);
	g_capturing = false;

	if ((++frames & 0x3F) == 1) {
		std::uint64_t totPass = 0, totUnsup = 0;
		std::string   detail;
		for (std::size_t i = 0; i < g_mapWorkList.size(); ++i) {
			const auto& m = g_mapWorkList[i];
			totPass += m.passes;
			totUnsup += m.unsupported;
			if (i < 20)
				detail += fmt::format(" [{}]t{}s{}:{}{}", i, m.target, m.slice, m.passes,
					m.unsupported ? fmt::format("+{}u", m.unsupported) : "");
		}
		logger::info("[ShadowThreaded][M2] frame {}: maps={} passes={} unsupported={} |{}",
			frames, g_mapWorkList.size(), totPass, totUnsup, detail);
	}
}

void ShadowThreaded::InstallHooks()
{
	if (hooksInstalled)
		return;
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
		if (v >= 0 && v <= 1)
			mode.store(static_cast<Mode>(v), std::memory_order_relaxed);
	}
	if (!IsActive())
		return;
	if (!REL::Module::IsSE()) {
		logger::info("[ShadowThreaded] SE-only; not installing on this runtime");
		return;
	}
	InstallHooks();
	logger::info("[ShadowThreaded] active, mode={} (capture)", static_cast<std::uint32_t>(GetMode()));
}
