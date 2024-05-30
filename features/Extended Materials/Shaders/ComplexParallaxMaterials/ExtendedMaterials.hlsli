// https://github.com/tgjones/slimshader-cpp/blob/master/src/Shaders/Sdk/Direct3D11/DetailTessellation11/POM.hlsl
// https://github.com/alandtse/SSEShaderTools/blob/main/shaders_vr/ParallaxEffect.h

// https://github.com/marselas/Zombie-Direct3D-Samples/blob/5f53dc2d6f7deb32eb2e5e438d6b6644430fe9ee/Direct3D/ParallaxOcclusionMapping/ParallaxOcclusionMapping.fx
// http://www.diva-portal.org/smash/get/diva2:831762/FULLTEXT01.pdf
// https://bartwronski.files.wordpress.com/2014/03/ac4_gdc.pdf

float GetMipLevel(float2 coords, Texture2D<float4> tex)
{
	// Compute the current gradients:
	float2 textureDims;
	tex.GetDimensions(textureDims.x, textureDims.y);

	float2 texCoordsPerSize = coords * textureDims;

	float2 dxSize = ddx(texCoordsPerSize);
	float2 dySize = ddy(texCoordsPerSize);

	// Find min of change in u and v across quad: compute du and dv magnitude across quad
	float2 dTexCoords = dxSize * dxSize + dySize * dySize;

	// Standard mipmapping uses max here
	float minTexCoordDelta = max(dTexCoords.x, dTexCoords.y);

	// Compute the current mip level  (* 0.5 is effectively computing a square root before )
	float mipLevel = max(0.5 * log2(minTexCoordDelta), 0);

	mipLevel++;  // Adjust scaling

#if !defined(PARALLAX)
	mipLevel++;
#endif
	return mipLevel;
}

#if defined(LANDSCAPE)
float2 GetParallaxCoords(float distance, float2 coords, float mipLevel, float3 viewDir, float3x3 tbn, Texture2D<float4> tex, SamplerState texSampler, uint channel, float blend, out float pixelOffset)
#else
float2 GetParallaxCoords(float distance, float2 coords, float mipLevel, float3 viewDir, float3x3 tbn, Texture2D<float4> tex, SamplerState texSampler, uint channel, out float pixelOffset)
#endif
{
	float3 viewDirTS = normalize(mul(tbn, viewDir));
	viewDirTS.xy /= viewDirTS.z * 0.5 + 0.5;

	float nearBlendToFar = saturate(distance / 4096.0);

	float maxHeight = 0.1;
	float minHeight = maxHeight * 0.5;

	float2 output;
	if (nearBlendToFar < 1.0) {
		uint numSteps;

#if defined(LANDSCAPE)
		numSteps = uint((32 * (1.0 - nearBlendToFar) * sqrt(saturate(blend))) + 0.5);
		numSteps = clamp((numSteps + 3) & ~0x03, 4, 32);
#else
		numSteps = uint((32 * (1.0 - nearBlendToFar)) + 0.5);
		numSteps = clamp((numSteps + 3) & ~0x03, 4, 32);
#endif

		mipLevel--;

		float stepSize = rcp(numSteps);

		float2 offsetPerStep = viewDirTS.xy * float2(maxHeight, maxHeight) * stepSize.xx;
		float2 prevOffset = viewDirTS.xy * float2(minHeight, minHeight) + coords.xy;

		float prevBound = 1.0;
		float prevHeight = 1.0;

		float2 pt1 = 0;
		float2 pt2 = 0;

		[loop] while (numSteps > 0)
		{
			float4 currentOffset[2];
			currentOffset[0] = prevOffset.xyxy - float4(1, 1, 2, 2) * offsetPerStep.xyxy;
			currentOffset[1] = prevOffset.xyxy - float4(3, 3, 4, 4) * offsetPerStep.xyxy;
			float4 currentBound = prevBound.xxxx - float4(1, 2, 3, 4) * stepSize;

			float4 currHeight;
			currHeight.x = tex.SampleLevel(texSampler, currentOffset[0].xy, mipLevel)[channel];
			currHeight.y = tex.SampleLevel(texSampler, currentOffset[0].zw, mipLevel)[channel];
			currHeight.z = tex.SampleLevel(texSampler, currentOffset[1].xy, mipLevel)[channel];
			currHeight.w = tex.SampleLevel(texSampler, currentOffset[1].zw, mipLevel)[channel];

			bool4 testResult = currHeight >= currentBound;
			[branch] if (any(testResult))
			{
				[flatten] if (testResult.w)
				{
					pt1 = float2(currentBound.w, currHeight.w);
					pt2 = float2(currentBound.z, currHeight.z);
				}
				[flatten] if (testResult.z)
				{
					pt1 = float2(currentBound.z, currHeight.z);
					pt2 = float2(currentBound.y, currHeight.y);
				}
				[flatten] if (testResult.y)
				{
					pt1 = float2(currentBound.y, currHeight.y);
					pt2 = float2(currentBound.x, currHeight.x);
				}
				[flatten] if (testResult.x)
				{
					pt1 = float2(currentBound.x, currHeight.x);
					pt2 = float2(prevBound, prevHeight);
				}
				break;
			}

			prevOffset = currentOffset[1].zw;
			prevBound = currentBound.w;
			prevHeight = currHeight.w;
			numSteps -= 4;
		}

		float delta2 = pt2.x - pt2.y;
		float delta1 = pt1.x - pt1.y;
		float denominator = delta2 - delta1;

		float parallaxAmount = 0.0;
		[flatten] if (denominator == 0.0)
		{
			parallaxAmount = 0.0;
		}
		else
		{
			parallaxAmount = (pt1.x * delta2 - pt2.x * delta1) / denominator;
		}

		float offset = (1.0 - parallaxAmount) * -maxHeight + minHeight;
		pixelOffset = lerp(parallaxAmount, 0.5, nearBlendToFar);
		return output = lerp(viewDirTS.xy * offset + coords.xy, coords, nearBlendToFar);
	}

	pixelOffset = 0.5;
	return coords;
}

