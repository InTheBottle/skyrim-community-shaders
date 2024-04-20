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

Texture2DArray<float4> TexShadowMapSampler : register(t80);
StructuredBuffer<PerGeometry> perShadow : register(t81);
Texture2D<float> SkylightingTexture : register(t82);

#if defined(WATER) || defined(EFFECT)

float3 GetVL(float3 worldPosition, float3 viewPosition, float rawDepth, float depth, float2 screenPosition)
{
	const static uint nSteps = 16;
	const static float step = rcp(nSteps);

	uint noiseIdx = dot(screenPosition, float2(1, lightingData[0].BufferDim.x)) + lightingData[0].Timer * 1000;
	float noise = frac(0.5 + noiseIdx * 0.38196601125);

	float3 endPointWS = worldPosition * depth / viewPosition.z;
	
	PerGeometry sD = perShadow[0];
	sD.EndSplitDistances.x = GetScreenDepth(sD.EndSplitDistances.x);
	sD.EndSplitDistances.y = GetScreenDepth(sD.EndSplitDistances.y);
	sD.EndSplitDistances.z = GetScreenDepth(sD.EndSplitDistances.z);
	sD.EndSplitDistances.w = GetScreenDepth(sD.EndSplitDistances.w);
	
	float3 volumetric = 0.0;
	for (int i = 0; i < nSteps; ++i) {
		float3 samplePositionWS = lerp(worldPosition, endPointWS, (i + noise) * step);
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
		float sampleShadowDepth = TexShadowMapSampler.SampleLevel(LinearSampler, half3(samplePositionLS.xy, cascadeIndex), 0);

		if (sampleShadowDepth > samplePositionLS.z - shadowMapThreshold)
			volumetric += 1;
	}

	volumetric /= 16.0;

	return volumetric;
}

float3 GetShadow(float3 positionWS)
{
	PerGeometry sD = perShadow[0];

	half cascadeIndex = 0;
	half4x3 lightProjectionMatrix = sD.ShadowMapProj[0];
	half shadowMapThreshold = sD.AlphaTestRef.y;

	lightProjectionMatrix = sD.ShadowMapProj[1];
	shadowMapThreshold = sD.AlphaTestRef.z;
	cascadeIndex = 1;

	half3 positionLS = mul(transpose(lightProjectionMatrix), half4(positionWS.xyz, 1)).xyz;

	float depth = TexShadowMapSampler.SampleLevel(LinearSampler, half3(positionLS.xy, cascadeIndex), 0);

	if (depth > positionLS.z - shadowMapThreshold)
		return 1;

	return 0;
}
#endif