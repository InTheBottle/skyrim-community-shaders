#include "Common/FrameBuffer.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

#if defined(TERRAIN_BLENDING)
Texture2D<float> SceneDepthTexture : register(t0);
#else
Texture2D<SCENE_DEPTH_FORMAT> SceneDepthTexture : register(t0);
#endif
Texture2D<unorm float2> ContactShadowsTexture : register(t1);
Texture2D<float2> HalfOcclusionTexture : register(t2);
Texture2DArray<float> DistantShadowMap : register(t3);
RWTexture2D<unorm float2> OutputTexture : register(u0);
RWTexture2D<float2> HalfOcclusionRW : register(u1);
SamplerComparisonState DistantShadowMapSampler : register(s1);

#if defined(TERRAIN_SHADOWS)
SamplerState LinearSampler : register(s0);
#	include "TerrainShadows/TerrainShadows.hlsli"
#endif

struct DistantMapCascade
{
	float4 CameraToMap[3];
	float4 Params;
};

cbuffer DistantShadowsCB : register(b1)
{
	float2 RenderSize;
	float2 InvRenderSize;
	float StartDistance;
	float FadeLength;
	float MaxRayLength;
	float Intensity;
	float ThicknessScale;
	uint SampleCount;
	uint UseContactShadows;
	float pad0;
	uint2 HalfSize;
	float2 pad1;
	DistantMapCascade MapCascades[2];
	float MapFilterRadius;
	float MapBiasTexels;
	float MapInvResolution;
	float MapBlendBand;
};

static const float MinStepLength = 32.0;
static const float FirstStepDepthScale = 0.004;
static const float DepthBiasScale = 0.002;
static const float TerrainShadowSkipMargin = 64.0;
static const float UpsampleDepthTolerance = 0.05;
static const float HalfDepthScale = 1.0 / 1024.0;
static const float HalfDepthMax = 65504.0;

float GetViewDepth(uint2 pixel)
{
	return SharedData::GetScreenDepth(SceneDepthTexture[pixel]);
}

float TraceOcclusion(uint2 pixel, uint2 noisePixel, out float viewDepth)
{
	float occlusion = 0.0;

	float depth = SceneDepthTexture[pixel];
	viewDepth = SharedData::GetScreenDepth(depth);
	float firstStep = max(MinStepLength, viewDepth * FirstStepDepthScale);
	bool trace = depth != FrameBuffer::FarPlaneDepth() && viewDepth > StartDistance && firstStep < MaxRayLength;

	float3 positionWS = 0.0;
	[branch] if (trace)
	{
		float2 uv = (pixel + 0.5) * InvRenderSize;
		float4 unprojected = mul(FrameBuffer::CameraViewProjInverse, float4(2.0 * float2(uv.x, 1.0 - uv.y) - 1.0, depth, 1.0));
		positionWS = unprojected.xyz / unprojected.w;

#if defined(TERRAIN_SHADOWS)
		if (TerrainShadows::GetTerrainShadow(positionWS + FrameBuffer::CameraPosAdjust.xyz + float3(0.0, 0.0, TerrainShadowSkipMargin), LinearSampler) <= 0.0) {
			occlusion = 1.0;
			trace = false;
		}
#endif
	}

	[branch] if (trace)
	{
		float4 originCS = mul(FrameBuffer::CameraViewProj, float4(positionWS, 1.0));
		float4 directionCS = mul(FrameBuffer::CameraViewProj, float4(SharedData::DirLightDirection.xyz, 0.0));
		float noise = Random::InterleavedGradientNoise(noisePixel, SharedData::FrameCount);

		float logGrowth = log2(MaxRayLength / firstStep) / SampleCount;
		float growth = exp2(logGrowth);
		float distanceTravelled = firstStep * exp2(logGrowth * noise);

		[loop] for (uint i = 0; i < SampleCount; i++, distanceTravelled *= growth)
		{
			float4 sampleCS = originCS + directionCS * distanceTravelled;
			if (sampleCS.w <= 0.0)
				break;

			float2 sampleNDC = sampleCS.xy / sampleCS.w;
			if (any(abs(sampleNDC) >= 1.0))
				break;

			uint2 samplePixel = uint2((sampleNDC * float2(0.5, -0.5) + 0.5) * RenderSize);
			float depthDelta = sampleCS.w - GetViewDepth(samplePixel);

			float bias = max(MinStepLength, sampleCS.w * DepthBiasScale);
			float thickness = max(distanceTravelled * ThicknessScale, 2.0 * bias);

			float sampleOcclusion = saturate((depthDelta - bias) / bias) * saturate((thickness - depthDelta) / (0.25 * thickness));
			occlusion = max(occlusion, sampleOcclusion);

			if (occlusion >= 1.0)
				break;
		}
	}

	return occlusion;
}

[numthreads(8, 8, 1)] void TraceCS(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= HalfSize))
		return;

	uint2 pixel = min(dispatchID.xy * 2, uint2(RenderSize) - 1);
	float viewDepth;
	float occlusion = TraceOcclusion(pixel, dispatchID.xy, viewDepth);
	HalfOcclusionRW[dispatchID.xy] = float2(occlusion, min(viewDepth * HalfDepthScale, HalfDepthMax));
}

