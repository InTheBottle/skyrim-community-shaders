#include "../Common/DeferredShared.hlsli"
#include "../Common/FrameBuffer.hlsl"
#include "../Common/Spherical Harmonics/SphericalHarmonics.hlsli"

cbuffer SkylightingCB : register(b1)
{
	row_major float4x4 OcclusionViewProj;
	float4 OcclusionDir;

	float4 PosOffset;
	uint4 ArrayOrigin;
	uint4 ValidID0;
	uint4 ValidID1;
};

Texture2D<unorm float> srcOcclusionDepth : register(t0);

RWTexture3D<sh2> outProbeArray : register(u0);

SamplerState samplerPointClamp : register(s0);

#define ARRAY_DIM uint3(128, 128, 64)
#define ARRAY_SIZE float3(8192, 8192, 8192 * 0.5)

[numthreads(8, 8, 1)] void main(uint3 dtid
								: SV_DispatchThreadID) {
	const static float lerpFactor = rcp(256);
	const static float rcpMaxUint = rcp(4294967295.0);

	uint3 cellID = (int3(dtid) - ArrayOrigin.xyz) % ARRAY_DIM;
	bool isValid = all(cellID >= ValidID0.xyz) && all(cellID <= ValidID1.xyz);  // check if the cell is newly added

	float3 cellCentreMS = cellID + 0.5 - ARRAY_DIM / 2;
	cellCentreMS = cellCentreMS / ARRAY_DIM * ARRAY_SIZE + PosOffset.xyz;

	float3 cellCentreOS = mul(OcclusionViewProj, float4(cellCentreMS, 1)).xyz;
	cellCentreOS.y = -cellCentreOS.y;
	float2 occlusionUV = cellCentreOS.xy * 0.5 + 0.5;

	if (all(occlusionUV > 0) && all(occlusionUV < 1)) {
		float occlusionDepth = srcOcclusionDepth.SampleLevel(samplerPointClamp, occlusionUV, 0);
		bool visible = cellCentreOS.z < occlusionDepth;

		sh2 occlusionSH = shScale(shEvaluate(OcclusionDir.xyz), float(visible));
		if (isValid)
			occlusionSH = shAdd(shScale(outProbeArray[dtid], 1 - lerpFactor), shScale(occlusionSH, lerpFactor));  // exponential accumulation
		outProbeArray[dtid] = occlusionSH;
	} else if (!isValid) {
		outProbeArray[dtid] = 1;
	}
}