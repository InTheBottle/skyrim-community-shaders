#include "../Common/FrameBuffer.hlsl"
#include "../Common/GBuffer.hlsli"
#include "../Common/Spherical Harmonics/SphericalHarmonics.hlsli"
#include "../Common/VR.hlsli"

Texture2D<float> DepthTexture : register(t0);

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
	// Since PerGeometry is passed between c++ and hlsl, can't have different defines due to strong typing
	float4x3 ShadowMapProj[2][3];
	float4x4 CameraViewProjInverse[2];
};

Texture2DArray<unorm float> TexShadowMapSampler : register(t1);
StructuredBuffer<PerGeometry> perShadow : register(t2);
Texture2DArray<float4> BlueNoise : register(t3);
Texture2D<unorm float> OcclusionMapSampler : register(t4);

RWTexture2D<unorm half4> SkylightingTextureRW : register(u0);
RWTexture2D<unorm half> WetnessOcclusionTextureRW : register(u1);

cbuffer PerFrame : register(b0)
{
	row_major float4x4 OcclusionViewProj;
	float4 EyePosition;
	float4 ShadowDirection;
	float4 BufferDim;
	float4 CameraData;
	uint FrameCount;
};

SamplerState LinearSampler : register(s0);
SamplerComparisonState ShadowSamplerPCF : register(s1);

half GetBlueNoise(half2 uv)
{
	return BlueNoise[uint3(uv % 128, FrameCount % 64)];
}

#if !defined(SHADOWMAP)
[numthreads(8, 8, 1)] void main(uint3 globalId
								: SV_DispatchThreadID) {
	float2 uv = float2(globalId.xy + 0.5) * (BufferDim.zw) * DynamicResolutionParams2.xy;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);
	uv = ConvertFromStereoUV(uv, eyeIndex);

	float rawDepth = DepthTexture[globalId.xy];

	float4 positionCS = float4(2 * float2(uv.x, -uv.y + 1) - 1, rawDepth, 1);

	float4 positionMS = mul(CameraViewProjInverse[eyeIndex], positionCS);
	positionMS.xyz = positionMS.xyz / positionMS.w;

	positionMS.xyz = positionMS.xyz + CameraPosAdjust[eyeIndex] - EyePosition;

	float3 startPositionMS = positionMS;

	half noise = GetBlueNoise(globalId.xy) * 2.0 * shPI;

	half2x2 rotationMatrix = half2x2(cos(noise), sin(noise), -sin(noise), cos(noise));

	float2 PoissonDisc[16] = {
		float2(0.107883f, 0.374004f),
		float2(0.501633f, 0.773888f),
		float2(0.970519f, 0.248024f),
		float2(0.999939f, 0.896329f),
		float2(0.492874f, 0.0122379f),
		float2(0.00650044f, 0.943358f),
		float2(0.569201f, 0.382672f),
		float2(0.0345164f, 0.00137333f),
		float2(0.93289f, 0.616749f),
		float2(0.3155f, 0.989013f),
		float2(0.197119f, 0.701132f),
		float2(0.721946f, 0.983612f),
		float2(0.773705f, 0.0301218f),
		float2(0.400403f, 0.541612f),
		float2(0.728111f, 0.236213f),
		float2(0.240547f, 0.0980255f)
	};

	uint sampleCount = 16;

	float occlusionThreshold = mul(OcclusionViewProj, float4(positionMS.xyz, 1)).z;

	bool fadeOut = length(startPositionMS) > 1024;

	half weight = 0;
	half wetnessWeight = 0;

	sh2 shSkylighting = shZero();
	float wetnessOcclusion = 0;

	half fadeFactor = pow(saturate(length(positionMS.xyz) / 10000.0), 8);

	[unroll] for (uint i = 0; i < sampleCount; i++)
	{
		float3 rayDir = float3(PoissonDisc[i].xy * 2.0 - 1.0, 0);
		rayDir.xy = mul(rayDir.xy, rotationMatrix);

		positionMS.xy = startPositionMS + rayDir.xy * 128;

		rayDir = normalize(rayDir);

		half2 occlusionPosition = mul((float2x4)OcclusionViewProj, float4(positionMS.xyz, 1));
		occlusionPosition.y = -occlusionPosition.y;
		half2 occlusionUV = occlusionPosition.xy * 0.5 + 0.5;

		float wetnessScale = 1.0 - (length(rayDir) * 0.5);

		if ((occlusionUV.x == saturate(occlusionUV.x) && occlusionUV.y == saturate(occlusionUV.y)) || !fadeOut) {
			half shadowMapValues = saturate((OcclusionMapSampler.SampleLevel(LinearSampler, occlusionUV, 0) - occlusionThreshold + 0.0001) * 1024);

			sh2 sh = shEvaluate(rayDir);
			shSkylighting = shAdd(shSkylighting, shScale(sh, lerp(shadowMapValues, 1.0, fadeFactor)));
			wetnessOcclusion += shadowMapValues * wetnessScale;
			wetnessWeight += wetnessScale;
			weight++;
		} else {
			sh2 sh = shEvaluate(rayDir);
			shSkylighting = shAdd(shSkylighting, shScale(sh, 1));
			weight++;
			wetnessWeight += wetnessScale;
		}
	}

	if (weight > 0.0) {
		float shFactor = 4.0 * shPI / weight;
		shSkylighting = shScale(shSkylighting, shFactor);
		wetnessOcclusion /= wetnessWeight;
	}

	SkylightingTextureRW[globalId.xy] = shSkylighting * 0.5 + 0.5;
	WetnessOcclusionTextureRW[globalId.xy] = saturate(wetnessOcclusion * wetnessOcclusion * 4);
}
#else

