#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

#if defined(USE_SIGMA)
#	include "NRD/NRDSigma.hlsli"
#endif

Texture2D<float> srcDepth : register(t0);
Texture2D<float4> srcNormalRoughness : register(t1);
Texture2DArray<float4> ShadowCascadeMap : register(t2);

struct DirectionalShadowLightData
{
	column_major float4x4 ShadowProj[2];
	column_major float4x4 InvShadowProj[2];
	float2 EndSplitDistances;
	float2 StartSplitDistances;
	float4 CascadeDepthParams;
};
StructuredBuffer<DirectionalShadowLightData> DirectionalShadowLights : register(t3);

RWTexture2D<float> outShadow : register(u0);

SamplerComparisonState comparisonSampler : register(s0);
#if defined(USE_SIGMA)
SamplerState linearSampler : register(s1);
#endif

static const float JITTER_RADIUS = 128.0;
static const float TAN_SUN_ANGULAR_RADIUS = 0.00465;

#if defined(USE_SIGMA)
float LinearizeDepth(float depth, float cascadeNear, float cascadeFar)
{
	return cascadeNear * cascadeFar / (cascadeFar - depth * (cascadeFar - cascadeNear));
}
#endif

[numthreads(8, 8, 1)] void main(uint2 dtid
								: SV_DispatchThreadID) {
	float depth = srcDepth[dtid];
	if (depth >= 1.0) {
#if defined(USE_SIGMA)
		outShadow[dtid] = NRD_FP16_MAX;
#else
		outShadow[dtid] = 1.0;
#endif
		return;
	}

	float2 uv = (dtid + 0.5) * SharedData::BufferDim.zw;
	uv *= FrameBuffer::DynamicResolutionParams2.xy;
	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	uv = Stereo::ConvertFromStereoUV(uv, eyeIndex);

	float4 positionCS = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
	positionCS = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], positionCS);
	float3 positionWS = positionCS.xyz / positionCS.w;
	float3 positionWSAbsolute = positionWS + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;

	DirectionalShadowLightData shadowData = DirectionalShadowLights[0];

#if defined(USE_SIGMA)
	float distToOccluder = NRD_FP16_MAX;

	float3 noise = float3(Random::pcg3d(uint3(dtid, SharedData::FrameCount))) / 4294967295.0 * 2.0 - 1.0;
	float3 jitteredWS = positionWSAbsolute + noise * JITTER_RADIUS;

	float ndcDepth = FrameBuffer::GetShadowDepth(jitteredWS - FrameBuffer::CameraPosAdjust[eyeIndex].xyz, eyeIndex);
	float linearDepth = SharedData::GetScreenDepth(ndcDepth);

	if (linearDepth > 0 && linearDepth < shadowData.EndSplitDistances.y) {
		float cascadeSelect = saturate((linearDepth - shadowData.StartSplitDistances.y) /
			(shadowData.EndSplitDistances.x - shadowData.StartSplitDistances.y));
		uint cascadeIndex = uint(cascadeSelect);

		float3 positionLS = mul(shadowData.ShadowProj[cascadeIndex], float4(jitteredWS, 1)).xyz;

		if (all(positionLS.xy >= 0) && all(positionLS.xy <= 1)) {
			float4 shadowDepths = ShadowCascadeMap.GatherRed(linearSampler, float3(positionLS.xy, cascadeIndex));

			float4 depthParams = shadowData.CascadeDepthParams;
			float cascadeNear = cascadeIndex == 0 ? depthParams.x : depthParams.z;
			float cascadeFar = cascadeIndex == 0 ? depthParams.y : depthParams.w;

			float pixelLinZ = LinearizeDepth(positionLS.z, cascadeNear, cascadeFar);

			float distSum = 0;
			uint occluderCount = 0;
			for (uint i = 0; i < 4; i++) {
				float occluderLinZ = LinearizeDepth(shadowDepths[i], cascadeNear, cascadeFar);
				if (occluderLinZ < pixelLinZ) {
					distSum += pixelLinZ - occluderLinZ;
					occluderCount++;
				}
			}

			if (occluderCount > 0)
				distToOccluder = distSum / occluderCount;
		}

		float fade = saturate(linearDepth / shadowData.EndSplitDistances.y);
		float fadeFactor = 1.0 - pow(fade * fade, 8);
		if (fadeFactor < 1.0 && distToOccluder < NRD_FP16_MAX)
			distToOccluder = lerp(NRD_FP16_MAX, distToOccluder, fadeFactor);
	}

	outShadow[dtid] = SIGMA_FrontEnd_PackPenumbra(distToOccluder, TAN_SUN_ANGULAR_RADIUS);
#else
	float3 noise = float3(Random::pcg3d(uint3(dtid, SharedData::FrameCount))) / 4294967295.0 * 2.0 - 1.0;
	float3 jitteredWS = positionWSAbsolute + noise * JITTER_RADIUS;

	float shadowSample = 1.0;

	float ndcDepth = FrameBuffer::GetShadowDepth(jitteredWS - FrameBuffer::CameraPosAdjust[eyeIndex].xyz, eyeIndex);
	float linearDepth = SharedData::GetScreenDepth(ndcDepth);

	if (linearDepth > 0 && linearDepth < shadowData.EndSplitDistances.y) {
		float cascadeSelect = saturate((linearDepth - shadowData.StartSplitDistances.y) /
			(shadowData.EndSplitDistances.x - shadowData.StartSplitDistances.y));
		uint cascadeIndex = uint(cascadeSelect);

		float3 positionLS = mul(shadowData.ShadowProj[cascadeIndex], float4(jitteredWS, 1)).xyz;

		if (all(positionLS.xy >= 0) && all(positionLS.xy <= 1)) {
			shadowSample = ShadowCascadeMap.SampleCmpLevelZero(comparisonSampler, float3(positionLS.xy, cascadeIndex), positionLS.z);
		}

		float fade = saturate(linearDepth / shadowData.EndSplitDistances.y);
		float fadeFactor = 1.0 - pow(fade * fade, 8);
		shadowSample = lerp(1.0, shadowSample, fadeFactor);
	}

	outShadow[dtid] = shadowSample;
#endif
}
