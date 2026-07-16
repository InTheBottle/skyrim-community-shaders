#pragma once

#include <cstdint>

namespace RE
{
	class BSShadowLight;
}

// Local-light (spot / point-omni) shadow-map caching.
//
// The engine re-renders every shadow caster into every local light's shadow map
// every frame (BSShadowFrustumLight / BSShadowParabolicLight vtable slot 0xA,
// BSShadowLight::Render). For a light whose transform AND whose caster set +
// transforms are unchanged, that shadow map is bit-identical to last frame's, so
// the render is pure waste.
//
// This module starts as a PROFILER (env CS_LIGHTSHADOW_PROF=1): it times the
// per-light Render calls and measures the true cache-hit rate (how many local
// shadow maps are unchanged frame-to-frame), which sizes the caching opportunity
// before the (larger, slot-pinning) cache is built.
namespace LocalLightShadowCache
{
	// Installs the per-light Render vtable hooks. Reads CS_LIGHTSHADOW_PROF.
	// Safe no-op unless the env gate is set. Call once at startup.
	void Install();

	// Frame boundaries around the engine shadow driver. Called from
	// Deferred::Main_RenderShadowMaps. Cheap no-ops unless profiling is enabled.
	void BeginShadowFrame();
	void EndShadowFrame();

	// True once Install() saw the env gate.
	bool Enabled();

	// Ceiling-measurement toggle: when true, local-light (spot/point) shadow
	// renders are skipped entirely (shadows go stale/wrong — for measuring the
	// upper-bound FPS win only, NOT correct). Runtime-togglable via devbench.
	void SetSkipLocal(bool a_skip);
	bool GetSkipLocal();
}
