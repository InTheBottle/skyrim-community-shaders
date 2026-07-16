#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

// Staggered shadow-map updates ("round-robin"): the PROVEN parts of the local-light shadow-cache
// investigation, distilled. The whole-map depth blit is byte-exact and never stalls (see
// comshaders-local-light-shadow-cache memory); what was missing was a way to decide when a cached
// slice is still valid. Round-robin sidesteps that: instead of trying to detect staleness, it just
// updates ONE shadow-map UNIT per frame and reuses (blits) every other unit's cached slice.
//
// A UNIT groups the maps that must update together:
//   * spot light          = 1 map      -> 1 unit
//   * point/parabolic      = 2 halves   -> 1 unit (both update together, no seam)
//   * directional cascade  = 1 map      -> 1 unit (one cascade per frame)
// Because interiors render only local maps and exteriors only sun cascades, "one unit per frame"
// means one local light indoors and one cascade outdoors -- automatically.
//
// Correctness: local shadow maps are LIGHT-space (camera-independent), so a reused local slice stays
// correct as the player moves/looks around; only a dynamic caster UNDER that light lags by up to
// (unit-count - 1) frames -- a soft, low-res shadow. A moved light gets a new unit key -> immediate
// re-render. Directional cascades are camera-relative, so reusing them lags the whole scene's sun
// shadow while moving (opt-in via mode 2).
namespace ShadowMapCache
{
	// 0 = off, 1 = stagger local lights (spot 13 / point 15) via map-level blit, 2 = also stagger
	// directional cascades (14). One shadow-map UNIT is re-rendered per frame; every other unit reuses
	// (blits) its cached depth slice. NOTE: skipping the engine's per-map walk (callOriginal) entirely is
	// NOT viable -- it orphans the load-bearing incremental pass-node free and hangs (see the shadow-ab
	// live-methodology memory / culling-mt); mode 1/2 keep the walk and only skip the static-caster draws.
	void SetMode(int a_mode);
	int  GetMode();

	// Called once at the start of the shadow phase (advances the frame counter + picks the active unit).
	void BeginFrame();

	// Per-map decision. rmode: 13 spot / 14 dir-cascade / 15 point. Returns true if this map should be
	// RENDERED fresh this frame (its unit is the active one, it moved, or it has no valid cache);
	// false if the caller should REUSE the cached slice (call Blit). a_lightXf = descriptor+0x00.
	bool ShouldRender(void* a_camera, std::uint32_t a_rmode, const std::uint8_t* a_lightXf);

	// Copy the freshly-rendered atlas slice (this frame's) into the unit's private cache layer. Call
	// after the engine/instanced render on a ShouldRender==true map. Splits the copy so no single
	// CopySubresourceRegion spans a full D16 subresource (the DXVK quirk) and copies only the port rect.
	void Capture(ID3D11DeviceContext* a_ctx, void* a_camera, std::uint32_t a_slice, const std::int32_t* a_port);

	// Restore the unit's cached layer into this frame's atlas slice. Returns false if there is no
	// valid cache for this camera (caller must render instead).
	bool Blit(ID3D11DeviceContext* a_ctx, void* a_camera, std::uint32_t a_slice, const std::int32_t* a_port);

	// Lazily allocate the private cache array (mirrors the shadow atlas, target 4). False if not ready.
	bool EnsureCache();

	// Measurement (zero-risk): called from the detour passthrough for every shadow-atlas map the engine
	// actually renders this frame. target=descriptor+84 (4 = shadow atlas); rmode=13 spot/14 cascade/15
	// point. Bumps Updates()=all atlas maps, Reuses()=local (spot/point) maps, so the devbench per-second
	// rate answers "how much per-frame local-shadow work is there to skip in this scene?".
	void NotePassthroughMap(std::uint32_t a_target, std::uint32_t a_rmode);

	// Counters (devbench).
	std::uint64_t Updates();
	std::uint64_t Reuses();
	std::uint64_t Units();
}
