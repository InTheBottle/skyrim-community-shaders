struct PerGeometry
{
	float4 VPOSOffset;					            
	float4 ShadowSampleParam;   // fPoissonRadiusScale / iShadowMapResolution in z and w
	float4 EndSplitDistances;   // cascade end distances int xyz, cascade count int z
	float4 StartSplitDistances; // cascade start ditances int xyz, 4 int z
	float4 FocusShadowFadeParam;
	float4 DebugColor;					      
	float4 PropertyColor;					           
	float4 AlphaTestRef;					          
	float4 ShadowLightParam;   	// Falloff in x, ShadowDistance squared in z
	float4x3 FocusShadowMapProj[4];
	float4x3 ShadowMapProj[4];
	float4x4 CameraViewProjInverse;
};

Texture2DArray<float4> TexShadowMapSampler : register(t80);
StructuredBuffer<PerGeometry> perShadow : register(t81);
Texture2D<float> SkylightingTexture : register(t82);

#if defined(WATER) || defined(EFFECT)
// Derived from the interleaved gradient function from Jimenez 2014 http://goo.gl/eomGso
float InterleavedGradientNoise2(float2 uv)
{
	float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(uv, magic.xy)));
}

float3 GetVL(float3 worldPosition, float3 viewPosition, float rawDepth, float depth, float2 screenPosition)
{ 
    float step = 1.0 / 16.0;
    float pos = 0.0;

	float noise = InterleavedGradientNoise2(screenPosition);
	viewPosition.z = lerp(viewPosition.z, depth, noise / 16.0);

	PerGeometry sD = perShadow[0];

    float3 volumetric = 0.0;
    for (int i = 0; i < 16; ++i)
    {
		float3 positionWS = mul(CameraViewInverse[0], half4(viewPosition.xy, lerp(viewPosition.z, depth, i / 16.0), 1));
     
		half cascadeIndex = 0;
		half4x3 lightProjectionMatrix = sD.ShadowMapProj[0];
		half shadowMapThreshold = sD.AlphaTestRef.y;

		[flatten] if (2.5 < sD.EndSplitDistances.w && sD.EndSplitDistances.y < rawDepth)
		{
			lightProjectionMatrix = sD.ShadowMapProj[2];
			shadowMapThreshold = sD.AlphaTestRef.z;
			cascadeIndex = 2;
		}
		else if (sD.EndSplitDistances.x < rawDepth)
		{
			lightProjectionMatrix = sD.ShadowMapProj[1];
			shadowMapThreshold = sD.AlphaTestRef.z;
			cascadeIndex = 1;
		}
		
		half3 positionLS = mul(transpose(lightProjectionMatrix), half4(positionWS.xyz, 1)).xyz;

		float depth = TexShadowMapSampler.SampleLevel(LinearSampler, half3(positionLS.xy, cascadeIndex), 0);
		
		if (depth > positionLS.z - shadowMapThreshold)
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