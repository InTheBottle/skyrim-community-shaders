#pragma once

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
		kCapture = 1,  // M1/M2: observe + log the per-map partition (inline render unchanged).
		kSerial = 2,   // Phase 0: claim the covered passes, replay them from the mapWorkList on
		               //          ONE deferred context in map order, ExecuteCommandList in place.
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

	std::atomic<Mode> mode{ Mode::kOff };
	bool              hooksInstalled = false;
	std::uint64_t     frames = 0;
};
