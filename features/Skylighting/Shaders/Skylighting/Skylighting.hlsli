#ifndef __SKYLIGHTING_DEPENDENCY_HLSL__
#define __SKYLIGHTING_DEPENDENCY_HLSL__

#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/Shading.hlsli"
#include "Common/SharedData.hlsli"

namespace Skylighting
{
#if defined(SKYLIGHTING_PROBE_REGISTER)
	Texture3D<uint> SkylightingProbeArray : register(SKYLIGHTING_PROBE_REGISTER);
#elif defined(PSHADER)
	Texture3D<uint> SkylightingProbeArray : register(t50);
#endif

	const static uint NUM_DIRECTIONS = 32;
	const static float GOLDEN_ANGLE = 2.39996323;

	const static uint3 ARRAY_DIM = uint3(256, 256, 128);
	const static float3 ARRAY_SIZE = 10000.f * float3(1, 1, 0.5);
	const static float3 CELL_SIZE = ARRAY_SIZE / ARRAY_DIM;

	float3 GetHemisphereDirection(uint index)
	{
		float maxZenith = SharedData::skylightingSettings.MaxZenith;
		float cosMaxZenith = cos(maxZenith);
		float z = cosMaxZenith + (1.0 - cosMaxZenith) * (float(index) + 0.5) / float(NUM_DIRECTIONS);
		float r = sqrt(1.0 - z * z);
		float phi = index * GOLDEN_ANGLE;
		return float3(r * cos(phi), r * sin(phi), z);
	}

	struct ProbeData
	{
		uint bitmask0, bitmask1, bitmask2, bitmask3;
		uint bitmask4, bitmask5, bitmask6, bitmask7;
		float weight0, weight1, weight2, weight3;
		float weight4, weight5, weight6, weight7;
	};

	uint GetBitmask(ProbeData data, uint i)
	{
		if (i == 0) return data.bitmask0;
		if (i == 1) return data.bitmask1;
		if (i == 2) return data.bitmask2;
		if (i == 3) return data.bitmask3;
		if (i == 4) return data.bitmask4;
		if (i == 5) return data.bitmask5;
		if (i == 6) return data.bitmask6;
		return data.bitmask7;
	}

	float GetWeight(ProbeData data, uint i)
	{
		if (i == 0) return data.weight0;
		if (i == 1) return data.weight1;
		if (i == 2) return data.weight2;
		if (i == 3) return data.weight3;
		if (i == 4) return data.weight4;
		if (i == 5) return data.weight5;
		if (i == 6) return data.weight6;
		return data.weight7;
	}

	ProbeData MakeDefaultProbeData()
	{
		ProbeData data;
		data.bitmask0 = data.bitmask1 = data.bitmask2 = data.bitmask3 = 0xFFFFFFFF;
		data.bitmask4 = data.bitmask5 = data.bitmask6 = data.bitmask7 = 0xFFFFFFFF;
		data.weight0 = data.weight1 = data.weight2 = data.weight3 = 0;
		data.weight4 = data.weight5 = data.weight6 = data.weight7 = 0;
		return data;
	}

	float GetFadeOutFactor(float3 positionMS)
	{
		float3 uvw = saturate(positionMS / ARRAY_SIZE + .5);
		float3 dists = min(uvw, 1 - uvw);
		float edgeDist = min(dists.x, min(dists.y, dists.z));
		return saturate(edgeDist * 20);
	}

	float MixDiffuse(float visibility)
	{
		return lerp(SharedData::skylightingSettings.MinDiffuseVisibility, 1.0, visibility);
	}

	float MixSpecular(float visibility)
	{
		return lerp(SharedData::skylightingSettings.MinSpecularVisibility, 1.0, visibility);
	}

	float EvaluateBitmask(uint bitmask, float3 normal)
	{
		float vis = 0;
		float wsum = 0;
		[unroll] for (uint d = 0; d < NUM_DIRECTIONS; d++) {
			float w = max(0, dot(GetHemisphereDirection(d), normal));
			wsum += w;
			if (bitmask & (1u << d))
				vis += w;
		}
		return vis / max(wsum, 0.001);
	}

