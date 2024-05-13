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
Texture2D<unorm float> OcclusionMapSampler : register(t4);

RWTexture2D<unorm half2> SkylightingTextureRW : register(u0);

cbuffer PerFrame : register(b1)
{
	row_major float4x4 OcclusionViewProj;
	float4 ShadowDirection;
	float4 BufferDim;
	float4 DynamicRes;
	float4 CameraData;
	uint FrameCount;
	uint pad0[3];
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

cbuffer PerFrameDeferredShared : register(b0)
{
	float4 CamPosAdjust[2];
	float4 DirLightDirectionVS[2];
	float4 DirLightColor;
	float4 CameraData2;
	float2 BufferDim2;
	float2 RcpBufferDim;
	float4x4 ViewMatrix[2];
	float4x4 ProjMatrix[2];
	float4x4 ViewProjMatrix[2];
	float4x4 InvViewMatrix[2];
	float4x4 InvProjMatrix[2];
	float4x4 InvViewProjMatrix[2];
	row_major float3x4 DirectionalAmbient;
	uint FrameCount2;
	uint pad02[3];
};

Texture2D<unorm half3> NormalRoughnessTexture : register(t5);

half3 DecodeNormal(half2 f)
{
	f = f * 2.0 - 1.0;
	// https://twitter.com/Stubbesaurus/status/937994790553227264
	half3 n = half3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
	half t = saturate(-n.z);
	n.xy += n.xy >= 0.0 ? -t : t;
	return -normalize(n);
}

//#define SHADOWMAP
#define PI 3.1415927

#if !defined(SHADOWMAP)
[numthreads(8, 8, 1)] void main(uint3 globalId
								: SV_DispatchThreadID) {
	half2 uv = half2(globalId.xy + 0.5) * BufferDim.zw * DynamicRes.zw;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);

	half3 normalGlossiness = NormalRoughnessTexture[globalId.xy];
	half3 normalVS = DecodeNormal(normalGlossiness.xy);
	half3 normalWS = normalize(mul(InvViewMatrix[eyeIndex], half4(normalVS, 0)));
	half roughness = 1.0 - normalGlossiness.z;

	half rawDepth = DepthTexture[globalId.xy];

	half4 positionCS = half4(2 * half2(uv.x, -uv.y + 1) - 1, rawDepth, 1);

	PerGeometry sD = perShadow[0];

	half4 positionMS = mul(sD.CameraViewProjInverse[eyeIndex], positionCS);
	positionMS.xyz = positionMS.xyz / positionMS.w;

	half3 startPositionMS = positionMS;

	half noise = GetBlueNoise(globalId.xy) * 2.0 * PI;

	half2x2 rotationMatrix = half2x2(cos(noise), sin(noise), -sin(noise), cos(noise));

	half2 PoissonDisk[16] = {
		half2(-0.94201624, -0.39906216),
		half2(0.94558609, -0.76890725),
		half2(-0.094184101, -0.92938870),
		half2(0.34495938, 0.29387760),
		half2(-0.91588581, 0.45771432),
		half2(-0.81544232, -0.87912464),
		half2(-0.38277543, 0.27676845),
		half2(0.97484398, 0.75648379),
		half2(0.44323325, -0.97511554),
		half2(0.53742981, -0.47373420),
		half2(-0.26496911, -0.41893023),
		half2(0.79197514, 0.19090188),
		half2(-0.24188840, 0.99706507),
		half2(-0.81409955, 0.91437590),
		half2(0.19984126, 0.78641367),
		half2(0.14383161, -0.14100790)
	};

	uint sampleCount = 16;

	half2 skylighting = 0;

	half occlusionThreshold = mul(OcclusionViewProj, half4(positionMS.xyz, 1)).z;

	float3 V = normalize(positionMS.xyz);
	float3 R = reflect(V, normalWS);

	float2 weights = 0.0;

	[unroll] for (uint i = 0; i < sampleCount; i++)
	{
		half2 offset = mul(PoissonDisk[i].xy, rotationMatrix);
		half shift = half(i) / half(sampleCount);
		half radius = length(offset);

		positionMS.xy = startPositionMS + offset * 128;

		half2 occlusionPosition = mul((half2x4)OcclusionViewProj, half4(positionMS.xyz, 1));
		occlusionPosition.y = -occlusionPosition.y;
		half2 occlusionUV = occlusionPosition.xy * 0.5 + 0.5;

		half3 offsetDirection = normalize(half3(offset.xy, 0));

		if (occlusionUV.x == saturate(occlusionUV.x) && occlusionUV.y == saturate(occlusionUV.y)) {
			half shadowMapValues = OcclusionMapSampler.SampleCmpLevelZero(ShadowSamplerPCF, occlusionUV, occlusionThreshold - (1e-2 * 0.05 * radius));

			half3 H = normalize(-offsetDirection + V);
			half NoH = dot(normalWS, H);
			half a = NoH * roughness;
			half k = roughness / (1.0 - NoH * NoH + a * a);
			half ggx = k * k * (1.0 / PI);

			half2 contributions = half2(dot(normalWS.xyz, offsetDirection.xyz) * 0.5 + 0.5, ggx);

			skylighting += shadowMapValues * contributions;
			weights += contributions;
		} else {
			skylighting++;
			weights++;
		}
	}

	skylighting /= weights;

	SkylightingTextureRW[globalId.xy] = saturate(skylighting);
}
#else
[numthreads(8, 8, 1)] void main(uint3 globalId
								: SV_DispatchThreadID) {
	half2 uv = half2(globalId.xy + 0.5) * BufferDim.zw * DynamicRes.zw;
	uint eyeIndex = GetEyeIndexFromTexCoord(uv);

	half3 normalGlossiness = NormalRoughnessTexture[globalId.xy];
	half3 normalVS = DecodeNormal(normalGlossiness.xy);
	half3 normalWS = normalize(mul(InvViewMatrix[eyeIndex], half4(normalVS, 0)));
	half roughness = 1.0 - normalGlossiness.z;

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

	half noise = GetBlueNoise(globalId.xy) * 2.0 * PI;

	half2x2 rotationMatrix = half2x2(cos(noise), sin(noise), -sin(noise), cos(noise));

	half2 PoissonDisk[16] = {
		half2(-0.94201624, -0.39906216),
		half2(0.94558609, -0.76890725),
		half2(-0.094184101, -0.92938870),
		half2(0.34495938, 0.29387760),
		half2(-0.91588581, 0.45771432),
		half2(-0.81544232, -0.87912464),
		half2(-0.38277543, 0.27676845),
		half2(0.97484398, 0.75648379),
		half2(0.44323325, -0.97511554),
		half2(0.53742981, -0.47373420),
		half2(-0.26496911, -0.41893023),
		half2(0.79197514, 0.19090188),
		half2(-0.24188840, 0.99706507),
		half2(-0.81409955, 0.91437590),
		half2(0.19984126, 0.78641367),
		half2(0.14383161, -0.14100790)
	};

	uint sampleCount = 16;

	half2 skylighting = 0;

	float3 V = normalize(positionMS.xyz);
	float3 R = reflect(V, normalWS);

	float2 weights = 0.0;

	uint validSamples = 0;
	[unroll] for (uint i = 0; i < sampleCount; i++)
	{
		half2 offset = mul(PoissonDisk[i].xy, rotationMatrix);
		half shift = half(i) / half(sampleCount);
		half radius = length(offset);

		positionMS.xy = startPositionMS + offset.xy * 128 + ShadowDirection.xy * 128;

		half3 offsetDirection = normalize(half3(offset.xy, 0));

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

			float shadowMapValues = TexShadowMapSampler.SampleCmpLevelZero(ShadowSamplerPCF, half3(positionLS.xy, cascadeIndex), positionLS.z - (1e-2 * 0.1 * radius));
			
			half3 H = normalize(-offsetDirection + V);
			half NoH = dot(normalWS, H);
			half a = NoH * roughness;
			half k = roughness / (1.0 - NoH * NoH + a * a);
			half ggx = k * k * (1.0 / PI);

			half2 contributions = half2(dot(normalWS.xyz, offsetDirection.xyz) * 0.5 + 0.5, ggx);

			skylighting += shadowMapValues * contributions;
			weights += contributions;
		}
	}

	skylighting /= weights;

	SkylightingTextureRW[globalId.xy] = lerp(saturate(skylighting), 1.0, fadeFactor);
}
#endif