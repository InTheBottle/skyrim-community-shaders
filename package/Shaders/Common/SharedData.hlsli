#include "Common/Constants.hlsli"
#include "Common/VR.hlsli"

#if defined(PSHADER)

cbuffer SharedData : register(b5)
{
	row_major float3x4 DirectionalAmbientShared;
	float4 DirLightDirectionShared;
	float4 DirLightColorShared;
	float4 CameraData;
	float4 BufferDim;
	float Timer;
	uint FrameCount;
	uint pad0b4[2];
	float WaterHeight[25];
	uint pad1b4[3];
};

struct CPMSettings
{
	bool EnableComplexMaterial;
	bool EnableParallax;
	bool EnableTerrainParallax;
	bool EnableShadows;
};

struct CubemapCreatorSettings
{
	uint Enabled;
	float4 CubemapColor;
};

cbuffer FeatureData : register(b6)
{
	float Glossiness;
	float SpecularStrength;
	float SubsurfaceScatteringAmount;
	bool OverrideComplexGrassSettings;
	float BasicGrassBrightness;
	CPMSettings extendedMaterialSettings;
	CubemapCreatorSettings cubemapCreatorSettings;
	float scatterCoeffMult;
	float absorpCoeffMult;
};

Texture2D<float4> TexDepthSampler : register(t20);

// Get a raw depth from the depth buffer. [0,1] in uv space
float GetDepth(float2 uv, uint a_eyeIndex = 0)
{
	uv = ConvertToStereoUV(uv, a_eyeIndex);
	uv = GetDynamicResolutionAdjustedScreenPosition(uv);
	return TexDepthSampler.Load(int3(uv * BufferDim, 0)).x;
}

float GetScreenDepth(float depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

float GetScreenDepth(float2 uv, uint a_eyeIndex = 0)
{
	float depth = GetDepth(uv, a_eyeIndex);
	return GetScreenDepth(depth);
}

float GetWaterHeight(float3 worldPosition)
{
	float2 cellF = (((worldPosition.xy + CameraPosAdjust[0].xy)) / 4096.0) + 64.0;  // always positive
	int2 cellInt;
	float2 cellFrac = modf(cellF, cellInt);

	cellF = worldPosition.xy / float2(4096.0, 4096.0);  // remap to cell scale
	cellF += 2.5;                                       // 5x5 cell grid
	cellF -= cellFrac;                                  // align to cell borders
	cellInt = round(cellF);

	uint waterTile = (uint)clamp(cellInt.x + (cellInt.y * 5), 0, 24);  // remap xy to 0-24
	float waterHeight = -2147483648;                                   // lowest 32-bit integer

	[flatten] if (cellInt.x < 5 && cellInt.x >= 0 && cellInt.y < 5 && cellInt.y >= 0)
		waterHeight = WaterHeight[waterTile];
	return waterHeight;
}

// Derived from the interleaved gradient function from Jimenez 2014 http://goo.gl/eomGso
float InterleavedGradientNoise(float2 uv)
{
	// Temporal factor
	float frameStep = float(FrameCount % 16) * 0.0625f;
	uv.x += frameStep * 4.7526;
	uv.y += frameStep * 3.1914;

	float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(uv, magic.xy)));
}

#endif