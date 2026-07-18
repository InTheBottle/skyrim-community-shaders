#pragma once

/**
 * @brief Static local-light shadow cache -- the shadow-map-walk detours. OWNED BY LightLimitFix (installed
 *        from LightLimitFix::PostPostLoad); this is NOT a standalone EngineFix.
 *
 * Detours the engine's shadow-map walk (DrawWorld::RenderShadowmaps + BSShadowLight::RenderShadowmap). Per
 * map it claims the STATIC casters through UtilityPassReplica's capture hook (skipping their inline draw),
 * folds them into a change signature, and either BLITS the cached depth slice (static set unchanged) or
 * replays the statics via the engine's OWN RenderPassImmediately (fresh frame) -- one DrawIndexed each, no
 * instancing. Local-light maps that drew any dynamic caster this frame render fully fresh and are never
 * cached. SE-only (every RE'd offset is 1.5.97).
 */
namespace ShadowMapCacheHooks
{
	/** @brief Bring up UtilityPassReplica and install the two shadow-map-walk detours. Call once, from
	 *         LightLimitFix::PostPostLoad. SE-only; a no-op on AE. */
	void Install();
}
