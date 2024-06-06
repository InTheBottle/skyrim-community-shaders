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

float phaseHenyeyGreenstein(float cosTheta, float g)
{
	static const float scale = .25 / 3.1415926535;
	const float g2 = g * g;

	float num = (1.0 - g2);
	float denom = pow(abs(1.0 + g2 - 2.0 * g * cosTheta), 1.5);

	return scale * num / denom;
}

float GetVL(float3 startPosWS, float3 endPosWS, float2 screenPosition)
{
	const static uint nSteps = 16;
	const static float step = 1.0 / float(nSteps);

	float3 worldDir = endPosWS - startPosWS;

	float noise = InterleavedGradientNoise(screenPosition);

	PerGeometry sD = perShadow[0];
	sD.EndSplitDistances.x = GetScreenDepth(sD.EndSplitDistances.x);
	sD.EndSplitDistances.y = GetScreenDepth(sD.EndSplitDistances.y);
	sD.EndSplitDistances.z = GetScreenDepth(sD.EndSplitDistances.z);
	sD.EndSplitDistances.w = GetScreenDepth(sD.EndSplitDistances.w);

	float vl = 0;

	for (uint i = 0; i < nSteps; ++i) {
		float t = saturate((i + noise) * step);

		float shadow = 0;
		{
			float3 samplePositionWS = startPosWS + worldDir * t;
			float shadowMapDepth = length(samplePositionWS.xyz);

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

			half3 samplePositionLS = mul(transpose(lightProjectionMatrix), half4(samplePositionWS.xyz, 1)).xyz;
			float deltaZ = samplePositionLS.z - shadowMapThreshold;

			float4 depths = TexShadowMapSampler.GatherRed(LinearSampler, half3(samplePositionLS.xy, cascadeIndex), 0);

			shadow = dot(depths > deltaZ, 0.25);
		}
		vl += shadow;
	}
	return vl * step;
}
#endif
