#pragma once

#include <d3d11_1.h>

#include <atomic>
#include <cstdint>

namespace RE
{
	class BSRenderPass;
}

/**
 * @brief The utility-pass (depth-only: shadow map / z-prepass) detour seam for the static local-light
 *        shadow cache. Detours BSBatchRenderer::RenderPassImmediately (SE 1.5.97 0x141308440) so that,
 *        during the shadow-map walk, each utility pass is offered to a capture hook; everything else is
 *        forwarded to the engine untouched.
 *
 * The cache (ShadowMapCacheHooks) arms the hook, which claims each STATIC caster (so the engine's inline
 * draw is skipped) and folds it into a change signature. On a fresh frame the claimed statics are replayed
 * via RenderPassesOriginal -- the engine's OWN RenderPassImmediately (the detour trampoline), one DrawIndexed
 * each, byte-for-byte what the engine would have drawn inline. There is NO instancing and NO byte-exact
 * replica any more; the cache's only optimization is skipping the static replay on unchanged frames (a blit).
 *
 * All engine-derived constants cite their 1.5.97 address; see docs/development/utility-pass-re.md.
 */
class UtilityPassReplica
{
public:
	static UtilityPassReplica* GetSingleton()
	{
		static UtilityPassReplica singleton;
		return &singleton;
	}

	/** @brief Install the RenderPassImmediately detour (the seam the shadow cache rides to observe each
	 *         utility pass and offer it to the capture hook) once. */
	void EnsureInitialized();

	/** @brief RenderPassImmediately detour body. Offers utility passes to the capture hook during the
	 *         shadow-map walk; forwards everything else to the engine untouched. */
	void OnRenderPassImmediately(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags);

	/** @brief Replay a set of captured shadow passes through the engine's ORIGINAL RenderPassImmediately
	 *         (the detour trampoline) -- byte-for-byte the draws the engine would have issued inline, one
	 *         DrawIndexed per caster, no instancing and no replica. This is the shadow cache's fresh-frame
	 *         render: the claimed static casters were skipped inline during the walk, so replay them here.
	 *         Must be called while the map's RT/DSV/viewport are bound (per-map replay point). Parallel
	 *         arrays of a_count entries; a_alphaTests / a_renderFlags are per-pass. Bypasses the capture
	 *         hook (calls the trampoline directly), so no pass is re-captured. */
	void RenderPassesOriginal(RE::BSRenderPass* const* a_passes, const std::uint32_t* a_techniques,
		const std::uint8_t* a_alphaTests, const std::uint32_t* a_renderFlags, std::uint32_t a_count);

	/** @brief True when the pass is a plain covered TRISHAPE / SUB_INDEX_TRISHAPE (or a static skin-instance)
	 *         caster -- the coverage verdict handed to the capture hook so it can leave uncovered passes to
	 *         the engine. Stencil-above-water and the dynamic bone-setter branch report false. */
	[[nodiscard]] bool CanReplicate(RE::BSRenderPass* a_pass) const;

	/**
	 * @brief Shadow-capture hook (ShadowMapCacheHooks fan-out). While set, OnRenderPassImmediately offers
	 *        each utility pass to the hook BEFORE forwarding. Return true to signal "the caller took ownership
	 *        of this pass" -- the inline render is then skipped (the cache replays it later). Return false to
	 *        let the engine render the pass normally. The hook runs on the render thread during the shadow-map
	 *        walk. Params mirror RenderPassImmediately; `canReplicate` is the coverage verdict. nullptr detaches. */
	using ShadowCaptureHook = bool (*)(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags, bool a_canReplicate);
	void SetShadowCaptureHook(ShadowCaptureHook a_hook) { shadowCaptureHook.store(a_hook, std::memory_order_release); }

private:
	UtilityPassReplica() = default;

	void InstallHooks();

	std::atomic<ShadowCaptureHook> shadowCaptureHook{ nullptr };
	bool                           hooksInstalled = false;
};
