// GrassCullCS.hlsl — single-dispatch GPU culling of ALL grass batches.
//
// The engine draws grass as hundreds of instanced batches (BSMultiStreamInstanceTriShape); a per-batch compute
// dispatch is catastrophic on DXVK (each splits the render pass). Instead every batch's instance stream is
// concatenated into g_Instances and processed in ONE 2D dispatch: thread (x=localInstance, y=batch). Each
// SURVIVING 32-byte FP16 record (InstanceData1..4) is copied verbatim into the batch's region of g_Compacted,
// so the engine input layout and the grass VS/PS are unchanged (byte-exact survivors).
//
// Techniques (all free — one clip per clump):
//   1. Frustum cull       — clumps outside the view.
//   2. Hard distance cull — past the fade-out end (fully alpha-faded -> invisible but still drawn).
//   3. Random distance thinning (stochastic LOD) — progressively drop a fraction of clumps with distance;
//        position-hashed so each clump's keep/drop is stable across frames (no flicker) and spatially uncorrelated.
//   4. Hi-Z occlusion — drop clumps fully behind this frame's opaque occluders (terrain/rocks/buildings).
//
// Grass InstanceData1.xyz is CELL-RELATIVE. Skyrim renders CAMERA-RELATIVE (RunGrass: WorldPosition = mul(World,
// msPos) with the camera at the origin, absolute world = WorldPosition + CameraPosAdjust), so the batch's captured
// World folds cellRelPos to camera-relative space AT ITS CAPTURE FRAME. CameraPosAdjust moves with the camera every
// frame, so the World is NOT frame-invariant: we store the capture-frame posAdjust per batch, add it back to recover
// the frame-invariant ABSOLUTE world position, then subtract THIS frame's posAdjust to re-project into the current
// camera-relative space before applying the current view-proj. Without this the reconstruction drifts as the camera
// moves and on-screen grass slides out of frustum ("grass disappears when moving"). Absolute pos also stabilises the
// thinning hash (else it would flicker as posAdjust changes each frame).

struct GrassInstance
{
	uint4 a;  // FP16 h0..h7
	uint4 b;  // FP16 h8..h15
};

struct BatchDesc
{
	uint srcOffset;  // first instance of this batch in g_Instances
	uint count;      // instances in this batch
	uint dstOffset;  // first survivor slot for this batch in g_Compacted
	uint triCount;   // blade-mesh triangle count (for the indirect args)
};

cbuffer CullParams : register(b0)
{
	row_major float4x4 CameraViewProj;  // this frame's camera view-proj (frustum / distance / thinning / Hi-Z project)
	float g_RadiusWorld;                // conservative clump bounding radius (world units)
	float g_DistanceEnd;                // hard-cull clumps past this clip.w (0 = disabled)
	float g_ThinStart;                  // clip.w where stochastic thinning begins
	float g_ThinEnd;                    // clip.w of maximum thinning
	uint  g_BatchCount;
	float g_ThinMax;                    // max fraction thinned at g_ThinEnd (0..1; 0 = disabled)
	uint  g_HiZValid;                   // 1 = test against this-frame's opaque Hi-Z grid (t3)
	uint  g_HiZGridW;
	uint  g_HiZGridH;
	uint  g_HiZFullW;
	uint  g_HiZFullH;
	float g_HiZHeight;                  // blade height: test the clump's TOP point (base + up*height)
	float g_HiZNear;                    // camera near/far -> linearize the grid's occluder NDC-z to a view distance
	float g_HiZFar;
	float g_HiZMargin;                  // world-units a clump-top must be BEHIND the occluder to count as occluded
	float g_HiZPad;
	float4 g_CamPosAdjust;              // THIS frame's camera position adjust (grass World is camera-RELATIVE)
	float g_ThinCurve;                  // thinning ramp exponent (higher = concentrate thinning toward the fade edge)
	float g_ThinScale;                  // survivor scale-up compensation strength (0 = off)
	float g_ThinPad0;
	float g_ThinPad1;
};

