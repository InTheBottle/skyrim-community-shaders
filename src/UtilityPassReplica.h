#pragma once

#include <d3d11_1.h>

#include <array>
#include <atomic>
#include <cstdint>
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

	/// One normalized D3D11 call inside a recorded pass window.
	struct RecordedCall
	{
		enum class Kind : std::uint16_t
		{
			kVSSetShader,
			kPSSetShader,
			kVSSetConstantBuffers,
			kPSSetConstantBuffers,
			kVSSetShaderResources,
			kPSSetShaderResources,
			kVSSetSamplers,
			kPSSetSamplers,
			kIASetInputLayout,
			kIASetVertexBuffers,
			kIASetIndexBuffer,
			kIASetPrimitiveTopology,
			kOMSetRenderTargets,
			kOMSetBlendState,
			kOMSetDepthStencilState,
			kRSSetState,
			kRSSetViewports,
			kMapDiscardData,  ///< Unmap of a WRITE_DISCARD map: hash of the written bytes
			kDrawIndexed,
		};

		Kind          kind;
		std::uint16_t slot;   ///< first slot / count-carrying field where applicable
		std::uint64_t a;      ///< normalized arg 1 (pointer value, enum, count...)
		std::uint64_t b;      ///< normalized arg 2
		std::uint64_t c;      ///< normalized arg 3 / data hash
	};

	[[nodiscard]] Mode GetMode() const { return mode.load(std::memory_order_relaxed); }
	[[nodiscard]] bool IsActive() const { return GetMode() != Mode::kOff; }

	/** @brief Read CS_UTIL_RE_MODE, create resources, install hooks. Called from
	 *         State::Setup (device ready). Inert at mode 0. */
	void Setup();

	/** @brief RenderPassImmediately detour body. Routes utility passes per mode;
	 *         forwards everything else to the engine untouched. */
	void OnRenderPassImmediately(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags);

	/**
	 * @brief The replica itself: issue the exact engine-equivalent command stream for
	 *        one utility pass (technique/material change detection, ShaderSetup,
	 *        geometry dispatch, dirty-state flush, DrawIndexed). RE'd from 1.5.97.
	 */
	void ReplicaRenderPassImmediately(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags);

	/** @brief True when the pass is inside the replica's current RE coverage:
	 *         non-custom TRISHAPE / SUB_INDEX_TRISHAPE geometry plus skinned passes on
	 *         the static skin-instance Render branch. Stencil-above-water and the
	 *         dynamic bone-setter branch stay whole-pass engine. */
	[[nodiscard]] bool CanReplicate(RE::BSRenderPass* a_pass) const;

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

	// --- compare-mode recording ---
	void BeginWindow(std::vector<RecordedCall>& a_sink);
	void EndWindow();
	void DiffWindows(RE::BSRenderPass* a_pass, std::uint32_t a_technique);

	std::atomic<Mode> mode{ Mode::kOff };
	bool              hooksInstalled = false;

	// Per-pass command windows (render thread only).
	std::vector<RecordedCall> engineWindow;
	std::vector<RecordedCall> replicaWindow;

	// Rolling divergence stats for the log (compare mode).
	std::uint64_t passesCompared = 0;
	std::uint64_t passesDiverged = 0;
	std::uint64_t divergedByClass[3] = {};   ///< [0]=trishape [1]=subindex [2]=skinned
	std::uint32_t dumpBudgetByClass[3] = { 8, 24, 24 };  ///< per-class full-dump budgets
	std::uint64_t passesUnsupported = 0;     ///< outside replica coverage -> engine fallback
};