	float EvaluateBitmaskSpecular(uint bitmask, float3 dominantDir, float halfAngle)
	{
		float vis = 0;
		float wsum = 0;
		[unroll] for (uint d = 0; d < NUM_DIRECTIONS; d++) {
			float w = saturate((dot(GetHemisphereDirection(d), dominantDir) - halfAngle) / (1.0 - halfAngle));
			wsum += w;
			if (bitmask & (1u << d))
				vis += w;
		}
		return vis / max(wsum, 0.001);
	}

	float EvaluateDiffuse(ProbeData data, float3 normal, float fadeOutFactor = 1.0)
	{
		float visibility = 0;
		float wsum = 0;
		[unroll] for (uint p = 0; p < 8; p++) {
			float w = GetWeight(data, p);
			if (w > 0) {
				visibility += EvaluateBitmask(GetBitmask(data, p), normal) * w;
				wsum += w;
			}
		}
		visibility /= max(wsum, 0.001);
		visibility = lerp(1.0, saturate(visibility), fadeOutFactor);
		return MixDiffuse(visibility);
	}

	float EvaluateSpecular(ProbeData data, float3 dominantDir, float halfAngle, float fadeOutFactor = 1.0)
	{
		float visibility = 0;
		float wsum = 0;
		[unroll] for (uint p = 0; p < 8; p++) {
			float w = GetWeight(data, p);
			if (w > 0) {
				visibility += EvaluateBitmaskSpecular(GetBitmask(data, p), dominantDir, halfAngle) * w;
				wsum += w;
			}
		}
		visibility /= max(wsum, 0.001);
		visibility = lerp(1.0, saturate(visibility), fadeOutFactor);
		return MixSpecular(visibility);
	}

#if defined(PSHADER)
	void ApplySkylighting(inout float3 diffuseColor, inout float3 directionalAmbientColor, float3 albedo, float skylightingDiffuse)
	{
		float maxScale = 1.0;
		if (directionalAmbientColor.x > 0.0)
			maxScale = min(maxScale, diffuseColor.x / directionalAmbientColor.x);
		if (directionalAmbientColor.y > 0.0)
			maxScale = min(maxScale, diffuseColor.y / directionalAmbientColor.y);
		if (directionalAmbientColor.z > 0.0)
			maxScale = min(maxScale, diffuseColor.z / directionalAmbientColor.z);
		directionalAmbientColor *= maxScale;

		diffuseColor = max(0.0, diffuseColor - directionalAmbientColor);

		float3 linAmbient = Color::IrradianceToLinear(directionalAmbientColor);
		float3 multiBounceSkylighting = MultiBounceAO(albedo, skylightingDiffuse);
		directionalAmbientColor = Color::IrradianceToGamma(linAmbient * multiBounceSkylighting);

		diffuseColor += directionalAmbientColor;
	}
#endif

#if defined(PSHADER) || defined(SKYLIGHTING_PROBE_REGISTER)
	void SetProbe(inout ProbeData data, uint n, uint bitmask, float w)
	{
		if (n == 0) { data.bitmask0 = bitmask; data.weight0 = w; }
		else if (n == 1) { data.bitmask1 = bitmask; data.weight1 = w; }
		else if (n == 2) { data.bitmask2 = bitmask; data.weight2 = w; }
		else if (n == 3) { data.bitmask3 = bitmask; data.weight3 = w; }
		else if (n == 4) { data.bitmask4 = bitmask; data.weight4 = w; }
		else if (n == 5) { data.bitmask5 = bitmask; data.weight5 = w; }
		else if (n == 6) { data.bitmask6 = bitmask; data.weight6 = w; }
		else { data.bitmask7 = bitmask; data.weight7 = w; }
	}

