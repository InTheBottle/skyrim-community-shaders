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
//
// Grass InstanceData1.xyz is CELL-RELATIVE; the batch's STATIC World folds it to world space, then the per-frame
// camera view-proj takes it to clip: clip = mul(camVP, mul(World, cellRelPos)) == the engine's b2 WorldViewProj.

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
	row_major float4x4 CameraViewProj;      // this frame's camera view-proj (frustum / distance / thinning)
	row_major float4x4 PrevCameraViewProj;  // last frame's view-proj (Hi-Z reproject; grid is last frame's)
	float g_RadiusWorld;                    // conservative clump bounding radius (world units)
	float g_DistanceEnd;                    // hard-cull clumps past this clip.w (0 = disabled)
	float g_ThinStart;                      // clip.w where stochastic thinning begins
	float g_ThinEnd;                        // clip.w of maximum thinning
	uint  g_BatchCount;
	float g_ThinMax;                        // max fraction thinned at g_ThinEnd (0..1; 0 = disabled)
	uint  g_HiZValid;                       // 1 = sample the frame-lagged Hi-Z grid (t3)
	uint  g_HiZGridW;
	uint  g_HiZGridH;
	uint  g_HiZFullW;
	uint  g_HiZFullH;
	float g_HiZBias;                        // NDC-z slack; keep grass within this of the cell's farthest depth
};

StructuredBuffer<GrassInstance> g_Instances : register(t0);  // concatenated engine instance streams (mirrored)
StructuredBuffer<BatchDesc>     g_Batches   : register(t1);  // per-batch offsets/count
StructuredBuffer<float4x4>      g_World     : register(t2);  // per-batch STATIC World (row-major; engine b2 c8)
Texture2D<float>                g_HiZGrid   : register(t3);  // OcclusionCulling Hi-Z grid: farthest depth / 16px, LAST frame
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
	const float4x4 W = transpose(g_World[batchIdx]);
	const float3   worldPos = mul(W, float4(pos, 1.0)).xyz;
	const float4   clip = mul(CameraViewProj, float4(worldPos, 1.0));
	const float    w = clip.w;

	// 1. Frustum cull (clip-space; uses clip.w for depth -- convention-robust). Small clump -> w-relative margin.
	const float m = 0.15 * abs(w) + 0.0001;
	bool        visible = (w > 0.0) &&
					(clip.x >= -w - m) && (clip.x <= w + m) &&
					(clip.y >= -w - m) && (clip.y <= w + m) &&
					(clip.z <= w + m);

	// 2. Hard distance cull (past the fade-out end).
	if (visible && g_DistanceEnd > 0.0 && w > g_DistanceEnd)
		visible = false;

	// 3. Random distance thinning (stochastic LOD).
	if (visible && g_ThinMax > 0.0 && g_ThinEnd > g_ThinStart) {
		const float t = saturate((w - g_ThinStart) / (g_ThinEnd - g_ThinStart));
		if (Hash13(worldPos) > 1.0 - g_ThinMax * t)
			visible = false;
	}

	// 4. Frame-lagged Hi-Z occlusion. The grid is LAST frame's max-reduced scene depth (grass is in the
	//    z-prepass, so THIS frame's grid would self-occlude); reproject the STATIC clump with LAST frame's VP
	//    into that grid and drop it if it is behind the farthest occluder in its screen cell. STANDARD Z
	//    (near=0/far=1): the grid holds the farthest (max) depth, so occluded == clumpNdcZ > cellMax + bias
	//    (mirrors the OcclusionCulling HiZTestRect). A 3x3 cell neighbourhood takes the MAX (most conservative)
	//    to absorb one-frame camera motion + coarse 16px cells; over-cull only thins grass -- the z-prepass and
	//    forward pass cull identically, so a dropped clump is never a hole.
	if (visible && g_HiZValid != 0) {
		const float4 pclip = mul(PrevCameraViewProj, float4(worldPos, 1.0));
		if (pclip.w > 0.0) {
			const float  objZ = pclip.z / pclip.w;
			const float2 pndc = pclip.xy / pclip.w;
			const float2 uv = float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5);
			if (objZ <= 0.9995 && all(uv >= 0.0) && all(uv <= 1.0)) {  // skip sky/far + last-frame-offscreen
				const int2 c = int2(uv * float2(g_HiZFullW, g_HiZFullH)) / 16;
				float      cellMax = 0.0;
				[unroll] for (int dy = -1; dy <= 1; ++dy)
					[unroll] for (int dx = -1; dx <= 1; ++dx) {
						const int2 cc = clamp(c + int2(dx, dy), int2(0, 0), int2(g_HiZGridW - 1, g_HiZGridH - 1));
						cellMax = max(cellMax, g_HiZGrid.Load(int3(cc, 0)));
					}
				if (objZ > cellMax + g_HiZBias)
					visible = false;
			}
		}
	}

	if (visible) {
		uint idx;
		InterlockedAdd(g_Counters[batchIdx], 1u, idx);
		const uint base = (bd.dstOffset + idx) * 32u;
		g_Compacted.Store4(base, inst.a);
		g_Compacted.Store4(base + 16u, inst.b);
	}
}