half GetScreenDepth(half depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

[numthreads(8, 8, 1)] void main(uint3 globalId
								: SV_DispatchThreadID) {
	float2 uv = float2(globalId.xy + 0.5) * BufferDim.zw * DynamicResolutionParams2.xy;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);
	uv = ConvertFromStereoUV(uv, eyeIndex);

	float rawDepth = DepthTexture[globalId.xy];

	float4 positionCS = float4(2 * float2(uv.x, -uv.y + 1) - 1, rawDepth, 1);

	PerGeometry sD = perShadow[0];

	sD.EndSplitDistances.x = GetScreenDepth(sD.EndSplitDistances.x);
	sD.EndSplitDistances.y = GetScreenDepth(sD.EndSplitDistances.y);
	sD.EndSplitDistances.z = GetScreenDepth(sD.EndSplitDistances.z);
	sD.EndSplitDistances.w = GetScreenDepth(sD.EndSplitDistances.w);

	float4 positionMS = mul(CameraViewProjInverse[eyeIndex], positionCS);
	positionMS.xyz = positionMS.xyz / positionMS.w;

	float3 startPositionMS = positionMS;

	half fadeFactor = pow(saturate(dot(positionMS.xyz, positionMS.xyz) / sD.ShadowLightParam.z), 8);

	fadeFactor = lerp(1.0, fadeFactor, pow(saturate(dot(float3(0, 0, -1), ShadowDirection.xyz)), 0.25));

	half noise = GetBlueNoise(globalId.xy) * 2.0 * shPI;

	half2x2 rotationMatrix = half2x2(cos(noise), sin(noise), -sin(noise), cos(noise));

	float2 PoissonDisc[16] = {
		float2(0.107883f, 0.374004f),
		float2(0.501633f, 0.773888f),
		float2(0.970519f, 0.248024f),
		float2(0.999939f, 0.896329f),
		float2(0.492874f, 0.0122379f),
		float2(0.00650044f, 0.943358f),
		float2(0.569201f, 0.382672f),
		float2(0.0345164f, 0.00137333f),
		float2(0.93289f, 0.616749f),
		float2(0.3155f, 0.989013f),
		float2(0.197119f, 0.701132f),
		float2(0.721946f, 0.983612f),
		float2(0.773705f, 0.0301218f),
		float2(0.400403f, 0.541612f),
		float2(0.728111f, 0.236213f),
		float2(0.240547f, 0.0980255f)
	};

	uint sampleCount = 16;

	sh2 shSkylighting = shZero();
	float wetnessOcclusion = 0;

	half weight = 0.0;
	half wetnessWeight = 0;

	[unroll] for (uint i = 0; i < sampleCount; i++)
	{
		float3 rayDir = float3(PoissonDisc[i].xy * 2.0 - 1.0, 0);
		rayDir.xy = mul(rayDir.xy, rotationMatrix);

		positionMS.xy = startPositionMS + rayDir.xy * 128 + length(rayDir.xy) * ShadowDirection.xy * 128;

		rayDir = normalize(rayDir);

		float shadowMapDepth = length(positionMS.xyz);

		[flatten] if (sD.EndSplitDistances.z > shadowMapDepth)
		{
			half cascadeIndex = 0;
			float4x3 lightProjectionMatrix = sD.ShadowMapProj[eyeIndex][0];

			[flatten] if (2.5 < sD.EndSplitDistances.w && sD.EndSplitDistances.y < shadowMapDepth)
			{
				lightProjectionMatrix = sD.ShadowMapProj[eyeIndex][2];
				cascadeIndex = 2;
			}
			else if (sD.EndSplitDistances.x < shadowMapDepth)
			{
				lightProjectionMatrix = sD.ShadowMapProj[eyeIndex][1];
				cascadeIndex = 1;
			}

			float3 positionLS = mul(transpose(lightProjectionMatrix), float4(positionMS.xyz, 1)).xyz;
			half shadowMapValues = saturate((TexShadowMapSampler.SampleLevel(LinearSampler, float3(positionLS.xy, cascadeIndex), 0) - positionLS.z + 0.0001) * 1024);

			sh2 sh = shEvaluate(rayDir);
			shSkylighting = shAdd(shSkylighting, shScale(sh, lerp(shadowMapValues, 1.0, fadeFactor)));

			float wetnessScale = 1.0 - (length(rayDir) * 0.5);
			wetnessOcclusion += shadowMapValues * wetnessScale;
			wetnessWeight += wetnessScale;

			weight++;
		}
		else
		{
			sh2 sh = shEvaluate(rayDir);
			shSkylighting = shAdd(shSkylighting, shScale(sh, 1.0));

			float wetnessScale = 1.0 - (length(rayDir) * 0.5);
			wetnessOcclusion += wetnessScale;
			wetnessWeight += wetnessScale;

			weight++;
		}
	}

	if (weight > 0.0) {
		float shFactor = 4.0 * shPI / weight;
		shSkylighting = shScale(shSkylighting, shFactor);
		wetnessOcclusion /= wetnessWeight;
	}

	SkylightingTextureRW[globalId.xy] = shSkylighting * 0.5 + 0.5;
	WetnessOcclusionTextureRW[globalId.xy] = saturate(wetnessOcclusion * wetnessOcclusion * 4);
}
#endif