#include "ShadowDeferred.h"

#include "Globals.h"

void ShadowDeferred::Setup()
{
	char buf[8] = {};
	if (GetEnvironmentVariableA("CS_SHADOW_DEFERRED", buf, sizeof(buf)) && buf[0]) {
		const int v = atoi(buf);
		if (v >= 0 && v <= 1)
			mode.store(static_cast<Mode>(v), std::memory_order_relaxed);
	}
	if (!IsActive())
		return;

	auto* device = globals::d3d::device;
	if (!device) {
		logger::error("[ShadowDeferred] no D3D11 device; disabling");
		mode.store(Mode::kOff, std::memory_order_relaxed);
		return;
	}

	// Native D3D11 supports deferred contexts + command lists directly. The command list
	// is produced without a bound state (D3D11 deferred contexts start with default
	// state), so the shadow recording sets every state it needs explicitly -- which is
	// exactly what a privately-stated, eventually-threaded path must do anyway.
	const HRESULT hr = device->CreateDeferredContext(0, deferredContext.put());
	if (FAILED(hr) || !deferredContext) {
		logger::error("[ShadowDeferred] CreateDeferredContext failed (0x{:08X}); disabling", static_cast<std::uint32_t>(hr));
		mode.store(Mode::kOff, std::memory_order_relaxed);
		return;
	}

	InstallHooks();
	logger::info("[ShadowDeferred] active, mode={} (deferred context ready)", static_cast<std::uint32_t>(GetMode()));
}

void ShadowDeferred::InstallHooks()
{
	if (hooksInstalled)
		return;
	hooksInstalled = true;

	// The shadow-loop detour + immediate-context redirect + ExecuteCommandList are wired
	// here once the shadow-loop RE lands (workflow wf_8f1a3a44). Left unwired at this
	// checkpoint so the deferred-context creation and env gating can be validated first.
}
