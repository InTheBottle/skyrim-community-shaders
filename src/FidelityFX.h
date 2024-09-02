#pragma once

#include <FidelityFX/host/backends/dx11/ffx_dx11.h>
#include <FidelityFX/host/ffx_fsr3.h>
#include <FidelityFX/host/ffx_interface.h>

#include "Buffer.h"
#include "State.h"

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
	Texture2D* HUDLessColor;

	float viewScale = 0.01428222656f;

	FfxErrorCode Initialize(uint32_t a_maxContexts);
	void SetupFrameGenerationResources();
	FfxErrorCode InitializeFSR3();

	void DispatchFrameGeneration();
	void DispatchUpscaling();
	void Present(IDXGISwapChain* This, UINT SyncInterval, UINT Flags);

	struct MenuManagerDrawInterfaceStartHook
	{
		static void thunk(int64_t a1)
		{
			// Copy HUD-less texture which is automatically used to mask the UI
			if (GetSingleton()->enableFrameGeneration) {
				auto renderer = RE::BSGraphics::Renderer::GetSingleton();

				auto& swapChain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];

				auto state = State::GetSingleton();

				auto& context = state->context;

				ID3D11Resource* swapChainResource;
				swapChain.SRV->GetResource(&swapChainResource);

				state->BeginPerfEvent("FSR HudLessColor Copy");
				context->CopyResource(GetSingleton()->HUDLessColor->resource.get(), swapChainResource);
				state->EndPerfEvent();
			}

			func(a1);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void InstallHooks()
	{
		stl::write_thunk_call<MenuManagerDrawInterfaceStartHook>(REL::RelocationID(79947, 82084).address() + REL::Relocate(0x7E, 0x83, 0x17F));
	}
};
