#pragma once

// -----------------------------------------------------------------------------
// GPU Hi-Z occlusion for CommunityShaders. Each frame the scene depth is max-reduced
// into a coarse grid on the GPU and read back (HiZPrepass); the cull walk projects each
// object's bounds and rejects those provably behind the nearest grid cell (HiZTestRect).
//
// SE 1.5.97 ONLY (SKYRIM flat, not VR).
// -----------------------------------------------------------------------------

#include <cstdint>

#include <DirectXMath.h>
#include <immintrin.h>
#include <smmintrin.h>

struct ID3D11ShaderResourceView;

namespace RE
{
	class NiCamera;
	class NiAVObject;
}

namespace MOC
{
	// The Hi-Z projection helpers in MOC.cpp (CalculateViewProjection, the AABB/sphere tests)
	// use DirectXMath types (XMMATRIX/XMVECTOR) and SSE intrinsics.
	using namespace DirectX;

	// --- Runtime settings consumed by the Hi-Z occlusion path. The OcclusionCulling
	//     Feature writes these from its serialized settings; MOC reads them. ---
	extern bool          EnableOcclusionTesting;     // master gate for TestObject/TestMultiBound
	extern float         OccluderTestMinRadius;      // min world-bound radius for per-object tests
	extern bool          CullSmallShadows;           // shadow-map distance-scaled small-caster cull (shadow only)
	extern float         SmallShadowMinSize;         // shadow cull: base bound-radius threshold at the camera
	extern float         SmallShadowSlope;           // shadow cull: threshold growth per unit of camera distance

	/** @brief Mark the occlusion feature live. Hi-Z GPU resources are created lazily. */
	void Init();

	/** @brief Release the Hi-Z GPU resources and clear the initialized flag. */
	void Shutdown();

	/** @brief True once Init() has run. */
	bool IsInitialized();

	/**
	 * @brief Expose the GPU Hi-Z grid (max-reduced post-z-prepass depth, STANDARD Z near=0/far=1) for
	 *        a GPU consumer's own cull dispatch (grass). Returns the grid SRV, its cell/full dims, and
	 *        the frame it was reduced. Grass reads it a frame late (the texture is overwritten later in
	 *        the same frame at HiZPrepass), so callers gate on (currentFrame - a_buildFrame == 1).
	 * @return false if the Hi-Z grid is not yet allocated.
	 */
	bool GetHiZGridForCompute(ID3D11ShaderResourceView*& a_srv, int& a_gridW, int& a_gridH,
		int& a_fullW, int& a_fullH, std::uint64_t& a_buildFrame);

	/**
	 * @brief Main-scene per-frame Hi-Z step: gate the pass by main-camera identity, CAS-claim the
	 *        frame, and load the published depth snapshot's matrices into the shared test globals.
	 * @return true if this pass is the main world view and occlusion testing is active.
	 */
	bool BuildOccluders(RE::NiCamera* a_camera);

	/**
	 * @brief Hi-Z GPU occlusion frame step: max-reduce this frame's scene depth into a coarse
	 *        grid on the GPU, harvest the oldest staging-ring readback into the CPU snapshot the
	 *        cull tests against, and queue this frame's copy. RENDER THREAD ONLY (immediate
	 *        context) -- called from Deferred::PrepassPasses after the frame's depth is populated.
	 */
	void HiZPrepass();

	/**
	 * @brief Occlusion query for a scene object.
	 * @return true if the object may be visible (keep it), false if provably occluded.
	 *         Returns true (conservative) for all early-outs (not initialized, testing
	 *         disabled, app-culled, tiny/near bounds, no Hi-Z snapshot yet).
	 */
	bool TestObject(RE::NiAVObject* a_object);

	/**
	 * @brief Occlusion query for a multibound container (rooms, cells, building shells)
	 *        from the engine's TestBaseVisibility path. Tight AABB test.
	 * @return true if possibly visible; false if provably occluded.
	 */
	bool TestMultiBound(void* a_multiBound);

	/**
	 * @brief True only for the RENDER camera (the scene-list accumulation cull); the gate that
	 *        restricts occlusion testing to the main view.
	 */
	bool IsSceneListCamera(const RE::NiCamera* a_camera);

	/** @brief True for the sun shadow-gather camera (stage-1 caster pre-gather). */
	bool IsSunGatherCamera(const RE::NiCamera* a_camera);

	/** @brief Small-caster shadow cull -- drops the shadow only (keep=true; never actors/kAlwaysDraw). */
	bool TestShadowCasterSmall(RE::NiAVObject* a_object);
}
