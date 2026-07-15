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
		/// DEBUG (only populated when CS_UTIL_RE_DUMP is set): the written CB dwords for a
		/// kMapDiscardData record, so DiffWindows can report the exact diverging dword.
		std::shared_ptr<std::vector<std::uint32_t>> mapData;
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

	/** @brief Read CS_UTIL_RE_MODE, create resources, install hooks. Called from
	 *         State::Setup (device ready). Inert at mode 0. */
	void Setup();

	/** @brief Bring up the recorder/replica infrastructure (engine-call stub, RenderPassImmediately
	 *         detour + context recorder vfuncs) exactly once, independent of CS_UTIL_RE_MODE. The
	 *         ShadowThreaded BeginPass-ownership modes call this so the engine fallback trampoline and
	 *         the OnRenderPassImmediately observation point exist even with the replica mode off. */
	void EnsureInitialized();

	/** @brief A queryable snapshot of the structural-compare validation state. This is the
	 *  rerunnable vanilla-parity gate: reset the counters, run N frames in compare mode,
	 *  then read this back and assert diverged==0. `firstDivergingPass` pinpoints the
	 *  deviation (class + technique + the exact call index/field that first differed). */
	struct ValidationReport
	{
		std::uint64_t compared = 0;
		std::uint64_t diverged = 0;
		std::uint64_t divergedTrishape = 0;
		std::uint64_t divergedSubIndex = 0;
		std::uint64_t divergedSkinned = 0;
		std::uint64_t unsupported = 0;
		bool          haveFirstDiverge = false;
		// Populated on the first divergence since the last reset.
		std::uint32_t firstClass = 0;       ///< 0=trishape 1=subindex 2=skinned
		std::uint32_t firstTechnique = 0;
		std::uint64_t firstEngineCalls = 0;
		std::uint64_t firstReplicaCalls = 0;
		bool          firstSizeMismatch = false;
		std::uint64_t firstDiffIndex = 0;   ///< meaningful when !firstSizeMismatch
		std::uint32_t firstDiffField = 0;   ///< 0=kind 1=slot 2=a 3=b 4=c (the field that differed)
	};

	/** @brief Read the current validation counters (render-thread writes are plain; a torn
	 *  read is harmless for a monitoring snapshot). */
	[[nodiscard]] ValidationReport GetValidationReport() const;

	/** @brief Zero the compare counters + firstDivergingPass + refill the dump budgets, so
	 *  a fresh parity run starts clean. Safe to call from the tool thread. */
	void ResetValidation();

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
	 * @brief Shadow-capture hook (ShadowThreaded fan-out). While set, OnRenderPassImmediately
	 *        offers each utility pass to the hook BEFORE its own mode logic. Return true to
	 *        signal "the caller took ownership of this pass" -- the replica then skips its
	 *        inline render entirely (the worker pool will replay it later). Return false to
	 *        let the replica render the pass normally (observe-only capture). The hook runs on
	 *        the render thread during the shadow-map walk. Params mirror RenderPassImmediately;
	 *        `canReplicate` is the coverage verdict so the hook can leave uncovered passes to
	 *        the engine. Set to nullptr to detach. */
	using ShadowCaptureHook = bool (*)(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags, bool a_canReplicate);
	void SetShadowCaptureHook(ShadowCaptureHook a_hook) { shadowCaptureHook.store(a_hook, std::memory_order_release); }

	// --- Concurrent shadow fan-out (ShadowThreaded) -------------------------------------------
	// A ShadowWorker owns a PRIVATE 0x5D8 render-state block + technique/material/shadow-token
	// caches, so a worker thread can run the byte-exact setup pipeline (all CS_UTIL_RE_CSSETUP
	// reimpls, forced on when a worker is bound) on its OWN deferred context with zero shared
	// mutable state -- the precondition for recording shadow-map passes across threads. The
	// render-thread walk captures the covered passes per map; each worker replays a subset onto
	// its private context; the command lists execute on the immediate context afterwards.
	class ShadowWorker;  ///< opaque per-thread worker binding (block + caches + deferred context)

	/// Allocate a worker bound to @p a_ctx (a deferred context). @p a_initToken seeds the private
	/// shadow-geom-token cache from the global's pre-fan-out value.
	static ShadowWorker* MakeShadowWorker(ID3D11DeviceContext* a_ctx, std::uint32_t a_initToken);
	static void          FreeShadowWorker(ShadowWorker* a_worker);

	/// Seed one map's clean render-state block into the worker and reset the per-pass change-
	/// detection caches so the next flush re-binds the full RT/viewport/depth state.
	static void WorkerSeedMap(ShadowWorker* a_worker, const std::uint8_t* a_mapBlock);

	/// Bind/clear the worker as the calling thread's active private state (thread_local). Between
	/// Begin and End, ReplicaRenderPassImmediately reads/writes the worker's block/ctx/caches.
	static void WorkerBeginScope(ShadowWorker* a_worker);
	static void WorkerEndScope();

	/// Reimplementation of BSBatchRenderer::BeginPass (SE 1.5.97 0x141308030): the per-group DX11
	/// state setup + the m_PassGroupNext pass loop (each pass -> ReplicaRenderPassImmediately) + the
	/// RestoreTechnique cleanup, against the worker/engine block. Taking ownership here means the
	/// worker replay carries the per-group blend/depth state the raw pass capture missed. Returns the
	/// iterator-advance result (whether more groups remain). a2=&key, a3=&groupIndex, a4=&iterState.
	std::uint8_t BeginPassReplica(void* a_batchRenderer, void* a2, void* a3, void* a4, std::uint32_t a_renderFlags);

	/// Signature of the engine's original BSBatchRenderer::BeginPass (the detour trampoline).
	using BeginPassFn = void* (*)(void*, void*, void*, void*, std::uint32_t);

	/// Non-destructive enumeration compare for BeginPassReplica: computes the replica's expected pass
	/// dispatch by reading the (intact) chain, then runs the engine's BeginPass (a_engine = the detour
	/// trampoline) exactly ONCE for real -- it must not double-run, as BatchAdvance frees the group's
	/// list nodes -- and diffs the ordered (pass, key, alphaTest, renderFlags) it dispatched against the
	/// replica's. Proves the hash lookup + m_PassGroupNext walk + v11 derivation match. Returns the
	/// engine's iterator-advance result. Divergences are logged; totals via GetBeginPassCompareStats.
	std::uint8_t BeginPassCompare(void* a_batchRenderer, void* a2, void* a3, void* a4, std::uint32_t a_renderFlags, BeginPassFn a_engine);

	/// Cumulative BeginPassCompare tallies: total pure-covered groups compared and how many diverged.
	void GetBeginPassCompareStats(std::uint64_t& a_groups, std::uint64_t& a_diverged) const;

	/// Self-contained direct-read of an accumulator's BSBatchRenderer pass chains, counting the
	/// instanceable subset (whole-TRISHAPE, non-skinned, non-alpha-test = the same set CaptureHook
	/// claims in kInstance) and the remainder. Enumerates every group via the renderPassMap scatter
	/// table (br+0x48 entries, br+0x2C capacity), walks each PassGroup.passes[mode] chain
	/// (passArrayBase = *(br+8), head = *(base + 8*(mode+6*groupIdx)), next = passGroupNext). No engine
	/// walk drives it -- proves the direct-read finds the same passes, the prerequisite for owning +
	/// parallelizing the walk. a_accum = BSShaderAccumulator*. See shadow-walk-parallelization.md.
	void DirectReadInstanceableCount(void* a_accum, std::uint64_t& a_instanceable, std::uint64_t& a_other) const;

	/// Shadow-caster enumeration (the enumeration-rebuild source): walk the accumulator's populated
	/// pass chains (the SAME layout DirectReadInstanceableCount uses) and COLLECT the passes,
	/// classified. Instanceable (non-alpha whole-TRISHAPE non-skinned) -> (a_instPasses, a_instTechs)
	/// paired arrays for RenderShadowInstanced; everything else -> a_remainder. Pure CPU, no D3D --
	/// safe to run on a worker over a per-light PRIVATE accumulator. Must be called while the chains
	/// are populated (post-cull, pre-Func42; e.g. at RenderShadowmap/CullHook entry -- NOT Func43).
	/// Returns false (and clears the outputs) if a chain is cyclic/oversized or the layout is
	/// unreadable -- the caller must then fall back to the engine render for this map.
	[[nodiscard]] bool DirectReadEnumerate(void* a_accum, std::vector<RE::BSRenderPass*>& a_instPasses,
		std::vector<std::uint32_t>& a_instTechs, std::vector<RE::BSRenderPass*>& a_remainder) const;

	/// Regime-B MULTITHREAD gate for one covered pass: render it via the engine and via the WORKER path
	/// (private block + full setup/flush reimpl, the exact MT code) from the same state, and diff the
	/// per-draw effective-state fingerprints (vanilla::DrawStateValidator::CompareFingerprints). The
	/// worker runs on the immediate context but is draw-state-equivalent to a deferred worker. E==T proves
	/// the MT path binds identical effective state per draw. Requires CS_RE_REFLECT (reflection masking).
	void VerifyPassDrawStateThreaded(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags);

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

	std::atomic<Mode>              mode{ Mode::kOff };
	std::atomic<ShadowCaptureHook> shadowCaptureHook{ nullptr };
	bool                           hooksInstalled = false;

	// Per-pass command windows (render thread only).
	std::vector<RecordedCall> engineWindow;
	std::vector<RecordedCall> replicaWindow;

	// Rolling divergence stats for the log (compare mode).
	std::uint64_t passesCompared = 0;
	std::uint64_t passesDiverged = 0;
	std::uint64_t divergedByClass[3] = {};   ///< [0]=trishape [1]=subindex [2]=skinned
	std::uint32_t dumpBudgetByClass[3] = { 8, 24, 24 };  ///< per-class full-dump budgets
	std::uint64_t passesUnsupported = 0;     ///< outside replica coverage -> engine fallback

	// First divergence since the last ResetValidation() -- the pinpoint the parity gate
	// reports. Written once (guarded by firstDivergeCaptured) on the render thread.
	bool          firstDivergeCaptured = false;
	std::uint32_t firstDivergeClass = 0;
	std::uint32_t firstDivergeTechnique = 0;
	std::uint64_t firstDivergeEngineCalls = 0;
	std::uint64_t firstDivergeReplicaCalls = 0;
	bool          firstDivergeSizeMismatch = false;
	std::uint64_t firstDivergeIndex = 0;
	std::uint32_t firstDivergeField = 0;
};
