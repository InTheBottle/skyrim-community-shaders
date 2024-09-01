#pragma once

#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <FidelityFX/host/ffx_fsr3.h>
#include <FidelityFX/host/ffx_interface.h>

#include "Buffer.h"

class FidelityFX
{
public:
	static FidelityFX* GetSingleton()
	{
		static FidelityFX singleton;
		return &singleton;
	}

	bool enableFrameGeneration = true;

	FfxInterface ffxInterface;
	FfxFsr3Context fsrContext;
	FfxCommandList dx11CommandList;

	Texture2D* swapChainPreviousTexture;
	Texture2D* swapChainPreviousTextureSwap;

	Texture2D* swapChainTempTexture;

	FfxErrorCode Initialize(uint32_t a_maxContexts);
	FfxErrorCode InitializeFSR3();

	void SetupFrameGenerationResources();
	void DispatchFrameGeneration();
	void DispatchUpscaling();
	void Present(IDXGISwapChain* This, UINT SyncInterval, UINT Flags);
};
