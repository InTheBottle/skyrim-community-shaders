#pragma once

#include <FidelityFX/host/ffx_interface.h>
#include <FidelityFX/host/ffx_fsr3.h>
#include <FidelityFX/host/backends/dx11/ffx_dx11.h>

class FidelityFX
{
public:
	static FidelityFX* GetSingleton()
	{
		static FidelityFX singleton;
		return &singleton;
	}

	FfxInterface ffxInterface;
	FfxFsr3Context fsrContext;

	FfxErrorCode Initialize(uint32_t a_maxContexts);
	FfxErrorCode InitializeFSR3();
};
