#include "../Common/FrameBuffer.hlsl"
#include "../Common/GBuffer.hlsli"
#include "../Common/VR.hlsli"
#include "../Common/Spherical Harmonics/SphericalHarmonics.hlsli"

Texture2D<float> DepthTexture : register(t0);

Texture2D<unorm half4> SkylightingTexture : register(t1);
Texture2D<unorm half> WetnessOcclusionTexture : register(t2);

RWTexture2D<unorm float4> SkylightingTextureRW : register(u0);
RWTexture2D<unorm half> WetnessOcclusionTextureRW : register(u1);

cbuffer PerFrame : register(b0)
{
	row_major float4x4 OcclusionViewProj;
	float4 EyePosition;
	float4 ShadowDirection;
	float4 BufferDim;
	float4 CameraData;
	uint FrameCount;
};

SamplerState LinearSampler : register(s0);

half GetScreenDepth(half depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

// samples = 8, min distance = 0.5, average samples on radius = 2
static const float3 g_Poisson8[8] = {
	float3(-0.4706069, -0.4427112, +0.6461146),
	float3(-0.9057375, +0.3003471, +0.9542373),
	float3(-0.3487388, +0.4037880, +0.5335386),
	float3(+0.1023042, +0.6439373, +0.6520134),
	float3(+0.5699277, +0.3513750, +0.6695386),
	float3(+0.2939128, -0.1131226, +0.3149309),
	float3(+0.7836658, -0.4208784, +0.8895339),
	float3(+0.1564120, -0.8198990, +0.8346850)
};
[numthreads(8, 8, 1)] void main(uint3 globalId
								: SV_DispatchThreadID) {
	float2 uv = float2(globalId.xy + 0.5) * (BufferDim.zw )* DynamicResolutionParams2.xy;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);

	half weight = 1.0;

	half rawDepth = DepthTexture[globalId.xy];
	half depth = GetScreenDepth(rawDepth);

	sh2 skylightingSH = SkylightingTexture[globalId.xy] * 2.0 - 1.0;
	half wetnessOcclusion = WetnessOcclusionTexture[globalId.xy];

	for (uint i = 0; i < 8; i++) {
		int2 pxOffset = (g_Poisson8[i].xy * 2.0 - 1.0) * 3.0;
		uint2 offsetPosition = round(globalId.xy + pxOffset);
		float2 testUV = offsetPosition * BufferDim.zw;

		if (any(testUV < 0) || any(testUV > 1))
		  	continue;

		float sampleDepth = GetScreenDepth(DepthTexture[offsetPosition]);
		float attenuation = g_Poisson8[i].z * (1.0 - saturate(10.0 * abs(sampleDepth - depth) / depth));

		[branch] if (attenuation > 0.0)
		{
			skylightingSH = shAdd(skylightingSH, shScale(SkylightingTexture[offsetPosition] * 2.0 - 1.0, attenuation));
			wetnessOcclusion += WetnessOcclusionTexture[offsetPosition] * attenuation;
			weight += attenuation;
		}
	}

	skylightingSH = shScale(skylightingSH, 1.0 / weight);
	wetnessOcclusion /= weight;

	SkylightingTextureRW[globalId.xy] = skylightingSH * 0.5 + 0.5;
	WetnessOcclusionTextureRW[globalId.xy] = wetnessOcclusion;
}