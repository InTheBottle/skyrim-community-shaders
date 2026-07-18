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

	// Per-map decision. rmode: 13 spot / 15 point. a_staticSig is the caller's signature of this map's claimed
	// STATIC caster set. Returns true if the map must be RENDERED fresh (new unit, changed static set, or no
	// valid cache); false if the caller should REUSE the cached slice (call Blit). a_lightXf = descriptor+0x00.
	bool ShouldRender(void* a_camera, std::uint32_t a_rmode, const std::uint8_t* a_lightXf, std::uint64_t a_staticSig);

	// Copy the freshly-rendered atlas slice (this frame's) into the unit's private cache layer. Call after the
	// render on a ShouldRender==true map. Splits the copy so no single CopySubresourceRegion spans a full D16
	// subresource (the DXVK quirk) and copies only the port rect.
	void Capture(ID3D11DeviceContext* a_ctx, void* a_camera, std::uint32_t a_slice, const std::int32_t* a_port);

	// Restore the unit's cached layer into this frame's atlas slice. Returns false if there is no valid cache
	// for this camera (caller must render instead).
	bool Blit(ID3D11DeviceContext* a_ctx, void* a_camera, std::uint32_t a_slice, const std::int32_t* a_port);

	// Lazily allocate the private cache array (mirrors the shadow atlas, target 4). False if not ready.
	bool EnsureCache();

	// Snapshot of the last completed frame's cache activity, for the LightLimitFix UI.
	const Stats& GetStats();

	// Human-readable regeneration reason for the UI.
	const char* ReasonString(Reason a_reason);
}
