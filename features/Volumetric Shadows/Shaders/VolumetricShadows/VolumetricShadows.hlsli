#ifndef __VOLUMETRIC_SHADOWS_HLSLI__
#define __VOLUMETRIC_SHADOWS_HLSLI__

namespace VolumetricShadows
{
	Texture2D<float4> SharedShadowMap : register(t18);

	// EVSM exponents — must match values set in C++ (VolumetricShadows.h)
	static const float EVSM_EXPONENT_POS = 40.0;
	static const float EVSM_EXPONENT_NEG = 5.0;
	static const float EVSM_VARIANCE_BIAS = 0.001;
	static const float EVSM_LIGHT_BLEED_REDUCTION = 0.3;

	// Convert orthographic shadow projection depth to globally-normalized [0,1].
	// positionLS.z from mul(ShadowProj, worldPos) is orthographic [0,1] within the cascade.
	// We remap through world space to the same global range used during moment generation.
	float NormalizeDepth(float depth, float cascadeNear, float cascadeFar, float globalNear, float globalFar)
	{
		float worldZ = cascadeNear + depth * (cascadeFar - cascadeNear);
		return (worldZ - globalNear) / (globalFar - globalNear);
	}

	// Chebyshev upper bound: P(x >= t) <= variance / (variance + (t - mean)^2)
	// Returns visibility [0,1] where 1 = fully lit
	float ChebyshevUpperBound(float mean, float meanSq, float testValue)
	{
		float variance = max(meanSq - mean * mean, EVSM_VARIANCE_BIAS);

		float d = testValue - mean;
		float pMax = variance / (variance + d * d);

		// Reduce light bleeding by remapping [bleedReduction..1] -> [0..1]
		pMax = saturate((pMax - EVSM_LIGHT_BLEED_REDUCTION) / (1.0 - EVSM_LIGHT_BLEED_REDUCTION));

		// If the test value is behind the mean, it's fully lit
		return (testValue <= mean) ? 1.0 : pMax;
	}

	// Compute EVSM shadow from stored moments
	// moments = (E[e^cz], E[e^2cz], E[e^-cz], E[e^-2cz])
	float ComputeEVSM(float4 moments, float depth, float cascadeNear, float cascadeFar, float globalNear, float globalFar)
	{
		float d = NormalizeDepth(depth, cascadeNear, cascadeFar, globalNear, globalFar);
		float posWarp = exp(EVSM_EXPONENT_POS * d);
		float negWarp = exp(-EVSM_EXPONENT_NEG * d);

		// Positive exponent test (standard front-face shadow)
		float posShadow = ChebyshevUpperBound(moments.x, moments.y, posWarp);

		// Negative exponent test (back-face light bleed suppression)
		float negShadow = ChebyshevUpperBound(moments.z, moments.w, negWarp);

		return min(posShadow, negShadow);
	}

	// Sample a single cascade for EVSM shadow (3D ray march)
	float SampleEVSMCascade3D(
		uint cascadeIndex,
		float noise,
		uint sampleCount,
		float rcpSampleCount,
		float3 startPositionLS,
		float3 endPositionLS,
		float cascadeNear,
		float cascadeFar,
		float globalNear,
		float globalFar,
		out float firstSample)
	{
		float shadow = 0.0;
		firstSample = 1.0;

		[loop] for (uint k = 0; k < sampleCount; k++)
		{
			float t = (float(k) + noise) * rcpSampleCount;
			float3 samplePosLS = lerp(endPositionLS, startPositionLS, t);

			float4 moments = SharedShadowMap.SampleLevel(LinearSampler, samplePosLS.xy, 1u - cascadeIndex);
			float lit = ComputeEVSM(moments, samplePosLS.z, cascadeNear, cascadeFar, globalNear, globalFar);

			// Last to set firstSample is start position
			firstSample = lit;

			shadow += lit;
		}

		return shadow * rcpSampleCount;
	}