// https://advances.realtimerendering.com/s2006/Tatarchuk-POM.pdf

// Cheap method of creating shadows using height for a given light source
float GetParallaxSoftShadowMultiplier(float2 coords, float mipLevel, float3 L, float sh0, Texture2D<float4> tex, SamplerState texSampler, uint channel, float quality, float noise)
{
	[branch] if (quality > 0.0)
	{
		float2 rayDir = L.xy * 0.1 / L.z;
		float4 multipliers = 1.0 / ((float4(4, 3, 2, 1) * 2) + noise);
		float4 sh;
		sh.x = tex.SampleLevel(texSampler, coords + rayDir * multipliers.x, mipLevel)[channel];
		sh.y = tex.SampleLevel(texSampler, coords + rayDir * multipliers.y, mipLevel)[channel];
		sh.z = tex.SampleLevel(texSampler, coords + rayDir * multipliers.z, mipLevel)[channel];
		sh.w = tex.SampleLevel(texSampler, coords + rayDir * multipliers.w, mipLevel)[channel];
		return 1.0 - saturate(dot(sh - sh0 - multipliers > 0.0, 1.0)) * 0.75 * quality;
	}
	return 1.0;
}

float GetParallaxSoftShadowMultiplierTerrain(float2 coords, float mipLevel, float sh0, Texture2D<float4> tex, SamplerState texSampler, float2 rayDir, float4 multipliers)
{
	float4 sh;
	sh.x = tex.SampleLevel(texSampler, coords + rayDir * multipliers.x, mipLevel).w;
	sh.y = tex.SampleLevel(texSampler, coords + rayDir * multipliers.y, mipLevel).w;
	sh.z = tex.SampleLevel(texSampler, coords + rayDir * multipliers.z, mipLevel).w;
	sh.w = tex.SampleLevel(texSampler, coords + rayDir * multipliers.w, mipLevel).w;
	return 1.0 - saturate(dot(sh - sh0 - multipliers > 0.0, 1.0)) * 0.75;
}

#if defined(LANDSCAPE)
float GetParallaxSoftShadowMultiplierTerrain(PS_INPUT input, float2 coords[6], float mipLevel[6], float3 L, float sh0[6], float quality, float noise)
{
	float2 rayDir = L.xy * 0.1 / L.z;
	float4 multipliers = 1.0 / ((float4(4, 3, 2, 1) * 2) + noise);

	float occlusion = 0.0;
	if (quality > 0.0) {
		if (input.LandBlendWeights1.x > 0.0)
			occlusion += GetParallaxSoftShadowMultiplierTerrain(coords[0], mipLevel[0], sh0[0], TexColorSampler, SampTerrainParallaxSampler, rayDir, multipliers) * input.LandBlendWeights1.x;
		if (input.LandBlendWeights1.y > 0.0)
			occlusion += GetParallaxSoftShadowMultiplierTerrain(coords[1], mipLevel[1], sh0[1], TexLandColor2Sampler, SampTerrainParallaxSampler, rayDir, multipliers) * input.LandBlendWeights1.y;
		if (input.LandBlendWeights1.z > 0.0)
			occlusion += GetParallaxSoftShadowMultiplierTerrain(coords[2], mipLevel[2], sh0[2], TexLandColor3Sampler, SampTerrainParallaxSampler, rayDir, multipliers) * input.LandBlendWeights1.z;
		if (input.LandBlendWeights1.w > 0.0)
			occlusion += GetParallaxSoftShadowMultiplierTerrain(coords[3], mipLevel[3], sh0[3], TexLandColor4Sampler, SampTerrainParallaxSampler, rayDir, multipliers) * input.LandBlendWeights1.w;
		if (input.LandBlendWeights2.x > 0.0)
			occlusion += GetParallaxSoftShadowMultiplierTerrain(coords[4], mipLevel[4], sh0[4], TexLandColor5Sampler, SampTerrainParallaxSampler, rayDir, multipliers) * input.LandBlendWeights2.x;
		if (input.LandBlendWeights2.y > 0.0)
			occlusion += GetParallaxSoftShadowMultiplierTerrain(coords[5], mipLevel[5], sh0[5], TexLandColor6Sampler, SampTerrainParallaxSampler, rayDir, multipliers) * input.LandBlendWeights2.y;
	}
	return lerp(1.0, saturate(occlusion), quality);  // Blend weights seem to go greater than 1.0 sometimes
}
#endif
