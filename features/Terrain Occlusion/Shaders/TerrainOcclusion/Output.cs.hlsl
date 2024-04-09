#include "../Common/DeferredShared.hlsl"
#include "../Common/VR.hlsl"

struct PerPassTerraOcc
{
	uint EnableTerrainShadow;
	uint EnableTerrainAO;

	float HeightBias;

	float ShadowSofteningRadiusAngle;
	float2 ShadowFadeDistance;

	float AOMix;
	float AOPower;
	float AOFadeOutHeightRcp;

	float3 scale;
	float3 invScale;
	float3 offset;
	float2 zRange;
};

Texture2D<unorm half> TexDepth : register(t0);
StructuredBuffer<PerPassTerraOcc> perPassTerraOcc : register(t1);
Texture2D<float> TexTerraOcc : register(t2);
Texture2D<float> TexNormalisedHeight : register(t3);
Texture2D<float2> TexShadowHeight : register(t4);

RWTexture2D<unorm float> RWTexShadowMask : register(u0);
RWTexture2D<half4> RWTexGI : register(u1);

SamplerState SamplerDefault;

float2 GetTerrainOcclusionUV(float2 xy)
{
	return xy * perPassTerraOcc[0].scale.xy + perPassTerraOcc[0].offset.xy;
}

float2 GetTerrainOcclusionXY(float2 uv)
{
	return (uv - perPassTerraOcc[0].offset.xy) * perPassTerraOcc[0].invScale.xy;
}

float GetTerrainZ(float norm_z)
{
	return lerp(perPassTerraOcc[0].zRange.x, perPassTerraOcc[0].zRange.y, norm_z) + perPassTerraOcc[0].HeightBias;
}

float2 GetTerrainZ(float2 norm_z)
{
	return float2(GetTerrainZ(norm_z.x), GetTerrainZ(norm_z.y));
}

[numthreads(32, 32, 1)] void main(uint2 dtid
								  : SV_DispatchThreadID) {
	float2 uv = (dtid + .5) * RcpBufferDim;
#ifdef VR
	const uint eyeIndex = uv > .5;
#else
	const uint eyeIndex = 0;
#endif

	float3 ndc = float3(ConvertToStereoUV(uv, eyeIndex), 1);
	ndc = ndc * 2 - 1;
	ndc.y = -ndc.y;
	ndc.z = TexDepth[dtid];

	float4 worldPos = mul(InvViewMatrix[eyeIndex], mul(InvProjMatrix[eyeIndex], float4(ndc, 1)));
	worldPos.xyz /= worldPos.w;
	float viewDistance = length(worldPos);

	// if (viewDistance > 1e7)
	// 	return;

	worldPos.xyz += CamPosAdjust[0].xyz;

	float2 terraOccUV = GetTerrainOcclusionUV(worldPos.xy);

	if (any(terraOccUV < 0) && any(terraOccUV > 1))
		return;

	float terrainShadow = 1;
	float terrainAo = 1;

	if (perPassTerraOcc[0].EnableTerrainShadow && (viewDistance > perPassTerraOcc[0].ShadowFadeDistance.x)) {
		float fadeFactor = saturate((viewDistance - perPassTerraOcc[0].ShadowFadeDistance.x) / (perPassTerraOcc[0].ShadowFadeDistance.y - perPassTerraOcc[0].ShadowFadeDistance.x));
		float2 shadowHeight = GetTerrainZ(TexShadowHeight.SampleLevel(SamplerDefault, terraOccUV, 0));
		float shadowFraction = saturate((worldPos.z - shadowHeight.y) / (shadowHeight.x - shadowHeight.y));
		terrainShadow = lerp(1, shadowFraction, fadeFactor);
	}
	if (perPassTerraOcc[0].EnableTerrainAO) {
		float terrainHeight = GetTerrainZ(TexNormalisedHeight.SampleLevel(SamplerDefault, terraOccUV, 0).x);
		terrainAo = TexTerraOcc.SampleLevel(SamplerDefault, terraOccUV, 0).x;

		// power
		terrainAo = pow(terrainAo, perPassTerraOcc[0].AOPower);

		// height fadeout
		float fadeOut = saturate((worldPos.z - terrainHeight) * perPassTerraOcc[0].AOFadeOutHeightRcp);
		terrainAo = lerp(terrainAo, 1, fadeOut);

		terrainAo = lerp(1, terrainAo, perPassTerraOcc[0].AOMix);
	}

	half shadow = RWTexShadowMask[dtid];
	RWTexShadowMask[dtid] = min(shadow, terrainShadow);

	float4 gi = RWTexGI[dtid];
	gi.w *= terrainAo;
	RWTexGI[dtid] = gi;
}