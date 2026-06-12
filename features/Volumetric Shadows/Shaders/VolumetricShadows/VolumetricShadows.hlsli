#ifndef __VOLUMETRIC_SHADOWS_HLSLI__
#define __VOLUMETRIC_SHADOWS_HLSLI__

namespace VolumetricShadows
{
	Texture2D<float4> SharedShadowMap : register(t18);

	static const float MSM_MOMENT_BIAS = 0.003;

	float LinearizeDepth(float depth, float cascadeNear, float cascadeFar)
	{
		float linZ = cascadeNear * cascadeFar / (cascadeFar - depth * (cascadeFar - cascadeNear));
		return (linZ - cascadeNear) / (cascadeFar - cascadeNear);
	}

	// Inverse RGBA16 quantization: recover power moments from optimized storage
	// Reference: Peters, "Moment Shadow Mapping" (I3D 2015)
	float4 ConvertOptimizedMoments(float4 optimizedMoments)
	{
		optimizedMoments[0] -= 0.035955884801;
		return mul(optimizedMoments, float4x4(
			0.2227744146,  0.1549679261,  0.1451988946,  0.163127443,
			0.0771972861,  0.1394629426,  0.2120202157,  0.2591432266,
			0.7926986636,  0.7963415838,  0.7258694464,  0.6539092497,
			0.0319417555, -0.1722823173, -0.2758014811, -0.3376131734));
	}

	// Hamburger 4-moment shadow reconstruction
	// Reference: Peters, "Moment Shadow Mapping" (I3D 2015)
	float ComputeMSM(float4 optimizedMoments, float depth)
	{
		float4 b = ConvertOptimizedMoments(optimizedMoments);

		// Bias moments to reduce light bleeding
		b = lerp(b, 0.5, MSM_MOMENT_BIAS);

		float3 z;
		z[0] = depth;

		// Cholesky factorization of the Hankel matrix
		float L32D22 = mad(-b[0], b[1], b[2]);
		float D22 = mad(-b[0], b[0], b[1]);
		float squaredDepthVariance = mad(-b[1], b[1], b[3]);
		float D33D22 = dot(float2(squaredDepthVariance, -L32D22), float2(D22, L32D22));
		float InvD22 = 1.0 / D22;
		float L32 = L32D22 * InvD22;

		// Solve for the quadratic polynomial whose roots give the 2-point distribution
		float3 c = float3(1.0, z[0], z[0] * z[0]);
		c[1] -= b.x;
		c[2] -= b.y + L32 * c[1];
		c[1] *= InvD22;
		c[2] *= D22 / D33D22;
		c[1] -= L32 * c[2];
		c[0] -= dot(c.yz, b.xy);

		float p = c[1] / c[2];
		float q = c[0] / c[2];
		float D = (p * p * 0.25) - q;
		float r = sqrt(max(D, 0.0));
		z[1] = -p * 0.5 - r;
		z[2] = -p * 0.5 + r;

		// Compute shadow intensity from the 2-point distribution
		float4 switchVal = (z[2] < z[0]) ? float4(z[1], z[0], 1.0, 1.0) :
		                  ((z[1] < z[0]) ? float4(z[0], z[1], 0.0, 1.0) :
		                  float4(0.0, 0.0, 0.0, 0.0));
		float quotient = (switchVal[0] * z[2] - b[0] * (switchVal[0] + z[2]) + b[1])
		                 / ((z[2] - switchVal[1]) * (z[0] - z[1]));
		float shadowIntensity = switchVal[2] + switchVal[3] * quotient;
		return 1.0 - saturate(shadowIntensity);
	}

