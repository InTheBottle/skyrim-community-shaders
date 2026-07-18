#pragma once

#include <d3d11_1.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace RE
{
	class BSRenderPass;
}

/**
 * @brief Byte-exact reverse-engineered replica of the BSUtilityShader render path:
 *        BSBatchRenderer::RenderPassImmediately (SE 1.5.97 0x141308440) down to
 *        ID3D11DeviceContext::DrawIndexed.
 *
 * PURPOSE (RE baseline, not an optimization): for any save and any in-game situation,
 * re-issue exactly the same D3D11 commands with exactly the same data the engine would
 * have issued for a utility (depth-only: shadow map / z-prepass) pass. Once the replica
 * is provably command-identical, engine code can be switched off pass-by-pass and the
 * frame must not change -- that proven-equivalent C++ is the foundation later
 * optimizations (batching, indirect shadows, parallel setup) get built on.
 *
 * ARCHITECTURE
 *  - The replica shares ALL engine state: it reads and writes the same
 *    BSGraphics::Renderer globals block (dirty flags, technique/material caches,
 *    precomputed state-object arrays) at the same offsets, and issues its D3D11 calls
 *    on the same immediate context. Cross-pass caches therefore stay coherent with
 *    passes the engine still renders itself -- any single pass can be flipped between
 *    engine and replica without disturbing its neighbours.
 *  - The replica is driven by ShadowInstancingFix through the shadow-capture hook: during the
 *    shadow-map walk it offers each utility pass to the hook, which claims the instanceable subset
 *    for the batched replay (RenderShadowInstanced) and leaves the rest to the engine. Outside the
 *    walk (no hook armed) the hooks are inert and the engine renders everything.
 *
 * All engine-derived constants cite their 1.5.97 address; see
 * docs/development/utility-pass-re.md for the full RE dossier.
 */
class UtilityPassReplica
{
public:
	static UtilityPassReplica* GetSingleton()
	{
		static UtilityPassReplica singleton;
		return &singleton;
	}

	/** @brief Install the RenderPassImmediately detour (the seam ShadowInstancingFix's instancing
	 *         path rides to observe each utility pass and offer it to the shadow-capture hook) once. */
	void EnsureInitialized();

	/** @brief RenderPassImmediately detour body. Offers utility passes to the shadow-capture hook
	 *         during the shadow-map walk; forwards everything else to the engine untouched. */
	void OnRenderPassImmediately(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags);

	/**
	 * @brief The replica itself: issue the exact engine-equivalent command stream for
	 *        one utility pass (technique/material change detection, ShaderSetup,
	 *        geometry dispatch, dirty-state flush, DrawIndexed). RE'd from 1.5.97.
	 */
	void ReplicaRenderPassImmediately(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags);

	/** @brief Replay a set of captured shadow passes through the engine's ORIGINAL RenderPassImmediately
	 *         (the detour trampoline) -- byte-for-byte the draws the engine would have issued inline, one
	 *         DrawIndexed per caster, no instancing and no replica. This is the shadow cache's fresh-frame
	 *         render: the claimed static casters were skipped inline during the walk, so replay them here.
	 *         Must be called while the map's RT/DSV/viewport are bound (per-map replay point). Parallel
	 *         arrays of a_count entries; a_alphaTests / a_renderFlags are per-pass. Bypasses the capture
	 *         hook (calls the trampoline directly), so no pass is re-captured. */
	void RenderPassesOriginal(RE::BSRenderPass* const* a_passes, const std::uint32_t* a_techniques,
		const std::uint8_t* a_alphaTests, const std::uint32_t* a_renderFlags, std::uint32_t a_count);

	/** @brief Shadow instancing: render a set of captured static shadow passes as a small number of
	 *         DrawIndexedInstanced calls (one per unique mesh+technique group) instead of one DrawIndexed
	 *         per object. Each object's World matrix (identical to the b2 PerGeometry World the engine
	 *         builds) is packed into a per-instance vertex stream and consumed by the INSTANCED Utility VS.
	 *         Must be called while the shadow map's RT/DSV/viewport are still bound (per-map replay point).
	 *         a_passes / a_techniques are parallel arrays; a_count entries; a_renderFlags is the map's flags. */
	void RenderShadowInstanced(RE::BSRenderPass* const* a_passes, const std::uint32_t* a_techniques,
		std::uint32_t a_count, std::uint32_t a_renderFlags);

	/** @brief Z-prepass sibling of RenderShadowInstanced. Emits the claimed whole-TRISHAPE static depth
	 *         casters as DrawIndexedInstanced with a BYTE-EXACT FP32 World stream (64 B/instance) and a
	 *         bespoke R32G32B32A32 instance input layout (the engine ILCreate hardcodes R16). Byte-exact
	 *         is mandatory: the z-prepass depth is the early-Z equality reference for the forward pass.
	 *         Reuses the shadow grouping / technique setup / id|Instanced VS fetch verbatim. */
	void RenderDepthInstancedFP32(RE::BSRenderPass* const* a_passes, const std::uint32_t* a_techniques,
		std::uint32_t a_count, std::uint32_t a_renderFlags);

	/** @brief True when the pass is inside the replica's current RE coverage:
	 *         non-custom TRISHAPE / SUB_INDEX_TRISHAPE geometry plus skinned passes on
	 *         the static skin-instance Render branch. Stencil-above-water and the
	 *         dynamic bone-setter branch stay whole-pass engine. */
	[[nodiscard]] bool CanReplicate(RE::BSRenderPass* a_pass) const;

	/**
	 * @brief Shadow-capture hook (ShadowInstancingFix fan-out). While set, OnRenderPassImmediately
	 *        offers each utility pass to the hook BEFORE its own coverage logic. Return true to
	 *        signal "the caller took ownership of this pass" -- the replica then skips its
	 *        inline render entirely (the worker pool will replay it later). Return false to
	 *        let the replica render the pass normally (observe-only capture). The hook runs on
	 *        the render thread during the shadow-map walk. Params mirror RenderPassImmediately;
	 *        `canReplicate` is the coverage verdict so the hook can leave uncovered passes to
	 *        the engine. Set to nullptr to detach. */
	using ShadowCaptureHook = bool (*)(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags, bool a_canReplicate);
	void SetShadowCaptureHook(ShadowCaptureHook a_hook) { shadowCaptureHook.store(a_hook, std::memory_order_release); }

private:
	UtilityPassReplica() = default;

	void InstallHooks();

	/** @brief Replicated BSGraphics::Renderer::DrawTriShape (1.5.97 0x140D6BFE0):
	 *         dirty-state flush + IB/VB binds + DrawIndexed(3*tris, start, 0). */
	void DrawTriShapeReplica(void* a_rendererData, std::uint32_t a_startIndex, std::uint32_t a_triCount);

	/** @brief Replicated skinned dispatcher (1.5.97 0x141308970), static branch:
	 *         ShaderSetup (raw alpha-test), draw-struct build, optional dynamic-shape
	 *         ring upload, skin-instance Render vfunc, RestoreGeometry. */
	void ReplicaRenderSkinned(RE::BSRenderPass* a_pass, bool a_alphaTest, std::uint32_t a_renderFlags);

	std::atomic<ShadowCaptureHook> shadowCaptureHook{ nullptr };
	bool                           hooksInstalled = false;
};