StructuredBuffer<GrassInstance> g_Instances : register(t0);  // concatenated engine instance streams (mirrored)
StructuredBuffer<BatchDesc>     g_Batches   : register(t1);  // per-batch offsets/count
StructuredBuffer<float4x4>      g_World     : register(t2);  // per-batch captured World (row-major; engine b2 c8, camera-relative)
Texture2D<float>                g_HiZGrid   : register(t3);  // OcclusionCulling Hi-Z grid: farthest depth / 16px, THIS frame
StructuredBuffer<float4>        g_CaptureAdjust : register(t4);  // per-batch camera posAdjust at capture (recovers absolute world)
RWByteAddressBuffer             g_Compacted : register(u0);  // survivors (also a vertex buffer)
RWStructuredBuffer<uint>        g_Counters  : register(u1);  // per-batch survivor counts (reset to 0 each frame)

// Stable, spatially-uncorrelated hash -> [0,1). Same clump position -> same value every frame (no thin flicker).
float Hash13(float3 p3)
{
	p3 = frac(p3 * 0.1031);
	p3 += dot(p3, p3.zyx + 31.32);
	return frac((p3.x + p3.y) * p3.z);
}

[numthreads(64, 1, 1)] void main(uint3 tid
								 : SV_DispatchThreadID) {
	const uint batchIdx = tid.y;
	if (batchIdx >= g_BatchCount)
		return;
	const BatchDesc bd = g_Batches[batchIdx];
	const uint local = tid.x;
	if (local >= bd.count)
		return;

	const GrassInstance inst = g_Instances[bd.srcOffset + local];

	// InstanceData1.xyz = cell-relative clump position.
	const float3 pos = float3(
		f16tof32(inst.a.x & 0xFFFFu),
		f16tof32(inst.a.x >> 16),
		f16tof32(inst.a.y & 0xFFFFu));

	// Reconstruct the engine clip per frame (moving-camera correct). StructuredBuffer<float4x4> reads column-major
	// but the engine World is row-major -> transpose so mul(W,v) applies rows; CameraViewProj is a row_major field.
	// The captured World is camera-relative to the CAPTURE frame: add the capture-frame posAdjust to get the
	// frame-invariant ABSOLUTE world, then subtract THIS frame's posAdjust for the current camera-relative position
	// that CameraViewProj expects. (See file header -- this is what makes the cull moving-camera correct.)
	const float4x4 W = transpose(g_World[batchIdx]);
	const float3   worldPosCapRel = mul(W, float4(pos, 1.0)).xyz;                    // camera-relative @ capture frame
	const float3   worldPosAbs = worldPosCapRel + g_CaptureAdjust[batchIdx].xyz;     // absolute world (frame-invariant)
	const float3   worldPos = worldPosAbs - g_CamPosAdjust.xyz;                      // camera-relative @ THIS frame
	const float4   clip = mul(CameraViewProj, float4(worldPos, 1.0));
	const float    w = clip.w;

	// 1. Frustum cull (clip-space; uses clip.w for depth -- convention-robust). The margin MUST cover the clump's
	//    blade extent: only the origin point is tested, but blades rise ~g_RadiusWorld world-units around it, so a
	//    clump whose origin is just off an edge can still have visible blades. A world extent projects to a
	//    near-CONSTANT clip margin (clip = ndc*w, ndc_extent = world/(w*tan) -> clip_extent = world/tan), so add a
	//    constant term as well as the w-relative slack -- else edge clumps pop in/out as the camera pitches/turns.
	const float m = 0.15 * abs(w) + g_RadiusWorld;
	bool        visible = (w > 0.0) &&
					(clip.x >= -w - m) && (clip.x <= w + m) &&
					(clip.y >= -w - m) && (clip.y <= w + m) &&
					(clip.z <= w + m);

	// 2. Hard distance cull (past the fade-out end).
	if (visible && g_DistanceEnd > 0.0 && w > g_DistanceEnd)
		visible = false;

	// 3. Random distance thinning (stochastic LOD). The local thinned fraction ramps across the fade region on a
	//    tunable curve (g_ThinCurve: >1 concentrates the drop toward the fade edge where grass is already faint).
	//    Computed once here and reused for the survivor scale-up compensation at store time. Position-hashed on the
	//    ABSOLUTE world pos so each clump's keep/drop is stable across frames (no flicker) and camera-independent.
	float thinFrac = 0.0;
	if (g_ThinMax > 0.0 && g_ThinEnd > g_ThinStart) {
		const float t = saturate((w - g_ThinStart) / (g_ThinEnd - g_ThinStart));
		thinFrac = g_ThinMax * pow(t, max(g_ThinCurve, 0.001));
	}
	if (visible && thinFrac > 0.0 && Hash13(worldPosAbs) > 1.0 - thinFrac)
		visible = false;

	// 4. Hi-Z occlusion vs THIS frame's OPAQUE-ONLY occluder grid (g_HiZGrid: the farthest depth-buffer z over each
	//    16px screen cell, reduced from the pre-grass scene depth -> holds only terrain/rocks/buildings, never grass).
	//    Test the clump's TOP point (base + blade height): if even the top is BEHIND the farthest opaque surface in
	//    its screen cell (by more than a physical margin), the whole clump is occluded -> drop it. Hole-safe by
	//    construction:
	//      - the grid is the FARTHEST opaque z, so a clump in front of anything visible through the cell survives;
	//      - sky / empty pixels read as the far-plane clear (max z), so a clump at a silhouette or poking above an
	//        occluder lands in a far-reading cell and is kept;
	//      - testing the TOP (not the base) keeps grass whose blades rise past a low wall.
	//    This-frame + opaque-only means no frame-lag flicker and no grass-vs-grass self-occlusion. The occluder's
	//    NDC-z is linearized to a view-space distance so the margin is a fixed world distance at any range.
	if (visible && g_HiZValid != 0) {
		const float4 topClip = mul(CameraViewProj, float4(worldPos + float3(0.0, 0.0, g_HiZHeight), 1.0));
		if (topClip.w > g_HiZNear) {
			const float2 uv = float2(topClip.x / topClip.w * 0.5 + 0.5, 0.5 - topClip.y / topClip.w * 0.5);
			if (all(uv >= 0.0) && all(uv <= 1.0)) {  // top on-screen (else keep -- conservative)
				const int2  cell = clamp(int2(uv * float2(g_HiZFullW, g_HiZFullH)) / 16,
					int2(0, 0), int2(g_HiZGridW - 1, g_HiZGridH - 1));
				const float occ = g_HiZGrid.Load(int3(cell, 0));  // farthest opaque NDC-z in the cell (standard: 1=far)
				const float wOcc = g_HiZNear / max(1.0 - occ * (g_HiZFar - g_HiZNear) / g_HiZFar, 1e-6);  // -> view dist
				if (topClip.w > wOcc + g_HiZMargin)               // clump-top >= margin behind the occluder -> occluded
					visible = false;
			}
		}
	}

	if (visible) {
		uint idx;
		InterlockedAdd(g_Counters[batchIdx], 1u, idx);
		const uint base = (bd.dstOffset + idx) * 32u;
		uint4      outB = inst.b;
		// Scale-up compensation: enlarge survivors by the local thinned fraction so a thinned field still reads as
		// full. InstanceData4.y (FP16 h13 = high half of inst.b.z) is the per-instance size -- the engine grass VS
		// builds each blade as Position * (InstanceData4.y * ScaleMask + 1), so ADDING to it grows the blade.
		if (g_ThinScale > 0.0 && thinFrac > 0.0) {
			const float sz = f16tof32(outB.z >> 16) + g_ThinScale * thinFrac;
			outB.z = (outB.z & 0xFFFFu) | (f32tof16(sz) << 16);
		}
		g_Compacted.Store4(base, inst.a);
		g_Compacted.Store4(base + 16u, outB);
	}
}
