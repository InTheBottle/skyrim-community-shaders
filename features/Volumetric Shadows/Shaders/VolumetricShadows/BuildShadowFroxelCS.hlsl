// Builds a view-space froxel grid of PCF-filtered directional shadow visibility.
//
// One thread per voxel:
//   1. Reconstruct the voxel's world-space (camera-relative) position via screen UV +
//      exponential view-Z slicing.
//   2. Pick the correct cascade for that view depth, smooth-blend at the boundary.
//   3. Sample the directional shadow cascade with a 5-tap cross PCF kernel and write
//      the visibility scalar to the grid.
//
// Consumers then sample the grid trilinearly via VolumetricShadows.hlsli without doing
// any cascade math or shadow projection of their own.

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

cbuffer VolumetricShadowsCB : register(b1)
{
	uint3 GridSize;
	uint HasShadows;
	float NearZ;
	float FarZFallback;
	float ShadowBias;
	float pad0;
};

struct DirectionalShadowLightData
{
	column_major float4x4 ShadowProj[2];
	column_major float4x4 InvShadowProj[2];
	float2 EndSplitDistances;
	float2 StartSplitDistances;
};

Texture2DArray<float> DirectionalShadowMap : register(t0);
StructuredBuffer<DirectionalShadowLightData> DirectionalShadowLights : register(t98);
RWTexture3D<float> ShadowFroxel : register(u0);
SamplerComparisonState ShadowSampler : register(s0);
SamplerState LinearSampler : register(s1);

// Convert a linear view-Z (positive forward) into the depth-buffer NDC z using Skyrim's
// CameraData. Inverse of SharedData::GetScreenDepth.
float ViewZToDeviceZ(float viewZ)
{
	return (SharedData::CameraData.x - SharedData::CameraData.w / viewZ) / SharedData::CameraData.z;
}

// Exponential mapping concentrates resolution close to the camera where shadow detail matters.
float SliceToViewZ(float slice, float farZ)
{
	float t = saturate(slice);
	return NearZ * pow(farZ / NearZ, t);
}

// 5-tap cross PCF using the hardware comparison sampler. Returns visibility in [0, 1].
float SampleDirectionalShadowPCF(float3 positionLS, uint cascadeIndex)
{
	uint shadowWidth;
	uint shadowHeight;
	uint shadowSlices;
	DirectionalShadowMap.GetDimensions(shadowWidth, shadowHeight, shadowSlices);
	if (cascadeIndex >= shadowSlices)
		return 1.0f;

	float2 texelSize = rcp(float2(max(shadowWidth, 1u), max(shadowHeight, 1u)));
	float compareDepth = positionLS.z - ShadowBias;

	// Fall back to a single tap near the cascade border to avoid bleeding from the neighbouring slice.
	float2 uvMin = texelSize * 1.5f;
	float2 uvMax = 1.0f.xx - uvMin;
	if (any(positionLS.xy < uvMin) || any(positionLS.xy > uvMax))
		return DirectionalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(saturate(positionLS.xy), cascadeIndex), compareDepth);

	float center = DirectionalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(positionLS.xy, cascadeIndex), compareDepth);
	float cross = DirectionalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(positionLS.xy + float2(texelSize.x, 0.0f), cascadeIndex), compareDepth);
	cross += DirectionalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(positionLS.xy - float2(texelSize.x, 0.0f), cascadeIndex), compareDepth);
	cross += DirectionalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(positionLS.xy + float2(0.0f, texelSize.y), cascadeIndex), compareDepth);
	cross += DirectionalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(positionLS.xy - float2(0.0f, texelSize.y), cascadeIndex), compareDepth);

	return (center * 4.0f + cross) * rcp(8.0f);
}

float SampleDirectionalShadow(float3 positionWS, float viewZ, uint eyeIndex)
{
	DirectionalShadowLightData light = DirectionalShadowLights[0];

	if (viewZ >= light.EndSplitDistances.y)
		return 1.0f;

	float splitDenom = max(light.EndSplitDistances.x - light.StartSplitDistances.y, 1e-4f);
	float cascadeSelect = saturate((viewZ - light.StartSplitDistances.y) / splitDenom);
	uint primaryCascade = (uint)cascadeSelect;

	float3 absolutePositionWS = positionWS + FrameBuffer::CameraPosAdjust[eyeIndex].xyz;
	float3 positionLS = mul(light.ShadowProj[primaryCascade], float4(absolutePositionWS, 1.0f)).xyz;
	if (any(positionLS.xy < 0.0f) || any(positionLS.xy > 1.0f))
		return 1.0f;

	float shadow = SampleDirectionalShadowPCF(positionLS, primaryCascade);

	[branch] if (cascadeSelect > 0.0f && cascadeSelect < 1.0f)
	{
		uint secondaryCascade = 1u - primaryCascade;
		float3 secondaryLS = mul(light.ShadowProj[secondaryCascade], float4(absolutePositionWS, 1.0f)).xyz;
		if (!any(secondaryLS.xy < 0.0f) && !any(secondaryLS.xy > 1.0f)) {
			float secondaryShadow = SampleDirectionalShadowPCF(secondaryLS, secondaryCascade);
			shadow = lerp(shadow, secondaryShadow, cascadeSelect);
		}
	}

	// Fade out smoothly near the far edge of the second cascade.
	float fade = saturate(viewZ / max(light.EndSplitDistances.y, 1.0f));
	float fadeFactor = 1.0f - pow(fade * fade, 8.0f);
	return lerp(1.0f, shadow, fadeFactor);
}

// Reconstruct camera-relative world position for a voxel center.
float3 ComputeVoxelWorldPosition(uint3 coord, out uint eyeIndex, out float viewZ)
{
	float3 volumeUVW = (float3(coord) + 0.5f) / float3(GridSize);

#if defined(VR)
	eyeIndex = Stereo::GetEyeIndexFromTexCoord(volumeUVW.xy);
	float2 eyeUV = Stereo::ConvertFromStereoUV(volumeUVW.xy, eyeIndex);
#else
	eyeIndex = 0;
	float2 eyeUV = volumeUVW.xy;
#endif

	DirectionalShadowLightData light = DirectionalShadowLights[0];
	float farZ = max(light.EndSplitDistances.y, NearZ + 1.0f);
	viewZ = SliceToViewZ(volumeUVW.z, farZ);

	float deviceZ = ViewZToDeviceZ(viewZ);

	float2 ndc = eyeUV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
	// Compose unjittered ViewProj inverse from ViewInverse * ProjUnjitteredInverse so the
	// grid is stable across frames (CameraViewProjInverse is the jittered variant).
	float4 viewH = mul(FrameBuffer::CameraProjUnjitteredInverse[eyeIndex], float4(ndc, deviceZ, 1.0f));
	float3 viewPos = viewH.xyz / viewH.w;
	float4 worldPosition = mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(viewPos, 1.0f));
	return worldPosition.xyz;
}

[numthreads(8, 8, 4)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID >= GridSize))
		return;

	if (HasShadows == 0u) {
		ShadowFroxel[dispatchID] = 1.0f;
		return;
	}

	uint eyeIndex;
	float viewZ;
	float3 positionWS = ComputeVoxelWorldPosition(dispatchID, eyeIndex, viewZ);

	float shadow = SampleDirectionalShadow(positionWS, viewZ, eyeIndex);
	ShadowFroxel[dispatchID] = saturate(shadow);
}
