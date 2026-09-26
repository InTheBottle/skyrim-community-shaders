#include "Common/FrameBuffer.hlsli"

// Per-instance culling for billboard tree LOD (BSDistantTreeShader's blocks). Same bindings and
// constant layout as GrassCullingCS so the feature's bucket machinery drives both; only the fields
// listed below are read. Survivors keep their raw 32-byte record, so the vanilla vertex layout
// still applies, and carry their block's origin in the extras buffer.
cbuffer CullParams : register(b0)
{
	float4 FrustumPlanes[6];

	float MinPixelSize;
	float FullDetailPixelSize;
	float LODMinKeep;
	float LODFadeBand;

	float MeshCostBias;
	float ProjScale;
	float MaxDistSq;  // zero means unlimited
	float EdgeFadeStart;

	float AlphaParam1;
	float AlphaParam2;
	float FadeNow;
	float FadeInTimeRcp;

	float InvisibleFadeCull;
	float SimpleShadingPixelSize;
	float CollisionDistSq;
	float MidLODPixelSize;

	float MeshLODBandPx;
	float HiZEnabled;
	float2 HiZSize;

	float HiZTexelPixels;
	float HiZMipCount;
	float OcclusionBias;
	float CostBiasStartDist;

	float FarLODPixelSize;
	float3 _pad0;
};

cbuffer CullBucket : register(b1)
{
	uint InstanceCount;
	float WavePeriod;
	float TimeBase;
	float PrevTimeBase;
	float3 BoundCenter;
	float ModelRadius;
	float DistScale;
	float MinPixelScale;
	float IsComplex;
	float MidLODEnabled;
	// The dispatch covers the instances referenced by this slice range.
	uint SliceTableOffset;
	uint SliceCount;
	float FarLODEnabled;
	float _pad2;
};

ByteAddressBuffer Instances : register(t0);
StructuredBuffer<float4> Origins : register(t1);
// Scene-depth max pyramid; see GrassHiZCS.hlsl.
Texture2D<float> HiZ : register(t2);
// .x is the first source instance; .y is the compacted start index.
StructuredBuffer<uint2> SliceTable : register(t3);

RWByteAddressBuffer Compacted : register(u0);
// Two per instance: [0] = block origin.xyz + 1.0 (marks a merged draw), [1] unused.
RWStructuredBuffer<float4> Extras : register(u1);
RWByteAddressBuffer Counter : register(u2);

[numthreads(64, 1, 1)] void main(uint3 tid : SV_DispatchThreadID)
{
	const uint compactIdx = tid.x;
	if (compactIdx >= InstanceCount || SliceCount == 0)
		return;

	// Find the source slice containing this compacted index.
	uint lo = 0;
	uint hi = SliceCount - 1;
	[loop] while (lo < hi)
	{
		const uint mid = (lo + hi + 1) >> 1;
		if (SliceTable[SliceTableOffset + mid].y <= compactIdx)
			lo = mid;
		else
			hi = mid - 1;
	}
	const uint2 slice = SliceTable[SliceTableOffset + lo];
	const uint idx = slice.x + (compactIdx - slice.y);

	const uint base = idx * 32;
	const uint4 raw0 = Instances.Load4(base);
	const uint4 raw1 = Instances.Load4(base + 16);

	// DistantTree.hlsl: InstanceData1 = position.xyz + scale, InstanceData2.xy = rotation cos/sin.
	const float3 localPos = float3(f16tof32(raw0.x & 0xFFFF), f16tof32(raw0.x >> 16), f16tof32(raw0.y & 0xFFFF));
	const float scale = f16tof32(raw0.y >> 16);
	const float rotCos = f16tof32(raw0.z & 0xFFFF);
	const float rotSin = f16tof32(raw0.z >> 16);

	const float4 og = Origins[idx];
	const float3 world = localPos + og.xyz;

	// Bounding sphere of the scaled, rotated billboard, matching the vertex shader's transform.
	const float3 boundCentre = BoundCenter * scale;
	const float3 rotatedCentre = float3(rotCos * boundCentre.x - rotSin * boundCentre.y, rotSin * boundCentre.x + rotCos * boundCentre.y, boundCentre.z);
	const float3 centre = world + rotatedCentre;
	const float radius = ModelRadius * abs(scale);

	const float3 dv = centre - FrameBuffer::CameraPosAdjust.xyz;
	const float distSq = dot(dv, dv);
	if (MaxDistSq > 0.0 && distSq > MaxDistSq)
		return;

	[unroll]
	for (uint p = 0; p < 6; ++p)
	{
		if (dot(FrustumPlanes[p].xyz, centre) - FrustumPlanes[p].w < -radius)
			return;
	}

	const float dist = max(sqrt(distSq), 1e-4);
	const float projPx = (radius / dist) * ProjScale;
	if (projPx < MinPixelSize)
		return;

	if (HiZEnabled > 0.5)
	{
		const float4 clipC = mul(FrameBuffer::CameraViewProj, float4(dv, 1.0));
		if (clipC.w > 0.0)
		{
			const float2 uv = (clipC.xy / clipC.w) * float2(0.5, -0.5) + 0.5;
			const float2 tc = uv * HiZSize;
			// Projected radius in level-0 Hi-Z texels.
			const float rT = projPx / HiZTexelPixels;

			// Select a mip coarse enough for the 3x3 footprint to cover the sphere.
			const float wantLevel = ceil(log2(max(2.0 * rT, 1.0)));

			// Skip occlusion when no available mip can cover the sphere safely.
			[branch] if (wantLevel <= HiZMipCount - 1.0)
			{
				const int level = (int)wantLevel;
				const float mipScale = exp2((float)level);
				const float2 tcL = tc / mipScale;
				const float rTL = rT / mipScale;
				const int2 dimL = max(int2(ceil(HiZSize / mipScale)), int2(1, 1));

				const int2 t0 = int2(floor(tcL - rTL));
				const int2 t1 = int2(floor(tcL + rTL));

				// Test the sphere's nearest point; a camera inside it yields a non-occluded depth.
				const float3 dvNear = dv * (max(dist - radius, 0.0) / dist);
				const float4 clipN = mul(FrameBuffer::CameraViewProj, float4(dvNear, 1.0));
				const float nearZ = clipN.z / max(clipN.w, 1e-4);

#ifdef REVERSE_Z
				float tileMax = 1.0;
#else
				float tileMax = 0.0;
#endif
				[unroll] for (int y = 0; y < 3; ++y)
				{
					[unroll] for (int x = 0; x < 3; ++x)
					{
						if (t0.x + x <= t1.x && t0.y + y <= t1.y)
						{
							const int2 t = clamp(t0 + int2(x, y), int2(0, 0), dimL - 1);
#ifdef REVERSE_Z
							tileMax = min(tileMax, HiZ.Load(int3(t, level)));
#else
							tileMax = max(tileMax, HiZ.Load(int3(t, level)));
#endif
						}
					}
				}

				// Cull only when the sphere is behind every sampled tile, allowing for depth error.
#ifdef REVERSE_Z
				if (nearZ < tileMax - OcclusionBias)
#else
				if (nearZ > tileMax + OcclusionBias)
#endif
					return;
			}
		}
	}

	uint slot;
	Counter.InterlockedAdd(0, 1, slot);
	Compacted.Store4(slot * 32, raw0);
	Compacted.Store4(slot * 32 + 16, raw1);
	Extras[slot * 2 + 0] = float4(og.xyz, 1.0);
	Extras[slot * 2 + 1] = float4(0.0, 0.0, 1.0, 0.0);
}
