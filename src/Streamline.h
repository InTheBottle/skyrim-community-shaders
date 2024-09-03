#pragma once

#include "Buffer.h"
#include "State.h"

#pragma warning(push)
#pragma warning(disable: 5103)
#include "Streamline/include/sl.h"
#include "Streamline/include/sl_consts.h"
#include "Streamline/include/sl_matrix_helpers.h"
#include "Streamline/include/sl_dlss.h"
#include "Streamline/include/sl_dlss_g.h"
#include "Streamline/include/sl_reflex.h"
#pragma warning(pop)

class Streamline
{
public:
	static Streamline* GetSingleton()
	{
		static Streamline singleton;
		return &singleton;
	}

	bool initialized = false;

	sl::ViewportHandle viewport;
	sl::FrameToken* currentFrame;

	bool enableFrameGeneration = true;

	HMODULE interposer = NULL;

	// SL Interposer Functions
	PFun_slInit* slInit{};
	PFun_slShutdown* slShutdown{};
	PFun_slIsFeatureSupported* slIsFeatureSupported{};
	PFun_slIsFeatureLoaded* slIsFeatureLoaded{};
	PFun_slSetFeatureLoaded* slSetFeatureLoaded{};
	PFun_slEvaluateFeature* slEvaluateFeature{};
	PFun_slAllocateResources* slAllocateResources{};
	PFun_slFreeResources* slFreeResources{};
	PFun_slSetTag* slSetTag{};
	PFun_slGetFeatureRequirements* slGetFeatureRequirements{};
	PFun_slGetFeatureVersion* slGetFeatureVersion{};
	PFun_slUpgradeInterface* slUpgradeInterface{};
	PFun_slSetConstants* slSetConstants{};
	PFun_slGetNativeInterface* slGetNativeInterface{};
	PFun_slGetFeatureFunction* slGetFeatureFunction{};
	PFun_slGetNewFrameToken* slGetNewFrameToken{};
	PFun_slSetD3DDevice* slSetD3DDevice{};
	
	// DLSSG specific functions
	PFun_slDLSSGGetState* slDLSSGGetState{};
	PFun_slDLSSGSetOptions* slDLSSGSetOptions{};

	// Reflex specific functions
	PFun_slReflexGetState* slReflexGetState{};
	PFun_slReflexSetMarker* slReflexSetMarker{};
	PFun_slReflexSleep* slReflexSleep{};
	PFun_slReflexSetOptions* slReflexSetOptions{};

	Texture2D* colorBufferShared;
	Texture2D* depthBufferShared;

	ID3D11ComputeShader* copyDepthToSharedBufferCS;

	void Initialize_preDevice();
	void Initialize_postDevice();

	HRESULT CreateDXGIFactory(REFIID riid, void** ppFactory);

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
	void UpgradeGameResources();

	void CopyColorToSharedBuffer();
	void CopyDepthToSharedBuffer();

	void Present();

	void SetConstants();

	struct Main_RenderWorld
	{
		static void thunk(bool a1)
		{
			GetSingleton()->SetConstants();
			func(a1);
		}

		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct MenuManagerDrawInterfaceStartHook
	{
		static void thunk(int64_t a1)
		{
			//// Copy HUD-less texture which is automatically used to mask the UI
			//auto renderer = RE::BSGraphics::Renderer::GetSingleton();

			//auto& swapChain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];

			//auto state = State::GetSingleton();

			//auto& context = state->context;

			//ID3D11Resource* swapChainResource;
			//swapChain.SRV->GetResource(&swapChainResource);

			//state->BeginPerfEvent("HudLessColor Copy");
			//context->CopyResource(GetSingleton()->HUDLessBuffer->resource.get(), swapChainResource);
			//state->EndPerfEvent();		

			func(a1);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void InstallHooks()
	{
		stl::write_thunk_call<Main_RenderWorld>(REL::RelocationID(35560, 36559).address() + REL::Relocate(0x831, 0x841, 0x791));
		stl::write_thunk_call<MenuManagerDrawInterfaceStartHook>(REL::RelocationID(79947, 82084).address() + REL::Relocate(0x7E, 0x83, 0x17F));
	}
};
