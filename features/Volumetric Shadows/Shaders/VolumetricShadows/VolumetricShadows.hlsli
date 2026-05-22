#ifndef __VOLUMETRIC_SHADOWS_HLSLI__
#define __VOLUMETRIC_SHADOWS_HLSLI__

// View-space froxel grid of PCF-filtered directional shadow visibility.
// Built each frame by BuildShadowFroxelCS.hlsl and bound at t18 by VolumetricShadows.cpp.

namespace VolumetricShadows
{
	Texture3D<float> SharedShadowMap : register(t18);

	static const float kFroxelNearZ = 16.0;

	// Match the build CS exponential slice mapping. The far distance comes from the
	// cascade's max split, which is shared at t98 already.
	float ViewZToSlice(float viewZ, float farZ)
	{
		float clampedZ = clamp(viewZ, kFroxelNearZ, farZ);
		return log(clampedZ / kFroxelNearZ) / log(max(farZ / kFroxelNearZ, 1.001));
	}

	// Project a camera-relative world position into the per-eye froxel grid UVW and read
	// trilinearly-filtered visibility. Returns 1.0 (fully lit) for positions outside the
	// frustum or beyond the cascade range.
	float SampleShadowFroxel(float3 positionWS, uint eyeIndex)
	{
		float4 clip = mul(FrameBuffer::CameraViewProjUnjittered[eyeIndex], float4(positionWS, 1.0));
		if (clip.w <= 0.0)
			return 1.0;

		float3 ndc = clip.xyz / clip.w;
		float2 screenUV = ndc.xy * float2(0.5, -0.5) + 0.5;
		if (any(screenUV < 0.0) || any(screenUV > 1.0) || ndc.z < 0.0 || ndc.z > 1.0)
			return 1.0;

		float viewZ = SharedData::GetScreenDepth(ndc.z);
		float farZ = max(DirectionalShadowLights[0].EndSplitDistances.y, kFroxelNearZ + 1.0);
		if (viewZ >= farZ || viewZ <= 0.0)
			return 1.0;

		float slice = ViewZToSlice(viewZ, farZ);

#if defined(VR)
		screenUV = Stereo::ConvertToStereoUV(screenUV, eyeIndex);
#endif

		return SharedShadowMap.SampleLevel(LinearSampler, float3(screenUV, slice), 0);
	}

	// Sample shadow visibility along a view ray (start..end in camera-relative world space)
	// using `baseSampleCount` jittered taps. Returns the average shadow and reports the start
	// position's shadow value via `surfaceShadow` for back-compat with the prior VSM API.
	float GetShadow3D(float3 startPosition, float3 endPosition, float noise, uint baseSampleCount, uint eyeIndex, out float surfaceShadow)
	{
		uint sampleCount = max(1u, baseSampleCount);
		float rcpSampleCount = rcp(sampleCount);

		float shadow = 0.0;
		surfaceShadow = 1.0;

		[loop] for (uint k = 0; k < sampleCount; k++)
		{
			float t = (float(k) + noise) * rcpSampleCount;
			float3 samplePos = lerp(endPosition, startPosition, t);
			float lit = SampleShadowFroxel(samplePos, eyeIndex);
			// Last iteration's `t` is closest to 1.0, hence closest to startPosition.
			surfaceShadow = lit;
			shadow += lit;
		}

		return shadow * rcpSampleCount;
	}

	// Single trilinear lookup at a surface position.
	float GetShadow2D(float3 position, uint eyeIndex, out float detailedShadow)
	{
		float shadow = SampleShadowFroxel(position, eyeIndex);
		detailedShadow = shadow;
		return shadow;
	}
}

#endif  // __VOLUMETRIC_SHADOWS_HLSLI__
