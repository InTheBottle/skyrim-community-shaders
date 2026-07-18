#pragma once

/**
 * @brief Static local-light shadow cache -- the shadow-map-walk detours. OWNED BY LightLimitFix (installed
 *        from LightLimitFix::PostPostLoad); this is NOT a standalone EngineFix.
 *
 * Detours the engine's shadow-map walk (DrawWorld::RenderShadowmaps + BSShadowLight::RenderShadowmap). On a
 * local-light map it claims BOTH the static and the dynamic casters through UtilityPassReplica's capture hook
 * (skipping their inline draws) and controls the render order: a STATIC BASE is cached once (blit the cached
 * depth, or on a static-set change render the statics fresh via the engine's own RenderPassImmediately and
 * copy them into the cache), then the DYNAMIC casters are replayed ON TOP every frame, depth-tested against
 * the base. So static furniture/walls cost nothing after the first frame while actors and swaying trees keep
 * moving. The static base regenerates only when the static signature changes (a static placed / disabled /
 * moved). Directional cascades claim only statics; their dynamics stay inline. No instancing. SE-only (every
 * RE'd offset is 1.5.97).
 */
namespace ShadowMapCacheHooks
{
	/** @brief Bring up UtilityPassReplica and install the two shadow-map-walk detours. Call once, from
	 *         LightLimitFix::PostPostLoad. SE-only; a no-op on AE. */
	void Install();
}
