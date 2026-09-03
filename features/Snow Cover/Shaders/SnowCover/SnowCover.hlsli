#include "Common/Color.hlsli"
#include "Common/LightingCommon.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#if defined(PSHADER)

namespace SnowCover
{

	Texture2D<float4> SnowAlbedo : register(t38);
	Texture2D<float3> SnowNormal : register(t39);
	Texture2D<float4> SnowRmaos : register(t40);
	Texture2D<float4> IceAlbedo : register(t41);
	Texture2D<float3> IceNormal : register(t42);
	Texture2D<float4> IceRmaos : register(t43);
	Texture2D<float> SnowMap : register(t44);

	// https://blog.selfshadow.com/publications/blending-in-detail/
	// for when s = (0,0,1)
	float3 MyReorientNormal(float3 n1, float3 n2)
	{
		n1 += float3(0, 0, 1);
		n2 *= float3(-1, -1, 1);

		return n1 * dot(n1, n2) / n1.z - n2;
	}

	// Weather snow must stay at full strength past the terrain LOD rings or the snow front crawls with the camera.
	float GetWeatherRange(float3 p, float viewDist)
	{
		float fadeStart = SharedData::snowCoverSettings.WeatherFadeStart;
		float fadeEnd = max(fadeStart + 1, SharedData::snowCoverSettings.WeatherFadeEnd);
		float wobble = 1000 * sin(p.z * 0.001 + cos(p.x * p.y * 0.001));
		return 1 - smoothstep(fadeStart, fadeEnd + wobble, viewDist);
	}

	// Keeps DynDOLOD ultra-tree billboards from reading as white slabs; 0 amount disables it.
	float GetObjectFade(float viewDist)
	{
		float amount = SharedData::snowCoverSettings.ObjectFadeAmount;
		if (amount <= 0)
			return 1;
		float fadeStart = SharedData::snowCoverSettings.ObjectFadeStart;
		float fadeEnd = max(fadeStart + 1, SharedData::snowCoverSettings.ObjectFadeEnd);
		return 1 - smoothstep(fadeStart, fadeEnd, viewDist) * amount;
	}

	float GetFireAttenuation(float3 p)
	{
		if (!SharedData::snowCoverSettings.EnableFireMelt)
			return 1;
		float atten = 1;
		uint count = min(SharedData::snowCoverSettings.FireCount, 16);
		for (uint i = 0; i < count; ++i) {
			float4 fire = SharedData::snowCoverSettings.FireSources[i];
			float radius = fire.w;
			if (radius < 1)
				continue;
			float inner = radius * SharedData::snowCoverSettings.FireInnerScale;
			atten = min(atten, smoothstep(inner, radius, length(p - fire.xyz)));
		}
		return atten;
	}

	float GetHeightMult(float3 p)
	{
		float2 scale = SharedData::snowCoverSettings.mapScale;
		float2 offset = SharedData::snowCoverSettings.mapOffset;
		float2 uv = p.xy * scale + offset;
		float height_tresh = p.z - SharedData::snowCoverSettings.SnowHeightOffset + (SnowMap.SampleLevel(SampColorSampler, uv, 0) - 0.5) * SharedData::snowCoverSettings.mapZscale;
		return height_tresh;
	}

	float GetEnvironmentalMultiplier(float3 p)
	{
		return (GetHeightMult(p) + SharedData::snowCoverSettings.SeasonalAltitude) / SharedData::snowCoverSettings.BlendSmoothness;
	}

	void ApplyFoliageColor(inout float3 color, float env_mult)
	{
		float gmult = saturate(env_mult - SharedData::snowCoverSettings.FoliageHeightOffset / 5000);
		float3 hsv = Color::RGBtoHSV(color);
		if (hsv.x > 0.55)
			hsv.x = frac(lerp(hsv.x, 1.1, gmult) * 2);
		else
			hsv.x = lerp(hsv.x, 0.1, gmult);
		hsv.y *= lerp(1, 0.25, 4.0 * gmult * (1.0 - gmult));
		color = Color::HSVtoRGB(hsv);
	}

