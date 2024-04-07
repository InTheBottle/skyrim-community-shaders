#pragma once

#include <nvapi.h>
#include "Buffer.h"

class VariableRateShading
{
public:
	static VariableRateShading* GetSingleton()
	{
		static VariableRateShading singleton;
		return &singleton;
	}

	bool enableFFR = true;

	bool nvapiLoaded = false;
	bool vrsActive = false;
	ID3D11Texture2D* singleEyeVRSTex[2];
	ID3D11UnorderedAccessView* singleEyeVRSUAV[2];
	ID3D11NvShadingRateResourceView* singleEyeVRSView[2];

	std::unordered_set<uint> screenTargets;

	Texture2D* reductionData = nullptr;
	ID3D11ComputeShader* computeNASDataCS = nullptr;
	ID3D11ComputeShader* computeShadingRateCS = nullptr;

	void ClearShaderCache();
	ID3D11ComputeShader* GetComputeNASData();
	ID3D11ComputeShader* GetComputeShadingRate();

	void DrawSettings();

	void ComputeNASData();
	void ComputeShadingRate();

	void Setup();
	void SetupSingleEyeVRS(int eye, int width, int height);
	void UpdateViews();
};