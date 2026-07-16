#pragma once

#include <cstdint>

struct ID3D11DeviceContext;

// Staggered shadow-map updates ("round-robin") for local lights: re-render ONE shadow-map UNIT per
// frame and reuse (blit) every other unit's cached depth slice. A UNIT groups the maps that update
// together:
//   * spot light      = 1 map     -> 1 unit
//   * point/parabolic = 2 halves  -> 1 unit (both update together, no seam)
// Because interiors render only local maps, "one unit per frame" means one local light indoors.
//
// Correctness: local shadow maps are LIGHT-space (camera-independent), so a reused local slice stays
// correct as the player moves/looks around; only a dynamic caster UNDER that light lags by up to
// (unit-count - 1) frames -- a soft, low-res shadow. A moved light gets a new unit key -> immediate
// re-render. Skipping the engine's per-map walk (callOriginal) entirely is NOT viable -- it orphans
// the load-bearing incremental pass-node free and hangs; this only skips the static-caster draws.
namespace ShadowMapCache
{
	// Called once at the start of the shadow phase (advances the frame counter + picks the active unit).
	void BeginFrame();

	// Per-map decision. rmode: 13 spot / 15 point. Returns true if this map should be RENDERED fresh
	// this frame (its unit is the active one, it moved, or it has no valid cache); false if the caller
	// should REUSE the cached slice (call Blit). a_lightXf = descriptor+0x00.
	bool ShouldRender(void* a_camera, std::uint32_t a_rmode, const std::uint8_t* a_lightXf);

	// Copy the freshly-rendered atlas slice (this frame's) into the unit's private cache layer. Call
	// after the render on a ShouldRender==true map. Splits the copy so no single CopySubresourceRegion
	// spans a full D16 subresource (the DXVK quirk) and copies only the port rect.
	void Capture(ID3D11DeviceContext* a_ctx, void* a_camera, std::uint32_t a_slice, const std::int32_t* a_port);

	// Restore the unit's cached layer into this frame's atlas slice. Returns false if there is no
	// valid cache for this camera (caller must render instead).
	bool Blit(ID3D11DeviceContext* a_ctx, void* a_camera, std::uint32_t a_slice, const std::int32_t* a_port);

	// Lazily allocate the private cache array (mirrors the shadow atlas, target 4). False if not ready.
	bool EnsureCache();
}