	void ApplySnowFoliage(inout float3 color, float3 worldNormal, float3 p, float skylight, float viewDist, float distMult)
	{
		float env_mult = GetEnvironmentalMultiplier(p);
		float weatherMult = GetWeatherRange(p, viewDist) * SharedData::snowCoverSettings.TimeSnowing * max(500, SharedData::snowCoverSettings.SnowingDensity) / 500;
		float mult = SharedData::snowCoverSettings.MainTint.a * saturate(env_mult);
		mult = distMult * skylight * saturate(mult + weatherMult) * smoothstep(SharedData::snowCoverSettings.minAngle, SharedData::snowCoverSettings.maxAngle, worldNormal.z);
#	if defined(GRASS)
		if (SharedData::snowCoverSettings.AffectGrassTint) {
#	else
		if (SharedData::snowCoverSettings.AffectTreeTint) {
#	endif
			ApplyFoliageColor(color, env_mult);
		}
		if (mult < 0.01)
			return;
		mult *= GetFireAttenuation(p);
		if (mult < 0.01)
			return;
		float2 uv = frac(SharedData::snowCoverSettings.UVScale * (p.xy + worldNormal.xy) / 100);
		float3 diffuse = Color::LinearToSrgb(SnowAlbedo.Sample(SampColorSampler, uv).rgb) * SharedData::snowCoverSettings.MainTint.rgb * Color::PBRLightingScale;

		color = lerp(color, diffuse, mult);
	}

#	if !defined(BASIC_SNOW_COVER)
	float ApplySnowBase(float3 worldNormal, inout float2 uv, out bool alt, float disp, float3 p, float skylight, float waterDist, float viewDist)
	{
		// the range in which water level affects snow
		waterDist = smoothstep(-64, 8, -waterDist - disp);
		// the amount of snow based on weather, TimeSnowing transitions smoothly between -1 in rain and 1 when snowing
		float weatherMult = GetWeatherRange(p, viewDist) * pow(SharedData::snowCoverSettings.TimeSnowing, 3) * max(500, SharedData::snowCoverSettings.SnowingDensity) / 500;
		weatherMult = clamp((weatherMult) * max(SharedData::snowCoverSettings.minAngle, worldNormal.z), -1, 1);
		// the amount of snow based on season and weather
		float env_mult = saturate(max((GetEnvironmentalMultiplier(p) + disp * 5), weatherMult)) - waterDist;
		// LODLANDNOISE is the LOD4 ring; without it that ring gets snow halved and pops at the cell seam
#		if !defined(LANDSCAPE) && !defined(LOD) && !defined(LODLANDSCAPE) && !defined(LODLANDNOISE)
		float distMult = GetObjectFade(viewDist);
#		else
		float distMult = 1;
#		endif
		float mult = distMult * skylight * env_mult * smoothstep(SharedData::snowCoverSettings.minAngle, SharedData::snowCoverSettings.maxAngle, worldNormal.z);
		if (mult <= 0) {
			alt = false;
			return mult;
		}
		mult *= GetFireAttenuation(p);
		if (mult <= 0) {
			alt = false;
			return 0;
		}
		float main_mult = (1 - abs(worldNormal.z - SharedData::snowCoverSettings.peakMainAngle)) + min(0, weatherMult) * SharedData::snowCoverSettings.minAngle;
		float alt_mult = (1 - abs(worldNormal.z - SharedData::snowCoverSettings.peakAltAngle)) + sin(p.z * 0.01 + cos(p.x * p.y * 0.01) * 0.025) * 0.05;
		alt = alt_mult > main_mult;
		// apparently LOD landscape color sampler clamps uvs
		uv = frac(SharedData::snowCoverSettings.UVScale * (p.xy / 100 + worldNormal.xy * disp));
		return min(1, mult);
	}

#		if defined(TRUE_PBR)
	float ApplySnowPBR(inout MaterialProperties material, inout float3 worldNormal, out float mult, float disp, float3 p, float skylight, float waterDist, float viewDist, float2 uv)
	{
		bool alt;
		mult = ApplySnowBase(worldNormal, uv, alt, disp, p, skylight, waterDist, viewDist);
		if (mult <= 0)
			return mult;
		float4 rmaos;
		if (alt) {
			rmaos = IceRmaos.Sample(SampColorSampler, uv);
			float3 albedo = IceAlbedo.Sample(SampColorSampler, uv).rgb;
			albedo = Color::Diffuse(albedo) * SharedData::snowCoverSettings.AltTint.rgb;
			material.BaseColor = lerp(material.BaseColor, albedo, mult * SharedData::snowCoverSettings.AltTint.w);
			worldNormal = TransformNormal(IceNormal.Sample(SampNormalSampler, uv).rgb);
			material.F0 = lerp(material.F0, rmaos.w * SharedData::snowCoverSettings.altSpec, mult);

		} else {
			rmaos = SnowRmaos.Sample(SampColorSampler, uv);
			float3 albedo = SnowAlbedo.Sample(SampColorSampler, uv).rgb;
			albedo = Color::Diffuse(albedo) * SharedData::snowCoverSettings.MainTint.rgb;
			material.BaseColor = lerp(material.BaseColor, albedo, mult * SharedData::snowCoverSettings.MainTint.w);
			worldNormal = TransformNormal(SnowNormal.Sample(SampNormalSampler, uv).rgb);
			material.F0 = lerp(material.F0, rmaos.w * SharedData::snowCoverSettings.mainSpec, mult);
		}
		material.Roughness = lerp(material.Roughness, rmaos.x, mult);
		material.Metallic = lerp(material.Metallic, rmaos.y, mult);
		material.AO = lerp(material.AO, rmaos.z, mult * 0.5);  //always leave a part of the original ao to make it more interesting
		material.GlintScreenSpaceScale = lerp(material.GlintScreenSpaceScale, SharedData::snowCoverSettings.Glint.x, mult);
		material.GlintLogMicrofacetDensity = lerp(material.GlintLogMicrofacetDensity, SharedData::snowCoverSettings.Glint.y, mult);
		material.GlintMicrofacetRoughness = lerp(material.GlintMicrofacetRoughness, SharedData::snowCoverSettings.Glint.z, mult);
		material.GlintDensityRandomization = lerp(material.GlintDensityRandomization, SharedData::snowCoverSettings.Glint.w, mult);
		return mult;
	}
#		else

	float ApplySnow(inout MaterialProperties material, inout float3 worldNormal, float disp, float3 p, float skylight, float waterDist, float viewDist, float2 uv)
	{
		bool alt;
		float mult = ApplySnowBase(worldNormal, uv, alt, disp, p, skylight, waterDist, viewDist);
		if (mult <= 0.0)
			return 0;
		float4 rmaos;
		if (alt) {
			float3 albedo = IceAlbedo.Sample(SampColorSampler, uv).rgb;
			albedo = Color::LinearToSrgb(albedo) * SharedData::snowCoverSettings.AltTint.rgb * Color::PBRLightingScale;
			rmaos = IceRmaos.Sample(SampColorSampler, uv);
			material.Roughness = lerp(material.Roughness, rmaos.x, mult);
			material.Shininess = lerp(material.Shininess, 25 * 500 * SharedData::snowCoverSettings.altSpec * rmaos.w, mult);
			worldNormal = TransformNormal(IceNormal.Sample(SampNormalSampler, uv).rgb);
			material.BaseColor = lerp(material.BaseColor, rmaos.z * albedo, mult * SharedData::snowCoverSettings.AltTint.w);
			material.SpecularColor = lerp(material.SpecularColor, rmaos.y * albedo + float3(1, 1, 1) * (1 - rmaos.y), mult);
			material.F0 = lerp(material.F0, rmaos.w * SharedData::snowCoverSettings.altSpec, mult);
		} else {
			float3 albedo = SnowAlbedo.Sample(SampColorSampler, uv).rgb;
			albedo = Color::LinearToSrgb(albedo) * SharedData::snowCoverSettings.MainTint.rgb * Color::PBRLightingScale;
			rmaos = SnowRmaos.Sample(SampColorSampler, uv);
			material.Roughness = lerp(material.Roughness, rmaos.x, mult);
			material.Shininess = lerp(material.Shininess, 25 * 500 * SharedData::snowCoverSettings.mainSpec * rmaos.w, mult);
			worldNormal = TransformNormal(SnowNormal.Sample(SampNormalSampler, uv).rgb);
			material.BaseColor = lerp(material.BaseColor, rmaos.z * albedo, mult * SharedData::snowCoverSettings.MainTint.w);
			material.SpecularColor = lerp(material.SpecularColor, rmaos.y * albedo + float3(1, 1, 1) * (1 - rmaos.y), mult);
			material.F0 = lerp(material.F0, rmaos.w * SharedData::snowCoverSettings.mainSpec, mult);
		}
		return mult;
	}
#		endif
#	endif

}
#endif
