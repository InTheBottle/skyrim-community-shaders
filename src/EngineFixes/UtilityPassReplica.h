#pragma once

#include <d3d11_1.h>

#include <array>
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
 *  - Modes (CS_UTIL_RE_MODE):
 *      0 = off       : engine renders everything (shipping behaviour, hooks inert).
 *      1 = compare   : per utility pass, the engine renders (commands recorded), then
 *                      the replica renders the same pass (commands recorded). Depth-only
 *                      passes are idempotent, so the double-render is visually harmless;
 *                      the two command windows are diffed immediately and any divergence
 *                      is logged with full pass identity. This is the mechanical
 *                      "exactly identical" check, per pass, in situ.
 *      2 = replace   : the replica renders utility passes INSTEAD of the engine (the
 *                      "switch off game code" proof). Non-utility passes untouched.
 *  - The D3D11CommandRecorder detours the immediate-context vtable entries the pass
 *    window can touch and records normalized (call, args, data-hash) tuples while a
 *    per-pass window is open. Zero overhead when no window is armed (single bool).
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

	enum class Mode : std::uint32_t
	{
		kOff = 0,
		kCompare = 1,
		kReplace = 2,
	};

	[[nodiscard]] Mode GetMode() const { return mode.load(std::memory_order_relaxed); }
	[[nodiscard]] bool IsActive() const { return GetMode() != Mode::kOff; }
	[[nodiscard]] bool HooksInstalled() const { return hooksInstalled; }

	/** @brief Runtime mode switch (devbench A/B): only meaningful when the hooks were
	 *         installed at Setup (launch with CS_UTIL_RE_MODE != 0); the detour reads the
	 *         mode per pass, so flipping mid-session is safe -- a frame split between
	 *         engine and replica passes stays coherent because both share all state. */
	bool SetMode(Mode a_mode)
	{
		if (!hooksInstalled)
			return false;
		mode.store(a_mode, std::memory_order_relaxed);
		return true;
	}

	/** @brief Install the RenderPassImmediately detour (the seam ShadowInstancingFix's instancing
	 *         path rides to observe each utility pass and offer it to the shadow-capture hook) once. */
	void EnsureInitialized();

	/** @brief RenderPassImmediately detour body. Routes utility passes per mode;
	 *         forwards everything else to the engine untouched. */
	void OnRenderPassImmediately(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags);

	/**
	 * @brief The replica itself: issue the exact engine-equivalent command stream for
	 *        one utility pass (technique/material change detection, ShaderSetup,
	 *        geometry dispatch, dirty-state flush, DrawIndexed). RE'd from 1.5.97.
	 */
	void ReplicaRenderPassImmediately(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags);

	/** @brief Shadow instancing: render a set of captured static shadow passes as a small number of
	 *         DrawIndexedInstanced calls (one per unique mesh+technique group) instead of one DrawIndexed
	 *         per object. Each object's World matrix (identical to the b2 PerGeometry World the engine
	 *         builds) is packed into a per-instance vertex stream and consumed by the INSTANCED Utility VS.
	 *         Must be called while the shadow map's RT/DSV/viewport are still bound (per-map replay point).
	 *         a_passes / a_techniques are parallel arrays; a_count entries; a_renderFlags is the map's flags. */
	void RenderShadowInstanced(RE::BSRenderPass* const* a_passes, const std::uint32_t* a_techniques,
		std::uint32_t a_count, std::uint32_t a_renderFlags);

	/** @brief Instanced-path command-validation counters (always-on, negligible cost):
	 *         { mapsValidated, invariantViolations, packChecks, packMismatches }. Nonzero
	 *         violations/mismatches = the instanced submission no longer covers exactly the
	 *         claimed pass set (dropped/duplicated casters or a broken FP16 pack). */
	[[nodiscard]] std::array<std::uint32_t, 4> InstValReport() const;

	/** @brief True when the pass is inside the replica's current RE coverage:
	 *         non-custom TRISHAPE / SUB_INDEX_TRISHAPE geometry plus skinned passes on
	 *         the static skin-instance Render branch. Stencil-above-water and the
	 *         dynamic bone-setter branch stay whole-pass engine. */
	[[nodiscard]] bool CanReplicate(RE::BSRenderPass* a_pass) const;

	/**
	 * @brief Shadow-capture hook (ShadowInstancingFix fan-out). While set, OnRenderPassImmediately
	 *        offers each utility pass to the hook BEFORE its own mode logic. Return true to
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

	std::atomic<Mode>              mode{ Mode::kOff };
	std::atomic<ShadowCaptureHook> shadowCaptureHook{ nullptr };
	bool                           hooksInstalled = false;

	std::uint64_t passesUnsupported = 0;  ///< outside replica coverage -> engine fallback
};
