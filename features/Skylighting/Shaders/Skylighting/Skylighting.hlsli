#ifndef SKYLIGHTING_INCLUDE
#define SKYLIGHTING_INCLUDE

#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

cbuffer SkylightingCB : register(b1)
{
	row_major float4x4 SL_OcclusionViewProj;
	float4 SL_OcclusionDir;

	float4 SL_PosOffset;
	uint4 SL_ArrayOrigin;
	uint4 SL_ValidID0;
	uint4 SL_ValidID1;

	float4 SL_MixParams;  // x: min diffuse visibility, y: diffuse mult, z: min specular visibility, w: specular mult

	uint4 SL_DoOcclusion;
};

const static uint3 SKYLIGHTING_ARRAY_DIM = uint3(128, 128, 64);
const static float3 SKYLIGHTING_ARRAY_SIZE = float3(8192, 8192, 8192 * 0.5);
const static float3 SKYLIGHTING_CELL_SIZE = SKYLIGHTING_ARRAY_SIZE / SKYLIGHTING_ARRAY_DIM;

sh2 sampleSkylighting(Texture3D<sh2> probeArray, float3 positionMS, float3 normalWS)
{
	float3 positionMSAdjusted = positionMS - SL_PosOffset;
	float3 uvw = positionMSAdjusted / SKYLIGHTING_ARRAY_SIZE + .5;

	if (any(uvw < 0) || any(uvw > 1))
		return 1;

	float3 cellVxCoord = uvw * SKYLIGHTING_ARRAY_DIM;
	int3 cell000 = floor(cellVxCoord - 0.5);
	float3 trilinearPos = cellVxCoord - 0.5 - cell000;

	sh2 sum = 0;
	float wsum = 0;
	for (int i = 0; i < 2; i++)
		for (int j = 0; j < 2; j++)
			for (int k = 0; k < 2; k++) {
				int3 offset = int3(i, j, k);
				int3 cellID = cell000 + offset;

				if (any(cellID < 0) || any(cellID >= SKYLIGHTING_ARRAY_DIM))
					continue;

				float3 cellCentreMS = cellID + 0.5 - SKYLIGHTING_ARRAY_DIM / 2;
				cellCentreMS = cellCentreMS * SKYLIGHTING_CELL_SIZE;

				// https://handmade.network/p/75/monter/blog/p/7288-engine_work__global_illumination_with_irradiance_probes
				// basic tangent checks
				if (dot(cellCentreMS - positionMSAdjusted, normalWS) <= 0)
					continue;

				float3 trilinearWeights = 1 - abs(offset - trilinearPos);
				float w = trilinearWeights.x * trilinearWeights.y * trilinearWeights.z;

				uint3 cellTexID = (cellID + SL_ArrayOrigin.xyz) % SKYLIGHTING_ARRAY_DIM;
				sh2 probe = shScale(probeArray[cellTexID], w);

				sum = shAdd(sum, probe);
				wsum += w;
			}

	sh2 result = shScale(sum, rcp(wsum + 1e-10));

	return result;
}

// http://torust.me/ZH3.pdf
// ZH hallucination that makes skylighting more directional
// skipped luminance because it's single channel
float shHallucinateZH3Irradiance(sh2 sh, float3 normal)
{
	const static float factor = sqrt(5.0f / (16.0f * 3.1415926f));

	float3 zonalAxis = normalize(sh.wyz);
	float ratio = abs(dot(sh.wyz, zonalAxis)) / sh.x;
	float3 zonalL2Coeff = sh.x * (0.08f * ratio + 0.6f * ratio * ratio);

	float fZ = dot(zonalAxis, normal);
	float zhDir = factor * (3.0f * fZ * fZ - 1.0f);

	float result = shFuncProductIntegral(sh, shEvaluateCosineLobe(normal));

	result += 0.25f * zonalL2Coeff * zhDir;

	return saturate(result);
}

// https://www.gdcvault.com/play/1026701/Fast-Denoising-With-Self-Stabilizing
float3 getSpecularDominantDirection(float3 N, float3 V, float roughness)
{
	float f = (1 - roughness) * (sqrt(1 - roughness) + roughness);
	float3 R = reflect(-V, N);
	float3 D = lerp(N, R, f);

	return normalize(D);
}

sh2 fauxSpecularLobeSH(float3 N, float3 V, float roughness)
{
	// https://www.gdcvault.com/play/1026701/Fast-Denoising-With-Self-Stabilizing
	// get dominant ggx reflection direction
	float f = (1 - roughness) * (sqrt(1 - roughness) + roughness);
	float3 R = reflect(-V, N);
	float3 D = lerp(N, R, f);
	float3 dominantDir = normalize(D);

	sh2 directional = shEvaluate(dominantDir);
	sh2 cosineLobe = shEvaluateCosineLobe(dominantDir);
	sh2 result = shAdd(shScale(directional, f), shScale(cosineLobe, 1 - f));

	return result;
}

#endif