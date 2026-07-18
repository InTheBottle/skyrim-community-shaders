#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

// Static/dynamic local-light shadow cache. Local shadow maps are re-rendered only when the STATIC caster set
// under a light changes; otherwise the cached depth slice is blitted forever. A "unit" groups the maps that
// update together:
//   * spot light      = 1 map     -> 1 unit
//   * point/parabolic = 2 halves  -> 1 unit (both cameras share a world position)
//
// A map is cache-eligible only when it drew NO dynamic caster this frame -- no skinned actor AND no swaying
// tree (TreeAnim). Such maps hold only STATIC, light-space (camera-independent) depth, so a reused blit stays
// correct as the player moves/looks around. Change detection is by a per-unit signature: an FNV hash over each
// claimed static caster's placed-reference identity + world transform. If the signature is unchanged the slice
// is blitted (no render); if it differs (a static was placed / disabled / moved) the map renders fresh once and
// the new signature is stored. In gameplay statics almost never change, so nearly every frame is a pure blit.
//
// Correctness: skipping the engine's per-map walk (callOriginal) entirely is NOT viable -- it orphans the
// load-bearing incremental pass-node free and hangs; this only skips the static-caster draws.
namespace ShadowMapCache
{
	// Why a unit last rendered fresh -- surfaced in the LightLimitFix UI.
	enum class Reason : std::uint32_t
	{
		None,             // nothing regenerated
		FirstBuild,       // a newly seen light
		MovedLight,       // the light moved -> its shadow view changed
		StaticSetChanged, // a static caster was added / disabled / moved under this light
		NoCache,          // the map had no valid cached slice yet (first capture)
	};

	// Per-frame telemetry for the UI. Counters reset each frame in BeginFrame; the lastRegen* fields are sticky.
	struct Stats
	{
		std::uint32_t frame = 0;           // authoritative cache clock
		std::uint32_t unitsTotal = 0;      // tracked local-light units
		std::uint32_t layersValid = 0;     // cached static slices held
		std::uint32_t freshThisFrame = 0;  // static (re)renders this frame
		std::uint32_t blitsThisFrame = 0;  // cached-slice reuses this frame
		std::uint32_t regenThisFrame = 0;  // static-set-change regenerations this frame
		std::uint32_t lastRegenFrame = 0;  // frame of the most recent regeneration
		Reason        lastReason = Reason::None;
		std::uint64_t lastReasonKey = 0;   // unit key (camera-position hash) of that regeneration
	};

	// Called once at the start of the shadow phase (advances the frame counter + publishes stats).
	void BeginFrame();

	enum class Action
	{
		RenderFull,     // no valid cache / light moved -> render statics over the inline dynamics; capture next frame
		StaticCapture,  // (re)build the base this frame: suppress dynamics, render statics static-only, capture it
		CopyBase,       // valid cache: copy the cached static base into the slice, then render dynamics ON TOP
	};

	// PRE-WALK decision for a local map, keyed by shadow camera. Chooses this frame's action from the unit's
	// state (valid cache? capture pending? light moved?). Must run before the caster walk so a CopyBase can lay
	// the base down before the engine draws dynamics. Updates the unit's position key.
	Action PreWalkDecide(void* a_camera);

	// After a StaticCapture: mark the base captured + valid, clear the pending-capture flag.
	void NoteCaptured(void* a_camera);

	// A caster's static/dynamic verdict flipped under this light (a static started / stopped moving) -> schedule a
	// rebuild of the base next frame. Cull-set churn never calls this.
	void NoteChanged(void* a_camera);

	// Copy the freshly-rendered atlas slice (this frame's) into the unit's private cache layer. Call after the
	// static-only render on a StaticCapture map. Splits the copy so no single CopySubresourceRegion spans a full
	// D16 subresource (the DXVK quirk) and copies only the port rect.
	void Capture(ID3D11DeviceContext* a_ctx, void* a_camera, std::uint32_t a_slice, const std::int32_t* a_port);

	// Copy the unit's cached static base INTO this frame's atlas slice (as the base the engine then draws dynamics
	// on top of). Returns false if there is no valid cache for this camera. The caller must have the slice's depth
	// target DETACHED (CopySubresourceRegion needs the destination unbound) and re-attach it afterwards (BindSlice).
	bool Blit(ID3D11DeviceContext* a_ctx, void* a_camera, std::uint32_t a_slice, const std::int32_t* a_port);

	// Re-attach this frame's atlas slice as the (depth-only) render target -- a per-slice D16 DSV on the shadow
	// atlas -- after Blit/Capture detached it, so the engine's caster draws land back in the slice.
	void BindSlice(ID3D11DeviceContext* a_ctx, std::uint32_t a_slice);

	// Lazily allocate the private cache array (mirrors the shadow atlas, target 4). False if not ready.
	bool EnsureCache();

	// Snapshot of the last completed frame's cache activity, for the LightLimitFix UI.
	const Stats& GetStats();

	// Human-readable regeneration reason for the UI.
	const char* ReasonString(Reason a_reason);
}
