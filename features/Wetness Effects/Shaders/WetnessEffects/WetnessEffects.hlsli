#include "WetnessEffects/optimized-ggx.hlsl"

struct PerPassWetnessEffects
{
	float Time;
	uint Raining;
	float Wetness;
	float PuddleWetness;
	row_major float3x4 DirectionalAmbientWS;
	uint EnableWetnessEffects;
	float MaxRainWetness;
	float MaxPuddleWetness;
	float MaxShoreWetness;
	uint ShoreRange;
	float PuddleRadius;
	float PuddleMaxAngle;
	float PuddleMinWetness;
	float MinRainWetness;
	float SkinWetness;
	float WeatherTransitionSpeed;
	float pad[3];
};

StructuredBuffer<PerPassWetnessEffects> perPassWetnessEffects : register(t22);

#define LinearSampler SampShadowMaskSampler

// https://www.unrealengine.com/en-US/blog/physically-based-shading-on-mobile
float2 EnvBRDFApprox(float3 F0, float Roughness, float NoV)
{
	const float4 c0 = { -1, -0.0275, -0.572, 0.022 };
	const float4 c1 = { 1, 0.0425, 1.04, -0.04 };
	float4 r = Roughness * c0 + c1;
	float a004 = min(r.x * r.x, exp2(-9.28 * NoV)) * r.x + r.y;
	float2 AB = float2(-1.04, 1.04) * a004 + r.zw;
	return AB;
}

uint3 pcg3d(uint3 v)
{
	v = v * 1664525u + 1013904223u;

	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;

	v ^= v >> 16u;

	v.x += v.y * v.z;
	v.y += v.z * v.x;
	v.z += v.x * v.y;

	return v;
}

uint iqint3(uint2 x)
{
	uint2 q = 1103515245U * ((x >> 1U) ^ (x.yx));
	uint n = 1103515245U * ((q.x) ^ (q.y >> 3U));

	return n;
}

float RainFade(float normalised_t)
{
	const float rain_stay = .5;

	if (normalised_t < rain_stay)
		return 1.0;

	float val = lerp(1.0, 0.0, (normalised_t - rain_stay) / (1.0 - rain_stay));
	return val * val;
}

float GetRainDrops(float3 pos, float t)
{
	const float gridsize = 3;
	const float radius_min = 0.3;
	const float radius_max = 0.5;
	const float lifetime = 2.0;
	const float density = 0.1;

	float2 grid_uv = pos / gridsize;
	int2 grid = grid_uv;
	grid_uv -= grid;

	float wetness = 0;

	for (int i = -1; i <= 1; i++)
		for (int j = -1; j <= 1; j++) {
			int2 grid_curr = grid + int2(i, j);
			float t_offset = float(iqint3(grid_curr)) / 4294967295.0;

			float residual = t / lifetime + t_offset + pos.z * 0.001;
			uint timestep = residual;
			residual = residual - timestep;

			uint3 hash = pcg3d(uint3(grid_curr, timestep));
			float3 float_hash = float3(hash) / 4294967295.0;
			if (float_hash.z < density) {
				float2 to_centre = int2(i, j) + float_hash.xy - grid_uv;
				float drop_radius = lerp(radius_min, radius_max, saturate(float(iqint3(hash.yz)) / 4294967295.0));
				if (dot(to_centre, to_centre) < drop_radius * drop_radius)
					wetness = max(wetness, RainFade(residual));
			}
		}

	return wetness;
}

// https://github.com/BelmuTM/Noble/blob/master/LICENSE.txt

float hash11(float p)
{
	return frac(sin(p) * 1e4);
}

float noise(float3 pos)
{
	const float3 step = float3(110.0, 241.0, 171.0);
	float3 i = floor(pos);
	float3 f = frac(pos);
	float n = dot(i, step);

	float3 u = f * f * (3.0 - 2.0 * f);
	return lerp(lerp(lerp(hash11(n + dot(step, float3(0.0, 0.0, 0.0))), hash11(n + dot(step, float3(1.0, 0.0, 0.0))), u.x),
					lerp(hash11(n + dot(step, float3(0.0, 1.0, 0.0))), hash11(n + dot(step, float3(1.0, 1.0, 0.0))), u.x), u.y),
		lerp(lerp(hash11(n + dot(step, float3(0.0, 0.0, 1.0))), hash11(n + dot(step, float3(1.0, 0.0, 1.0))), u.x),
			lerp(hash11(n + dot(step, float3(0.0, 1.0, 1.0))), hash11(n + dot(step, float3(1.0, 1.0, 1.0))), u.x), u.y),
		u.z);
}

float3 GetWetnessAmbientSpecular(float2 uv, float3 N, float3 V, float roughness)
{
#if defined(DYNAMIC_CUBEMAPS)
	float3 R = reflect(-V, N);
	float NoV = saturate(dot(N, V));

	float level = roughness * 9.0;

	float3 specularIrradiance = specularTexture.SampleLevel(SampColorSampler, R, level);

#	if defined(DYNAMIC_CUBEMAPS) && !defined(VR)
	// float4 ssrBlurred = ssrTexture.SampleLevel(SampColorSampler, uv, 0);
	// float4 ssrRaw = ssrRawTexture.SampleLevel(SampColorSampler, uv, 0);
	// float4 ssrTexture = lerp(ssrRaw, ssrBlurred, sqrt(roughness));
	// specularIrradiance = sRGB2Lin(lerp(specularIrradiance, ssrTexture.rgb, ssrTexture.a));
	specularIrradiance = sRGB2Lin(specularIrradiance);
#	else
	specularIrradiance = sRGB2Lin(specularIrradiance);
#	endif

#else
	float3 R = reflect(-V, N);
	float NoV = saturate(dot(N, V));

	float3 specularIrradiance = sRGB2Lin(mul(perPassWetnessEffects[0].DirectionalAmbientWS, float4(R, 1.0))) * noise(R * lerp(10.0, 1.0, roughness * roughness));
#endif

	// Split-sum approximation factors for Cook-Torrance specular BRDF.
#if defined(DYNAMIC_CUBEMAPS)
	float2 specularBRDF = specularBRDF_LUT.Sample(LinearSampler, float2(NoV, roughness));
#else
	float2 specularBRDF = EnvBRDFApprox(0.02, roughness, NoV);
#endif

	// Horizon specular occlusion
	float horizon = min(1.0 + dot(R, N), 1.0);
	specularIrradiance *= horizon * horizon;

	// Roughness dependent fresnel
	// https://www.jcgt.org/published/0008/01/03/paper.pdf
	float3 Fr = max(1.0.xxx - roughness.xxx, 0.02) - 0.02;
	float3 S = 0.02 + Fr * pow(1.0 - NoV, 5.0);

	return specularIrradiance * (S * specularBRDF.x + specularBRDF.y);
}

float3 GetWetnessSpecular(float3 N, float3 L, float3 V, float3 lightColor, float roughness)
{
	lightColor *= 0.01;
	return LightingFuncGGX_OPT3(N, V, L, roughness, 1.0 - roughness) * lightColor;
}