	// Sample a single cascade for MSM shadow
	float SampleVSMCascade3D(
		uint cascadeIndex,
		float noise,
		uint sampleCount,
		float rcpSampleCount,
		float3 startPositionLS,
		float3 endPositionLS,
		float cascadeNear,
		float cascadeFar,
		out float firstSample)
	{
		float shadow = 0.0;
		firstSample = 1.0;

		[loop] for (uint k = 0; k < sampleCount; k++)
		{
			float t = (float(k) + noise) * rcpSampleCount;
			float3 samplePosLS = lerp(endPositionLS, startPositionLS, t);

			float4 moments = SharedShadowMap.SampleLevel(LinearSampler, samplePosLS.xy, 1u - cascadeIndex);
			float depth = LinearizeDepth(samplePosLS.z, cascadeNear, cascadeFar);
			float lit = ComputeMSM(moments, depth);

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

		// Compute cascade blend factor
		float cascadeSelect = saturate((shadowMapDepth - directionalShadowLightData.StartSplitDistances.y) / (directionalShadowLightData.EndSplitDistances.x - directionalShadowLightData.StartSplitDistances.y));

		// Determine which cascade(s) to sample
		uint primaryCascade = uint(cascadeSelect);
		bool needsBlending = (cascadeSelect > 0.0) && (cascadeSelect < 1.0);

		float4 depthParams = directionalShadowLightData.CascadeDepthParams;

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
		float shadow = SampleVSMCascade3D(primaryCascade, noise, sampleCount, rcpSampleCount, startLS, endLS, primaryNear, primaryFar, primaryFirstSample);
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
			float shadowBlend = SampleVSMCascade3D(secondaryCascade, noise, sampleCount, rcpSampleCount, startLS, endLS, secondaryNear, secondaryFar, secondaryFirstSample);
			float blendFactor = smoothstep(0, 1, cascadeSelect);
			shadow = lerp(shadow, shadowBlend, blendFactor);
			surfaceShadow = lerp(surfaceShadow, secondaryFirstSample, blendFactor);
		}

		// Apply distance fade
		float fadeFactor = 1.0 - pow(fade * fade, 8);
		surfaceShadow = lerp(1.0, surfaceShadow, fadeFactor);
		return lerp(1.0, shadow, fadeFactor);
	}

	// Sample a single cascade for MSM shadow (2D point sample)
	float SampleVSMCascade2D(uint cascadeIndex, float3 positionLS, float cascadeNear, float cascadeFar)
	{
		float4 moments = SharedShadowMap.SampleLevel(LinearSampler, positionLS.xy, 1u - cascadeIndex);
		float depth = LinearizeDepth(positionLS.z, cascadeNear, cascadeFar);
		return ComputeMSM(moments, depth);
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

		// Compute cascade blend factor
		float cascadeSelect = saturate((shadowMapDepth - directionalShadowLightData.StartSplitDistances.y) / (directionalShadowLightData.EndSplitDistances.x - directionalShadowLightData.StartSplitDistances.y));

		// Determine which cascade(s) to sample
		uint primaryCascade = uint(cascadeSelect);
		bool needsBlending = (cascadeSelect > 0.0) && (cascadeSelect < 1.0);

		float4 depthParams = directionalShadowLightData.CascadeDepthParams;

		// Transform position to light space for primary cascade
		float3 positionLS = mul(directionalShadowLightData.ShadowProj[primaryCascade], float4(positionWS, 1)).xyz;
		positionLS.xy = saturate(positionLS.xy);

		float primaryNear = primaryCascade == 0 ? depthParams.x : depthParams.z;
		float primaryFar = primaryCascade == 0 ? depthParams.y : depthParams.w;

		// Sample primary cascade
		float shadow = SampleVSMCascade2D(primaryCascade, positionLS, primaryNear, primaryFar);

		// Blend with secondary cascade if needed
		[branch] if (needsBlending)
		{
			uint secondaryCascade = 1 - primaryCascade;

			positionLS = mul(directionalShadowLightData.ShadowProj[secondaryCascade], float4(positionWS, 1)).xyz;
			positionLS.xy = saturate(positionLS.xy);

			float secondaryNear = secondaryCascade == 0 ? depthParams.x : depthParams.z;
			float secondaryFar = secondaryCascade == 0 ? depthParams.y : depthParams.w;

			float shadowBlend = SampleVSMCascade2D(secondaryCascade, positionLS, secondaryNear, secondaryFar);
			shadow = lerp(shadow, shadowBlend, smoothstep(0, 1, cascadeSelect));
		}

		// Apply distance fade
		float fadeFactor = 1.0 - pow(fade * fade, 8);
		detailedShadow = lerp(1.0, shadow, fadeFactor);
		return detailedShadow;
	}
}

#endif  // __VOLUMETRIC_SHADOWS_HLSLI__
