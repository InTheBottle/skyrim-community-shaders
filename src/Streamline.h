#pragma once

#include "Buffer.h"
#include "State.h"

#pragma warning(push)
#pragma warning(disable: 5103)
#include "Streamline/include/sl.h"
#include "Streamline/include/sl_consts.h"
#include "Streamline/include/sl_dlss_g.h"
#pragma warning(pop)

class Streamline
{
public:
	static Streamline* GetSingleton()
	{
		static Streamline singleton;
		return &singleton;
	}

	sl::ViewportHandle viewport;

	Texture2D* HUDLessBuffer;

	void Initialize();

	HRESULT CreateSwapchain(IDXGIAdapter* pAdapter,
		D3D_DRIVER_TYPE DriverType,
		HMODULE Software,
		UINT Flags,
		const D3D_FEATURE_LEVEL* pFeatureLevels,
		UINT FeatureLevels,
		UINT SDKVersion,
		const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
		IDXGISwapChain** ppSwapChain,
		ID3D11Device** ppDevice,
		D3D_FEATURE_LEVEL* pFeatureLevel,
		ID3D11DeviceContext** ppImmediateContext);

	void SetupFrameGenerationResources();

	void UpgradeGameResource(RE::RENDER_TARGET a_target);

	void UpgradeGameResourceDepth(RE::RENDER_TARGET_DEPTHSTENCIL a_target);

	void UpgradeGameResources();

	void SetTags();

	void SetConstants();

	struct MenuManagerDrawInterfaceStartHook
	{
		static void thunk(int64_t a1)
		{
			// Copy HUD-less texture which is automatically used to mask the UI
			auto renderer = RE::BSGraphics::Renderer::GetSingleton();

			auto& swapChain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];

			auto state = State::GetSingleton();

			auto& context = state->context;

			ID3D11Resource* swapChainResource;
			swapChain.SRV->GetResource(&swapChainResource);

			state->BeginPerfEvent("HudLessColor Copy");
			context->CopyResource(GetSingleton()->HUDLessBuffer->resource.get(), swapChainResource);
			state->EndPerfEvent();		

			func(a1);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void InstallHooks()
	{
		stl::write_thunk_call<MenuManagerDrawInterfaceStartHook>(REL::RelocationID(79947, 82084).address() + REL::Relocate(0x7E, 0x83, 0x17F));
	}
};