float UpsampleOcclusion(uint2 pixel, float viewDepth)
{
	int2 base = int2(pixel >> 1);
	int2 maxHalf = int2(HalfSize) - 1;
	float2 subPixel = float2(pixel & 1) * 0.5;
	float scaledDepth = viewDepth * HalfDepthScale;
	float rcpTolerance = rcp(max(scaledDepth * UpsampleDepthTolerance, MinStepLength * HalfDepthScale));

	float weightSum = 0.0;
	float occlusionSum = 0.0;
	float closestDelta = 3.402823466e+38;
	float closestOcclusion = 0.0;

	[unroll] for (uint i = 0; i < 4; i++)
	{
		int2 offset = int2(i & 1, i >> 1);
		int2 halfPixel = min(base + offset, maxHalf);
		float2 halfSample = HalfOcclusionTexture[halfPixel];
		float sampleOcclusion = halfSample.x;
		float depthDelta = abs(halfSample.y - scaledDepth);

		float2 bilinear = lerp(1.0 - subPixel, subPixel, float2(offset));
		float weight = bilinear.x * bilinear.y * exp2(-depthDelta * rcpTolerance);
		weightSum += weight;
		occlusionSum += weight * sampleOcclusion;

		if (bilinear.x * bilinear.y > 0.0 && depthDelta < closestDelta) {
			closestDelta = depthDelta;
			closestOcclusion = sampleOcclusion;
		}
	}

	return weightSum > 1e-4 ? occlusionSum / weightSum : closestOcclusion;
}

[numthreads(8, 8, 1)] void ResolveCS(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= uint2(RenderSize)))
		return;

	float depth = SceneDepthTexture[dispatchID.xy];
	if (depth == FrameBuffer::FarPlaneDepth())
		return;

	float viewDepth = SharedData::GetScreenDepth(depth);
	float fade = saturate((viewDepth - StartDistance) / FadeLength);
	if (fade <= 0.0)
		return;

	float shadow = 1.0 - UpsampleOcclusion(dispatchID.xy, viewDepth) * fade * Intensity;
	float2 contactShadows = UseContactShadows ? ContactShadowsTexture[dispatchID.xy] : float2(1.0, 1.0);
	OutputTexture[dispatchID.xy] = contactShadows * shadow;
}

float3 GetMapCoords(uint cascade, float3 positionWS)
{
	float4 position = float4(positionWS, 1.0);
	return float3(dot(MapCascades[cascade].CameraToMap[0], position), dot(MapCascades[cascade].CameraToMap[1], position), dot(MapCascades[cascade].CameraToMap[2], position));
}

float GetMapEdgeWeight(uint cascade, float2 uv)
{
	float2 edgeDistance = min(uv, 1.0 - uv);
	return MapCascades[cascade].Params.z * saturate(min(edgeDistance.x, edgeDistance.y) / MapBlendBand);
}

float SampleMapOcclusion(uint cascade, float3 positionWS)
{
	float3 biasedPosition = positionWS + SharedData::DirLightDirection.xyz * (MapCascades[cascade].Params.x * MapBiasTexels);
	float3 coords = GetMapCoords(cascade, biasedPosition);
	if (coords.z >= 1.0)
		return 0.0;

	float2 offset = MapInvResolution * MapFilterRadius;
	float lit = 0.0;
	[unroll] for (int y = -1; y <= 1; y++)
	{
		[unroll] for (int x = -1; x <= 1; x++)
			lit += DistantShadowMap.SampleCmpLevelZero(DistantShadowMapSampler, float3(coords.xy + float2(x, y) * offset, cascade), coords.z);
	}
	return 1.0 - lit / 9.0;
}

[numthreads(8, 8, 1)] void ShadowMapCS(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= uint2(RenderSize)))
		return;

	float depth = SceneDepthTexture[dispatchID.xy];
	if (depth == FrameBuffer::FarPlaneDepth())
		return;

	float viewDepth = SharedData::GetScreenDepth(depth);
	float fade = saturate((viewDepth - StartDistance) / FadeLength);
	if (fade <= 0.0)
		return;

	float2 uv = (dispatchID.xy + 0.5) * InvRenderSize;
	float4 unprojected = mul(FrameBuffer::CameraViewProjInverse, float4(2.0 * float2(uv.x, 1.0 - uv.y) - 1.0, depth, 1.0));
	float3 positionWS = unprojected.xyz / unprojected.w;

	float nearWeight = GetMapEdgeWeight(0, GetMapCoords(0, positionWS).xy);
	float farWeight = GetMapEdgeWeight(1, GetMapCoords(1, positionWS).xy);

	float occlusion = 0.0;
	[branch] if (nearWeight > 0.0)
		occlusion = nearWeight * SampleMapOcclusion(0, positionWS);
	[branch] if (nearWeight < 1.0 && farWeight > 0.0)
		occlusion += (1.0 - nearWeight) * farWeight * SampleMapOcclusion(1, positionWS);

	float shadow = 1.0 - occlusion * fade * Intensity;
	float2 contactShadows = UseContactShadows ? ContactShadowsTexture[dispatchID.xy] : float2(1.0, 1.0);
	OutputTexture[dispatchID.xy] = contactShadows * shadow;
}
