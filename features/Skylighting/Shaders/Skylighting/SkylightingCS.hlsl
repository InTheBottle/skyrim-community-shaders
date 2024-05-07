#include "../Common/VR.hlsli"

Texture2D<unorm float> DepthTexture : register(t0);

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
#if !defined(VR)
	float4x3 ShadowMapProj[1][3];
	float4x4 CameraViewProjInverse[1];
#else
	float4x3 ShadowMapProj[2][3];
	float4x4 CameraViewProjInverse[2];
#endif  // VR
};

Texture2DArray<unorm float> TexShadowMapSampler : register(t1);
StructuredBuffer<PerGeometry> perShadow : register(t2);
Texture2DArray<float4> BlueNoise : register(t3);

RWTexture2D<unorm half> SkylightingTextureRW : register(u0);

cbuffer PerFrame : register(b0)
{
	float4 BufferDim;
	float4 DynamicRes;
	float4 CameraData;
	uint FrameCount;
};

SamplerState LinearSampler : register(s0);
SamplerComparisonState ShadowSamplerPCF : register(s1);

half GetBlueNoise(half2 uv)
{
	return BlueNoise[uint3(uv % 128, FrameCount % 64)];
}

half GetScreenDepth(half depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

[numthreads(8, 8, 1)] void main(uint3 globalId
								: SV_DispatchThreadID) {
	half2 uv = half2(globalId.xy + 0.5) * BufferDim.zw * DynamicRes.zw;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);
	half rawDepth = DepthTexture[globalId.xy];

	half4 positionCS = half4(2 * half2(uv.x, -uv.y + 1) - 1, rawDepth, 1);

	PerGeometry sD = perShadow[0];
	sD.EndSplitDistances.x = GetScreenDepth(sD.EndSplitDistances.x);
	sD.EndSplitDistances.y = GetScreenDepth(sD.EndSplitDistances.y);
	sD.EndSplitDistances.z = GetScreenDepth(sD.EndSplitDistances.z);
	sD.EndSplitDistances.w = GetScreenDepth(sD.EndSplitDistances.w);

	half4 positionMS = mul(sD.CameraViewProjInverse[eyeIndex], positionCS);
	positionMS.xyz = positionMS.xyz / positionMS.w;

	half3 startPositionMS = positionMS;

	half fadeFactor = pow(saturate(dot(positionMS.xyz, positionMS.xyz) / sD.ShadowLightParam.z), 8);

	half noise = GetBlueNoise(globalId.xy) * 2.0 - 1.0;

	float2x2 rotationMatrix = float2x2(cos(noise), sin(noise), -sin(noise), cos(noise));

	float2 PoissonDisk[16] = {
		float2(-0.94201624, -0.39906216),
		float2(0.94558609, -0.76890725),
		float2(-0.094184101, -0.92938870),
		float2(0.34495938, 0.29387760),
		float2(-0.91588581, 0.45771432),
		float2(-0.81544232, -0.87912464),
		float2(-0.38277543, 0.27676845),
		float2(0.97484398, 0.75648379),
		float2(0.44323325, -0.97511554),
		float2(0.53742981, -0.47373420),
		float2(-0.26496911, -0.41893023),
		float2(0.79197514, 0.19090188),
		float2(-0.24188840, 0.99706507),
		float2(-0.81409955, 0.91437590),
		float2(0.19984126, 0.78641367),
		float2(0.14383161, -0.14100790)
	};

	uint sampleCount = 16;
	half range = 256;

	half skylighting = 0;

	[unroll] for (uint i = 0; i < sampleCount; i++)
	{
		half2 offset = mul(PoissonDisk[i].xy, rotationMatrix) * range;

		positionMS.xy = startPositionMS + offset;

		half shadowMapDepth = length(positionMS.xyz);

		[flatten] if (sD.EndSplitDistances.z > shadowMapDepth)
		{
			half cascadeIndex = 0;
			half4x3 lightProjectionMatrix = sD.ShadowMapProj[eyeIndex][0];
			half shadowMapThreshold = sD.AlphaTestRef.y;

			[flatten] if (2.5 < sD.EndSplitDistances.w && sD.EndSplitDistances.y < shadowMapDepth)
			{
				lightProjectionMatrix = sD.ShadowMapProj[eyeIndex][2];
				shadowMapThreshold = sD.AlphaTestRef.z;
				cascadeIndex = 2;
			}
			else if (sD.EndSplitDistances.x < shadowMapDepth)
			{
				lightProjectionMatrix = sD.ShadowMapProj[eyeIndex][1];
				shadowMapThreshold = sD.AlphaTestRef.z;
				cascadeIndex = 1;
			}

			half3 positionLS = mul(transpose(lightProjectionMatrix), half4(positionMS.xyz, 1)).xyz;

			float shadowMapValues = TexShadowMapSampler.SampleCmpLevelZero(ShadowSamplerPCF, half3(positionLS.xy, cascadeIndex), positionLS.z - shadowMapThreshold * PoissonDisk[i].x);
			skylighting += shadowMapValues;
		}
	}

	skylighting /= half(sampleCount);

	SkylightingTextureRW[globalId.xy] = lerp(saturate(skylighting), 1.0, fadeFactor);
}
