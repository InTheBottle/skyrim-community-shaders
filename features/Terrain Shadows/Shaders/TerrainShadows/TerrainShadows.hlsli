namespace TerrainShadows
{
	Texture2D<float2> ShadowHeightTexture : register(t60);

	float2 GetTerrainShadowUV(float2 xy)
	{
		return xy * SharedData::terraOccSettings.Scale.xy + SharedData::terraOccSettings.Offset.xy;
	}

	float GetTerrainZ(float norm_z)
	{
		return lerp(SharedData::terraOccSettings.ZRange.x, SharedData::terraOccSettings.ZRange.y, norm_z) - 256;
	}

	float2 GetTerrainZ(float2 norm_z)
	{
		return float2(GetTerrainZ(norm_z.x), GetTerrainZ(norm_z.y));
	}

	float GetTerrainShadow(const float3 worldPos, SamplerState samp)
	{
		if (!SharedData::terraOccSettings.EnableTerrainShadow)
			return 1.0;
		float2 rawHeight = ShadowHeightTexture.SampleLevel(samp, GetTerrainShadowUV(worldPos.xy), 0);
		float2 shadowHeight = GetTerrainZ(rawHeight);
		float penumbra = saturate((worldPos.z - shadowHeight.y) / max(shadowHeight.x - shadowHeight.y, 1e-4));
		return lerp(1.0, penumbra, smoothstep(0.0, 1e-3, rawHeight.x - rawHeight.y));
	}
}
