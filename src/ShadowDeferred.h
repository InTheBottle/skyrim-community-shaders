#pragma once

#include <atomic>
#include <cstdint>

#include <d3d11_1.h>
#include <winrt/base.h>

namespace RE
{
	class BSRenderPass;
}

/**
 * @brief Runs the engine's shadow-map render commands on a D3D11 DEFERRED context,
 *        separate from the game's immediate stream (Stage D of the utility RE baseline).
 *
 * GOAL (user, 2026-07-11): the engine's shadow-map D3D11 work is removed from the
 * immediate context; we issue every shadow command ourselves onto a deferred context,
 * record it into a command list, and ExecuteCommandList at the exact point the engine
 * would have submitted -- so the frame is bit-identical. Shadow depth passes use
 * engine-ORIGINAL shaders (CS does not modify shadow depth passes, so the CS
 * BeginTechnique substitution thunks are bypassed for the shadow scope).
 *
 * WHY DEFERRED + PRIVATE STATE: the eventual goal is to build the shadow command list
 * off the render thread. That requires the shadow path to touch only privately-owned
 * state -- its own RendererShadowState dirty block for SetDirtyStates flushes, its own
 * constant-buffer pools/rings -- never the immediate context's live globals. Phase 1
 * records single-threaded, in place, but is structured so the recording can move to a
 * worker later without touching the immediate context or the shared dirty block.
 *
 * MODES (CS_SHADOW_DEFERRED env, read once at Setup):
 *   0 = off      : engine renders shadows as usual (hooks inert).
 *   1 = deferred : the shadow loop is detoured; its commands are recorded onto the
 *                  deferred context (immediate context untouched) and executed in place.
 *
 * Foundation: the shadow passes are exactly the BSUtilityShader passes UtilityPassReplica
 * already reproduces byte-identically; this module reuses that command coverage while
 * swapping the target context and isolating the state.
 */
class ShadowDeferred
{
public:
	static ShadowDeferred* GetSingleton()
	{
		static ShadowDeferred singleton;
		return &singleton;
	}

	enum class Mode : std::uint32_t
	{
		kOff = 0,
		kDeferred = 1,
	};

	[[nodiscard]] Mode GetMode() const { return mode.load(std::memory_order_relaxed); }
	[[nodiscard]] bool IsActive() const { return GetMode() != Mode::kOff; }
	[[nodiscard]] bool HooksInstalled() const { return hooksInstalled; }

	/** @brief Runtime mode switch (devbench A/B): only meaningful when the detour was
	 *         installed at boot (launch with CS_SHADOW_DEFERRED=1). The detour reads the
	 *         mode per frame -- mode 0 runs the engine's shadow loop untouched, mode 1
	 *         records it on the deferred context -- so flipping mid-session is safe and
	 *         enables single-session engine-vs-deferred screenshot A/B. */
	bool SetMode(Mode a_mode)
	{
		if (!hooksInstalled)
			return false;
		mode.store(a_mode, std::memory_order_relaxed);
		return true;
	}

	/** @brief True while a shadow scope is recording onto the deferred context. CS's
	 *         BeginTechnique substitution thunks read this to bypass shader substitution
	 *         and to target the deferred context instead of the immediate one. Render
	 *         thread only in phase 1; promoted to thread-local when recording moves off
	 *         the render thread. */
	[[nodiscard]] bool ShadowScopeActive() const { return scopeActive; }

	/** @brief The context the shadow scope issues on: the deferred context while a scope
	 *         is active, else null. CS hooks that must follow the redirect call this. */
	[[nodiscard]] ID3D11DeviceContext* ScopeContext() const { return scopeActive ? deferredContext.get() : nullptr; }

	/** @brief Read CS_SHADOW_DEFERRED, create the deferred context, install the detour.
	 *         Called from State::Setup once the device is ready. Inert at mode 0. */
	void Setup();

	/** @brief DrawWorld::RenderShadowmaps (0x1412E3480) detour body: redirect the context
	 *         globals to the deferred context, run the original shadow loop (recording),
	 *         then FinishCommandList + ExecuteCommandList in place, isolating the render
	 *         state around the scope. @param a_original pointer to the trampoline func. */
	void RenderShadowmapsDetour(void* a_original);

private:
	ShadowDeferred() = default;

	void InstallHooks();

	std::atomic<Mode>                      mode{ Mode::kOff };
	bool                                   hooksInstalled = false;
	bool                                   scopeActive = false;
	winrt::com_ptr<ID3D11DeviceContext>    deferredContext;

	// Private copy of the engine's dynamic-VB ring buffer (skinned faces). Swapped into
	// the engine ring-slot pointers only while the shadow scope records, so the shadow
	// path's deferred-context DISCARD maps never touch the shared buffers the immediate
	// context reuses for the main-view pass. Created lazily from the engine buffer's desc.
	winrt::com_ptr<ID3D11Buffer>           privateDynVB;

	// Rolling stats for the log.
	std::uint64_t framesDeferred = 0;
};
