#pragma once

/**
 * @brief Static local-light shadow cache -- the shadow-map-walk detours. OWNED BY LightLimitFix (installed
 *        from LightLimitFix::PostPostLoad); this is NOT a standalone EngineFix.
 *
 * Detours the engine's shadow-map walk (DrawWorld::RenderShadowmaps + BSShadowLight::RenderShadowmap). On a
 * local-light map, DYNAMIC casters (skinned actors, wind-animated trees, and anything that can animate / have
 * havok / be script-toggled or has moved) render INLINE on the engine's own path so their live pose/position
 * is correct. Only the completely-STATIC casters are claimed (skipped inline): they are rendered once into a
 * cached depth layer and that cached static depth is COMPOSITED under the live dynamics every frame. So static
 * furniture/walls cost nothing after the first frame while actors and swaying trees keep moving. The static
 * base regenerates only when the static set changes (a static placed / disabled / moved); that capture frame
 * suppresses dynamics so the copied base is clean static. Directional cascades claim nothing (engine renders
 * everything). No instancing. SE-only (every RE'd offset is 1.5.97).
 */
namespace ShadowMapCacheHooks
{
	/** @brief Bring up UtilityPassReplica and install the two shadow-map-walk detours. Call once, from
	 *         LightLimitFix::PostPostLoad. SE-only; a no-op on AE. */
	void Install();
}
