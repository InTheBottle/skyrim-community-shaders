#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include <d3d11_1.h>
#include <winrt/base.h>

/**
 * @brief Multithreaded shadow-map recording built on the byte-exact UtilityPassReplica.
 *
 * GOAL (user, 2026-07-12): render Skyrim's shadow-map caster passes across worker threads --
 * each shadow map (sun cascade / point-spot light map) recorded on its own D3D11 deferred
 * context by a worker, then replayed on the immediate context in the engine's canonical map
 * order -- and prove, throughout, that the threaded output is identical to vanilla. Design +
 * gates: docs/development/mt-shadow-plan.md. Foundation: UtilityPassReplica reproduces every
 * covered BSUtilityShader pass byte-for-byte (parity gate: 213k passes / 0 diverged), so a
 * worker can re-issue a captured pass on its private context via ReplicaRenderPassImmediately.
 *
 * BUILD STAGES (each gated by its own validator before the next):
 *   M1  capture     : detour the shadow-map walk; partition the covered passes (per frame,
 *                     later per map) via UtilityPassReplica's ShadowCaptureHook. Engine/replica
 *                     still renders inline -- pure observation, validates the fan-out seam.
 *   M2  serial replay + Level-2 output diff (scratch atlas vs vanilla).
 *   M3  N deferred contexts + thread pool + ordered ExecuteCommandList + Level-1/2.
 *   M4  scale to per-map N; skinned/sub-index phase.
 *
 * MODES (CS_SHADOW_MT env, read once at Setup):
 *   0 = off      : hooks inert.
 *   1 = capture  : M1 -- observe + log the covered-pass partition of each shadow-map walk.
 */
class ShadowThreaded
{
public:
	static ShadowThreaded* GetSingleton()
	{
		static ShadowThreaded singleton;
		return &singleton;
	}

	enum class Mode : std::uint32_t
	{
		kOff = 0,
		kCapture = 1,       // M1/M2: observe + log the per-map partition (inline render unchanged).
		kSerial = 2,        // Phase 0: claim the covered passes, replay them from the mapWorkList on
		                    //          ONE deferred context via global-context redirection.
		kWorkerSerial = 3,  // Phase 1a: replay via a single ShadowWorker (PRIVATE block+caches, no
		                    //           global redirection) on the render thread -- proves the
		                    //           worker-block path renders correct shadows before threading.
		kConcurrent = 4,    // Phase 1b: N worker threads, each recording a subset of the maps onto
		                    //           its own deferred context; command lists execute in place.
		kOwnBeginPass = 5,  // Stage 1a (full-ownership track): single-threaded. The engine still does
		                    //           the per-map slice alloc + RT/DSV setup + clear + camera walk, but
		                    //           every BSBatchRenderer::BeginPass (0x141308030) -- the point where
		                    //           "renderpasses are ready to call RenderPassImmediately" -- routes
		                    //           to UtilityPassReplica::BeginPassReplica, a 1:1 reimplementation of
		                    //           the per-group DX11 state machine + the m_PassGroupNext pass loop
		                    //           (ReplicaRenderPassImmediately) + the RestoreTechnique cleanup.
		                    //           Validates the pass-orchestration ownership before ownership climbs
		                    //           up to RenderGeometryGroup -> RenderShadowmap setup/clear.
		kOwnBeginPassVerify = 6,  // Stage 1a command-level validation: for every pure-covered shadow
		                    //           BeginPass, run the engine's original AND BeginPassReplica from the
		                    //           same starting state and diff the dispatch sequence + 0x5D8 block
		                    //           delta + group-advance. diverged=0 proves the reimplemented
		                    //           orchestration is byte-exact (per-pass DX11 already byte-exact).
		kDrawStateVerify = 7,  // Regime-B MT gate (needs CS_RE_REFLECT): for every covered shadow pass,
		                    //           render it via the engine AND via the WORKER path (private block +
		                    //           full reimpl -- the exact MT code) from the same state, and diff the
		                    //           per-draw EFFECTIVE-STATE fingerprints (used CB bytes + all pipeline
		                    //           objects, normalized to ignore identity/order/context/unused). diff=0
		                    //           proves the MT path renders identically without command equality,
		                    //           the gate that frees MT to be optimized. See vanilla::DrawState.
		kParallelCull = 8,  // Perf track: parallelize the shadow CULL (97.8% of RenderShadowmaps, inline
		                    //           serial, render-thread frame limiter under DLSS). NiCamera::sub_1412C1600
		                    //           (the per-map cull walk) runs on worker threads into its map's private
		                    //           accumulator; the cheap submit (Func43, 2%) stays serial. Stage 1 =
		                    //           dispatch-and-join per map (proves the cull is correct off the render
		                    //           thread); Stage 2 = defer + fan out + serial submit phase. Target >=50%.
		kInstance = 9,      // Perf track (THE FPS lever): reduce the shadow DRAW COUNT via same-mesh
		                    //           instancing. Claim the instanceable subset (whole-TRISHAPE, non-skinned,
		                    //           non-alpha-test) per map; the engine renders the rest inline. Per map,
		                    //           group the claimed passes by (mesh, technique) and issue ONE
		                    //           DrawIndexedInstanced per group with each object's World in a per-instance
		                    //           vertex stream (UtilityPassReplica::RenderShadowInstanced). ~9x fewer
		                    //           shadow draws in dense exterior (88.9% instanceable). Inline on the
		                    //           immediate context (render thread) so the draw-count cut is the FPS win.
		kInstanceOwnVerify = 10,  // Walk-ownership foundation (docs/development/shadow-walk-parallelization.md):
		                    //           renders identically to kInstance, but ALSO direct-reads the accumulator's
		                    //           BSBatchRenderer pass chains (accum+0x130 -> renderPass inline PassGroup[]
		                    //           stride 0x30, walked via passGroupNext) in the Func43 hook -- BEFORE the
		                    //           engine walk -- and compares its instanceable-pass count to what
		                    //           CaptureHook collects during the walk. diverged=0 proves a self-contained
		                    //           direct-read can find the same passes without the engine driving the walk,
		                    //           the prerequisite for skipping the walk + moving traverse+build to workers.
		kEnumInstance = 13, // ENUMERATION REBUILD (docs/development/shadow-enumeration-rebuild.md). At Func42
		                    //           entry (0x1412CAC20 -- RT/DSV/viewport/camera bound, accumulator populated
		                    //           by the already-parallel cull), DirectReadEnumerate reads the caster chains
		                    //           ourselves. M0 proved enumeration == CaptureHook exactly.
		                    //           VERDICT (2026-07-15): SKIPPING Func42 to render from our enumeration is
		                    //           NON-VIABLE over the shared accumulator -- Func42's walk frees the pass
		                    //           nodes incrementally (sub_141307E80 -> NiMemFree, gated m_AutoClearPasses)
		                    //           with no per-frame bulk pool reset, so skipping corrupts the shared pool
		                    //           within ~1 frame: a passGroupNext chain cycles and the engine hangs on it
		                    //           (FREEZE proven, FPS=0). Same wall as kWalkMT. DEFAULT is now the safe
		                    //           observational path (enumerate+log; engine renders+frees); the skip path is
		                    //           opt-in (CS_SHADOW_ENUM_SKIP=1) as the harness for the only viable route:
		                    //           M3 PRIVATE per-light accumulators (own pool+free). The instancing win is
		                    //           already shipped via kInstance (mode 9), which keeps the engine free intact.
		kWalkMT = 12,       // CULLING MT (superseded by kEnumInstance): the per-map WALK runs on worker threads. The night's RE
		                    //           proved (a) 100+ engine sites read the one global D3D context slot
		                    //           0x143027EA0 -> raw engine render cannot parallelize; (b) serial-off-
		                    //           thread (dispatch+join) is STABLE with raw Func42 -> the crash is purely
		                    //           CONCURRENT immediate-ctx access. So each worker runs the walk with
		                    //           BeginPass (0x141308030) diverted to BeginPassReplica on its OWN deferred
		                    //           context (explicit ctx, never the global slot) + a WorkerSeedMap'd private
		                    //           block. Render thread does the render-thread-only setup (slice/clear/
		                    //           camera) + block snapshot per map, then fans the walks out; ordered
		                    //           ExecuteCommandList at the barrier. CS_SHADOW_WALKMT_JOIN=1 forces
		                    //           dispatch+join (serial validation gate) before enabling concurrency.
	};

