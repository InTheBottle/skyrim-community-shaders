#pragma once

#include <nvapi.h>

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

	void DrawSettings();

	void Setup();
	void SetupSingleEyeVRS(int eye, int width, int height);
	void UpdateViews();
};