	ProbeData Sample(float3 positionMS, float3 normalWS, float2 screenPosition)
	{
		ProbeData data = MakeDefaultProbeData();

		if (SharedData::InInterior)
			return data;

		positionMS.xyz += normalWS * CELL_SIZE * 0.5;

		float3 positionMSAdjusted = positionMS - SharedData::skylightingSettings.PosOffset.xyz;
		float3 uvw = positionMSAdjusted / ARRAY_SIZE + .5;

		if (any(uvw < 0) || any(uvw > 1))
			return data;

		float3 cellVxCoord = uvw * ARRAY_DIM;
		int3 cell000 = floor(cellVxCoord - 0.5);
		float3 trilinearPos = cellVxCoord - 0.5 - cell000;

		[unroll] for (int i = 0; i < 2; i++)
			[unroll] for (int j = 0; j < 2; j++)
				[unroll] for (int k = 0; k < 2; k++) {
					uint n = i * 4 + j * 2 + k;
					int3 cellOffset = int3(i, j, k);
					int3 cellIdx = cell000 + cellOffset;

					if (any(cellIdx < 0) || any((uint3)cellIdx >= ARRAY_DIM))
						continue;

					float3 cellCentreMS = cellIdx + 0.5 - ARRAY_DIM / 2;
					cellCentreMS = cellCentreMS * CELL_SIZE;

					float tangentWeight = dot(normalize(cellCentreMS - positionMSAdjusted), normalWS) * 0.5 + 0.5;

					float3 trilinearWeights = 1 - abs(cellOffset - trilinearPos);
					float w = trilinearWeights.x * trilinearWeights.y * trilinearWeights.z * tangentWeight;

					uint3 cellTexID = (cellIdx + SharedData::skylightingSettings.ArrayOrigin.xyz) % ARRAY_DIM;
					SetProbe(data, n, SkylightingProbeArray[cellTexID], w);
				}

		return data;
	}

	float GetSkylightingDiffuse(ProbeData data, float3 positionMS, float3 evalNormal, float vertexAO = 1.0)
	{
		if (SharedData::InInterior)
			return 1.0;

		float fadeOutFactor = GetFadeOutFactor(positionMS);
		float skylightingDiffuse = EvaluateDiffuse(data, evalNormal, fadeOutFactor);

		return saturate(skylightingDiffuse / max(vertexAO, EPSILON_DIVISION));
	}

	ProbeData SampleNoBias(float3 positionMS)
	{
		ProbeData data = MakeDefaultProbeData();

		if (SharedData::InInterior)
			return data;

		float3 positionMSAdjusted = positionMS - SharedData::skylightingSettings.PosOffset.xyz;
		float3 uvw = positionMSAdjusted / ARRAY_SIZE + .5;

		if (any(uvw < 0) || any(uvw > 1))
			return data;

		float3 cellVxCoord = uvw * ARRAY_DIM;
		int3 cell000 = floor(cellVxCoord - 0.5);
		float3 trilinearPos = cellVxCoord - 0.5 - cell000;

		[unroll] for (int i = 0; i < 2; i++)
			[unroll] for (int j = 0; j < 2; j++)
				[unroll] for (int k = 0; k < 2; k++)
		{
			uint n = i * 4 + j * 2 + k;
			int3 cellOffset = int3(i, j, k);
			int3 cellIdx = cell000 + cellOffset;

			if (any(cellIdx < 0) || any((uint3)cellIdx >= ARRAY_DIM))
				continue;

			float3 trilinearWeights = 1 - abs(cellOffset - trilinearPos);
			float w = trilinearWeights.x * trilinearWeights.y * trilinearWeights.z;

			uint3 cellTexID = (cellIdx + SharedData::skylightingSettings.ArrayOrigin.xyz) % ARRAY_DIM;
			SetProbe(data, n, SkylightingProbeArray[cellTexID], w);
		}

		return data;
	}

	float GetSimpleVisibility(ProbeData data)
	{
		float vis = 0;
		float wsum = 0;
		[unroll] for (uint p = 0; p < 8; p++) {
			float w = GetWeight(data, p);
			if (w > 0) {
				vis += (float(countbits(GetBitmask(data, p))) / float(NUM_DIRECTIONS)) * w;
				wsum += w;
			}
		}
		return vis / max(wsum, 0.001);
	}
#endif

}

#endif