	[[nodiscard]] Mode GetMode() const { return mode.load(std::memory_order_relaxed); }
	[[nodiscard]] bool IsActive() const { return GetMode() != Mode::kOff; }
	[[nodiscard]] bool HooksInstalled() const { return hooksInstalled; }

	bool SetMode(Mode a_mode)
	{
		if (!hooksInstalled)
			return false;
		mode.store(a_mode, std::memory_order_relaxed);
		return true;
	}

	/** @brief Read CS_SHADOW_MT, install the shadow-walk detour. Called from State::Setup. */
	void Setup();

	/** @brief Post-RenderShadowmaps state validation (leak detector): devbench-requested arming
	 *         (CS_SHADOW_STATE_VALIDATE env also arms it) + counter readback.
	 *         Report layout: { baselineFrames, checkedFrames, persistentDivergences, canaryHits,
	 *                          exitCheckedFrames, boundaryDiffs }. */
	std::atomic<bool> stateValidationRequested{ false };
	/** @brief Detector self-test: while true, each kInstance frame deliberately desyncs the GPU
	 *         rasterizer from the engine's CPU model; the armed validator MUST report divergences.
	 *         Runtime-armed only (devbench stateval {"selftest":true}) -- enabling it during a load
	 *         screen breaks loading, so never tie it to a boot-time env. */
	std::atomic<bool> stateValSelftest{ false };
	[[nodiscard]] std::array<std::uint32_t, 6> StateValReport() const;

	/** @brief DrawWorld::RenderShadowmaps (0x1412E3480) detour body: arm the capture hook,
	 *         run the original walk (the covered passes flow through the hook), disarm, report. */
	void RenderShadowmapsDetour(void* a_original);

	/** @brief BSShadowLight::RenderShadowmap (0x141305610, AddrLib 100820) planning interceptor.
	 *         One invocation == one shadow map (one {DSV-target, slice}). Brackets the map so the
	 *         capture hook partitions its passes, and snapshots the map's params (camera=desc+64,
	 *         accumulator=desc+72, target=desc+84, slice=desc+88, renderFlags=a4) BY VALUE into the
	 *         ordered mapWorkList -- the unit a worker will replay. @return the original's result. */
	std::int32_t RenderShadowmapDetour(void* a1, std::int64_t a2, void* a3, std::int32_t a4, void* a_original);

private:
	ShadowThreaded() = default;

	void InstallHooks();

	// Concurrent replay of the claimed passes across the maps.
	void ReplayWorkerSerial();  // kWorkerSerial: one ShadowWorker, render thread.
	void ReplayConcurrent();    // kConcurrent: workerCount threads, one deferred context each.

	std::atomic<Mode> mode{ Mode::kOff };
	bool              hooksInstalled = false;
	std::uint64_t     frames = 0;
	std::uint32_t     workerCount = 2;  // CS_SHADOW_MT_WORKERS (kConcurrent)
};
