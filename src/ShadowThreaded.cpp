#include "ShadowThreaded.h"

#include "Globals.h"
#include "UtilityPassReplica.h"

// M1 (capture): observe the shadow-map walk through UtilityPassReplica's ShadowCaptureHook and
// report the covered-pass partition per frame. The hook returns false (does NOT claim the pass),
// so the replica/engine still renders every pass inline -- this stage changes no rendering, it
// only proves the fan-out seam sees exactly the covered shadow-caster passes. M2+ will claim the
// covered passes here and replay them on worker deferred contexts.

namespace
{
	// Per-frame partition tallies, written by the capture hook on the render thread during the
	// walk and read after it returns. Reset at each frame's scope entry.
	std::uint64_t g_capTotal = 0;        // every utility pass seen during the shadow walk
	std::uint64_t g_capReplicable = 0;   // covered by the byte-exact replica (thread candidates)
	std::uint64_t g_capUnsupported = 0;  // outside coverage -> must stay on the serial engine path

	// Matches UtilityPassReplica::ShadowCaptureHook. Observe-only: tally and decline ownership.
	bool CaptureHookM1(RE::BSRenderPass* /*a_pass*/, std::uint32_t /*a_technique*/, bool /*a_alphaTest*/,
		std::uint32_t /*a_renderFlags*/, bool a_canReplicate)
	{
		++g_capTotal;
		if (a_canReplicate)
			++g_capReplicable;
		else
			++g_capUnsupported;
		return false;  // do not claim -- inline render proceeds unchanged
	}

	// DrawWorld::RenderShadowmaps 0x1412E3480 (AddrLib 100420) -- once-per-frame shadow driver.
	struct RenderShadowmapsHook
	{
		static void thunk() { ShadowThreaded::GetSingleton()->RenderShadowmapsDetour(&func); }
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void ShadowThreaded::RenderShadowmapsDetour(void* a_original)
{
	auto callOriginal = *static_cast<void (**)()>(a_original);
	if (!IsActive()) {
		callOriginal();
		return;
	}

	auto* replica = UtilityPassReplica::GetSingleton();

	g_capTotal = 0;
	g_capReplicable = 0;
	g_capUnsupported = 0;

	replica->SetShadowCaptureHook(&CaptureHookM1);
	callOriginal();  // covered passes flow through CaptureHookM1
	replica->SetShadowCaptureHook(nullptr);

	if ((++frames & 0x3F) == 1) {
		logger::info("[ShadowThreaded][M1] frame {}: shadow-walk passes total={} replicable={} unsupported={} ({:.1f}% covered)",
			frames, g_capTotal, g_capReplicable, g_capUnsupported,
			g_capTotal ? 100.0 * static_cast<double>(g_capReplicable) / static_cast<double>(g_capTotal) : 0.0);
	}
}

void ShadowThreaded::InstallHooks()
{
	if (hooksInstalled)
		return;
	stl::detour_thunk<RenderShadowmapsHook>(REL::RelocationID(100420, 0));
	hooksInstalled = true;
	logger::info("[ShadowThreaded] detoured RenderShadowmaps @ 0x{:X}", RenderShadowmapsHook::func.address());
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
