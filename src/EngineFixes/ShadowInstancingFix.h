#pragma once

/**
 * @brief Shadow-map instancing: cuts the shadow draw count via same-mesh instancing.
 *
 * Detours the engine's shadow-map walk (DrawWorld::RenderShadowmaps + BSShadowLight::RenderShadowmap)
 * and, per map, claims the instanceable caster subset (whole-TRISHAPE, non-skinned, non-alpha-test)
 * through UtilityPassReplica's shadow-capture hook. The claimed passes are grouped by (mesh, technique,
 * fade) and re-issued as one DrawIndexedInstanced per group on the immediate context (each object's
 * World packed into a per-instance vertex stream consumed by the INSTANCED Utility VS) -- ~9x fewer
 * shadow draws in dense exteriors. The engine renders the remainder inline. Local-light shadow maps are
 * additionally staggered (ShadowMapCache): one unit re-renders per frame, the rest blit their cached
 * depth slice. Byte-exact vs vanilla; SE-only (every RE'd offset is 1.5.97).
 */
struct ShadowInstancingFix : EngineFix
{
	/** @brief Returns the human-readable name of this fix. */
	std::string GetName() override { return "Shadow Instancing"; }

	/** @brief Brings up UtilityPassReplica and installs the two shadow-map-walk detours. */
	void Install() override;
};