	float GetVSMShadow3D(float3 startPosition, float3 endPosition, float noise, uint baseSampleCount, out float surfaceShadow)
	{
		DirectionalShadowLightData directionalShadowLightData = DirectionalShadowLights[0];

		// View-space z — matches the linear cascade split distances from BSShadowDirectionalLight.
		float3 midPosition = (startPosition + endPosition) * 0.5;
		float shadowMapDepth = SharedData::GetScreenDepth(FrameBuffer::GetShadowDepth(midPosition));

		// Cascade projections are world-space; positions come in camera-relative.
		startPosition += FrameBuffer::CameraPosAdjust.xyz;
		endPosition += FrameBuffer::CameraPosAdjust.xyz;

		// Early out beyond cascade range
		if (shadowMapDepth >= directionalShadowLightData.EndSplitDistances.y) {
			surfaceShadow = 1.0;
			return 1.0;
		}

		// Reduce over distance
		float fade = saturate(shadowMapDepth / directionalShadowLightData.EndSplitDistances.y);

		uint sampleCount = max(1, ceil(float(baseSampleCount) * (1.0 - fade)));
		float rcpSampleCount = rcp(sampleCount);

		// Compute cascade blend factor with smoothstep
		float cascadeSelect = saturate((shadowMapDepth - directionalShadowLightData.StartSplitDistances.y) / (directionalShadowLightData.EndSplitDistances.x - directionalShadowLightData.StartSplitDistances.y));

		// Determine which cascade(s) to sample
		uint primaryCascade = uint(cascadeSelect);
		bool needsBlending = (cascadeSelect > 0.0) && (cascadeSelect < 1.0);

		float4 depthParams = directionalShadowLightData.CascadeDepthParams;
		float4 globalParams = directionalShadowLightData.GlobalDepthParams;
		float globalNear = globalParams.x;
		float globalFar = globalParams.y;

		// Transform ray to light space for primary cascade
		float4x4 shadowProj = directionalShadowLightData.ShadowProj[primaryCascade];
		float3 startLS = mul(shadowProj, float4(startPosition, 1)).xyz;
		float3 endLS = mul(shadowProj, float4(endPosition, 1)).xyz;
		startLS.xy = saturate(startLS.xy);
		endLS.xy = saturate(endLS.xy);

		float primaryNear = primaryCascade == 0 ? depthParams.x : depthParams.z;
		float primaryFar = primaryCascade == 0 ? depthParams.y : depthParams.w;

		// Sample primary cascade
		float primaryFirstSample;
		float shadow = SampleEVSMCascade3D(primaryCascade, noise, sampleCount, rcpSampleCount, startLS, endLS, primaryNear, primaryFar, globalNear, globalFar, primaryFirstSample);
		surfaceShadow = primaryFirstSample;

		// Blend with secondary cascade if needed
		[branch] if (needsBlending)
		{
			uint secondaryCascade = 1 - primaryCascade;

			shadowProj = directionalShadowLightData.ShadowProj[secondaryCascade];
			startLS = mul(shadowProj, float4(startPosition, 1)).xyz;
			endLS = mul(shadowProj, float4(endPosition, 1)).xyz;
			startLS.xy = saturate(startLS.xy);
			endLS.xy = saturate(endLS.xy);

			float secondaryNear = secondaryCascade == 0 ? depthParams.x : depthParams.z;
			float secondaryFar = secondaryCascade == 0 ? depthParams.y : depthParams.w;

			float secondaryFirstSample;
			float shadowBlend = SampleEVSMCascade3D(secondaryCascade, noise, sampleCount, rcpSampleCount, startLS, endLS, secondaryNear, secondaryFar, globalNear, globalFar, secondaryFirstSample);
			shadow = lerp(shadow, shadowBlend, cascadeSelect);
			surfaceShadow = lerp(surfaceShadow, secondaryFirstSample, cascadeSelect);
		}

		// Apply distance fade
		float fadeFactor = 1.0 - pow(fade * fade, 8);
		surfaceShadow = lerp(1.0, surfaceShadow, fadeFactor);
		return lerp(1.0, shadow, fadeFactor);
	}

	// Sample a single cascade for EVSM shadow (2D point sample)
	float SampleEVSMCascade2D(uint cascadeIndex, float3 positionLS, float cascadeNear, float cascadeFar, float globalNear, float globalFar)
	{
		float4 moments = SharedShadowMap.SampleLevel(LinearSampler, positionLS.xy, 1u - cascadeIndex);
		return ComputeEVSM(moments, positionLS.z, cascadeNear, cascadeFar, globalNear, globalFar);
	}

	float GetVSMShadow2D(float3 position, out float detailedShadow)
	{
		DirectionalShadowLightData directionalShadowLightData = DirectionalShadowLights[0];

		float shadowMapDepth = SharedData::GetScreenDepth(FrameBuffer::GetShadowDepth(position));

		// Early out beyond cascade range
		if (shadowMapDepth >= directionalShadowLightData.EndSplitDistances.y) {
			detailedShadow = 1.0;
			return 1.0;
		}

		// Reduce over distance
		float fade = saturate(shadowMapDepth / directionalShadowLightData.EndSplitDistances.y);

		// Cascade projections are world-space; position comes in camera-relative.
		float3 positionWS = position + FrameBuffer::CameraPosAdjust.xyz;

		// Compute cascade blend factor with smoothstep
		float cascadeSelect = saturate((shadowMapDepth - directionalShadowLightData.StartSplitDistances.y) / (directionalShadowLightData.EndSplitDistances.x - directionalShadowLightData.StartSplitDistances.y));

		// Determine which cascade(s) to sample
		uint primaryCascade = uint(cascadeSelect);
		bool needsBlending = (cascadeSelect > 0.0) && (cascadeSelect < 1.0);

		float4 depthParams = directionalShadowLightData.CascadeDepthParams;
		float4 globalParams = directionalShadowLightData.GlobalDepthParams;
		float globalNear = globalParams.x;
		float globalFar = globalParams.y;

		// Transform position to light space for primary cascade
		float3 positionLS = mul(directionalShadowLightData.ShadowProj[primaryCascade], float4(positionWS, 1)).xyz;
		positionLS.xy = saturate(positionLS.xy);

		float primaryNear = primaryCascade == 0 ? depthParams.x : depthParams.z;
		float primaryFar = primaryCascade == 0 ? depthParams.y : depthParams.w;

		// Sample primary cascade
		float shadow = SampleEVSMCascade2D(primaryCascade, positionLS, primaryNear, primaryFar, globalNear, globalFar);

		// Blend with secondary cascade if needed
		[branch] if (needsBlending)
		{
			uint secondaryCascade = 1 - primaryCascade;

			positionLS = mul(directionalShadowLightData.ShadowProj[secondaryCascade], float4(positionWS, 1)).xyz;
			positionLS.xy = saturate(positionLS.xy);

			float secondaryNear = secondaryCascade == 0 ? depthParams.x : depthParams.z;
			float secondaryFar = secondaryCascade == 0 ? depthParams.y : depthParams.w;

			float shadowBlend = SampleEVSMCascade2D(secondaryCascade, positionLS, secondaryNear, secondaryFar, globalNear, globalFar);
			shadow = lerp(shadow, shadowBlend, cascadeSelect);
		}

		// Apply distance fade
		float fadeFactor = 1.0 - pow(fade * fade, 8);
		detailedShadow = lerp(1.0, shadow, fadeFactor);
		return detailedShadow;
	}
}

#endif  // __VOLUMETRIC_SHADOWS_HLSLI__
