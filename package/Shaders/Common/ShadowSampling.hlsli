#include "Common/Color.hlsl"

struct PerGeometry
{
	float4 VPOSOffset;
	float4 ShadowSampleParam;    // fPoissonRadiusScale / iShadowMapResolution in z and w
	float4 EndSplitDistances;    // cascade end distances int xyz, cascade count int z
	float4 StartSplitDistances;  // cascade start ditances int xyz, 4 int z
	float4 FocusShadowFadeParam;
	float4 DebugColor;
	float4 PropertyColor;
	float4 AlphaTestRef;
	float4 ShadowLightParam;  // Falloff in x, ShadowDistance squared in z
	float4x3 FocusShadowMapProj[4];
	float4x3 ShadowMapProj[4];
	float4x4 CameraViewProjInverse;
};

Texture2DArray<float4> TexShadowMapSampler : register(t25);
StructuredBuffer<PerGeometry> perShadow : register(t26);

#if defined(WATER) || defined(EFFECT)

cbuffer PerWaterType : register(b7)
{
	float3 ShallowColorNoWeather;
	uint pad0water;
	float3 DeepColorNoWeather;
	uint pad1water;
};

float3 GetShadow(float3 positionWS)
{
	PerGeometry sD = perShadow[0];
	sD.EndSplitDistances.x = GetScreenDepth(sD.EndSplitDistances.x);
	sD.EndSplitDistances.y = GetScreenDepth(sD.EndSplitDistances.y);
	sD.EndSplitDistances.z = GetScreenDepth(sD.EndSplitDistances.z);
	sD.EndSplitDistances.w = GetScreenDepth(sD.EndSplitDistances.w);

	float shadowMapDepth = length(positionWS.xyz);

	half cascadeIndex = 0;
	half4x3 lightProjectionMatrix = sD.ShadowMapProj[0];
	half shadowMapThreshold = sD.AlphaTestRef.y;

	[flatten] if (2.5 < sD.EndSplitDistances.w && sD.EndSplitDistances.y < shadowMapDepth)
	{
		lightProjectionMatrix = sD.ShadowMapProj[2];
		shadowMapThreshold = sD.AlphaTestRef.z;
		cascadeIndex = 2;
	}
	else if (sD.EndSplitDistances.x < shadowMapDepth)
	{
		lightProjectionMatrix = sD.ShadowMapProj[1];
		shadowMapThreshold = sD.AlphaTestRef.z;
		cascadeIndex = 1;
	}

	half3 positionLS = mul(transpose(lightProjectionMatrix), half4(positionWS.xyz, 1)).xyz;
	float deltaZ = positionLS.z - shadowMapThreshold;

	float4 depths = TexShadowMapSampler.GatherRed(LinearSampler, half3(positionLS.xy, cascadeIndex), 0);

	float shadow = dot(depths > deltaZ, 0.25);

	return shadow;
}

float GetVL(float3 startPosWS, float3 endPosWS, float2 screenPosition)
{
	const static uint nSteps = 16;
	const static float step = 1.0 / float(nSteps);

	float3 worldDir = endPosWS - startPosWS;

	float noise = InterleavedGradientNoise(screenPosition) * 2.0 * M_PI;

	startPosWS += worldDir * step * noise;

	float3 worldDirNorm = normalize(worldDir);

	float distRatio = abs(SunDir.z / worldDirNorm.z);
	float2 causticsUVShift = (worldDirNorm + SunDir / distRatio).xy * length(worldDir);

	noise = noise * 2.0 * M_PI;
	half2x2 rotationMatrix = half2x2(cos(noise), sin(noise), -sin(noise), cos(noise));

	PerGeometry sD = perShadow[0];
	sD.EndSplitDistances.x = GetScreenDepth(sD.EndSplitDistances.x);
	sD.EndSplitDistances.y = GetScreenDepth(sD.EndSplitDistances.y);
	sD.EndSplitDistances.z = GetScreenDepth(sD.EndSplitDistances.z);
	sD.EndSplitDistances.w = GetScreenDepth(sD.EndSplitDistances.w);

	float vl = 0;

	for (uint i = 0; i < nSteps; ++i) {
		float t = saturate(i * step);

		float shadow = 0;
		{
			float3 samplePositionWS = startPosWS + worldDir * t;

			float shadowMapDepth = length(samplePositionWS.xyz);

			half cascadeIndex = 0;
			half4x3 lightProjectionMatrix = sD.ShadowMapProj[0];
			half shadowMapThreshold = sD.AlphaTestRef.y;

			half shadowRange = sD.EndSplitDistances.x;

			[flatten] if (2.5 < sD.EndSplitDistances.w && sD.EndSplitDistances.y < shadowMapDepth)
			{
				lightProjectionMatrix = sD.ShadowMapProj[2];
				shadowMapThreshold = sD.AlphaTestRef.z;
				cascadeIndex = 2;
				shadowRange = sD.EndSplitDistances.z - sD.EndSplitDistances.y;
			}
			else if (sD.EndSplitDistances.x < shadowMapDepth)
			{
				lightProjectionMatrix = sD.ShadowMapProj[1];
				shadowMapThreshold = sD.AlphaTestRef.z;
				cascadeIndex = 1;
				shadowRange = sD.EndSplitDistances.y - sD.EndSplitDistances.x;
			}

			half3 samplePositionLS = mul(transpose(lightProjectionMatrix), half4(samplePositionWS.xyz, 1)).xyz;

			float2 sampleNoise = frac(noise + i * 0.38196601125);
			float r = sqrt(sampleNoise.x);
			float theta = 2 * M_PI * sampleNoise.y;
			float2 samplePositionLSOffset;
			sincos(theta, samplePositionLSOffset.y, samplePositionLSOffset.x);
			samplePositionLS.xy += 8.0 * samplePositionLSOffset * r / shadowRange;

			float deltaZ = samplePositionLS.z - shadowMapThreshold;

			float4 depths = TexShadowMapSampler.GatherRed(LinearSampler, half3(samplePositionLS.xy, cascadeIndex), 0);

			shadow = dot(depths > deltaZ, 0.25);

#	if defined(WATER_CAUSTICS)
			if (perPassWaterCaustics[0].EnableWaterCaustics) {
				float2 causticsUV = frac((startPosWS.xy + PosAdjust[0].xy + causticsUVShift * t) * 5e-4);
				shadow *= ComputeWaterCaustics(causticsUV);
			}
#	endif
		}
		vl += shadow;
	}
	return vl * step;
}
#endif
