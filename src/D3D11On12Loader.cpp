#include "D3D11On12Loader.h"

#include "Features/Upscaling/Streamline.h"

#include <d3d11on12.h>
#include <d3d12.h>
#include <detours/detours.h>
#include <dxgi1_4.h>

// The DDI entry the static D3D11On12 layer defines (see extern/D3D11On12 src/main.cpp).
// Referencing it here both pulls the object out of the static lib and re-exports it from
// CommunityShaders.dll so the runtime's GetProcAddress finds it on our module.
extern "C" HRESULT APIENTRY OpenAdapter_D3D11On12(void* pArgs);
#pragma comment(linker, "/export:OpenAdapter_D3D11On12")

namespace D3D11On12Loader
{
	namespace
	{
		bool loaded = false;
		ID3D12Device* d3d12Device = nullptr;
		ID3D12CommandQueue* d3d12Queue = nullptr;

		decltype(&LoadLibraryExW) realLoadLibraryExW = LoadLibraryExW;

		// The system d3d11.dll runtime resolves its 11on12 driver with
		// LoadLibraryExW(L"d3d11on12.dll"); hand it CommunityShaders.dll instead — the
		// layer is linked in and OpenAdapter_D3D11On12 is exported above.
		HMODULE WINAPI hk_LoadLibraryExW(LPCWSTR a_name, HANDLE a_file, DWORD a_flags)
		{
			if (a_name && (_wcsicmp(a_name, L"d3d11on12.dll") == 0 ||
							  _wcsicmp(a_name, L"C:\\Windows\\System32\\d3d11on12.dll") == 0)) {
				HMODULE self = nullptr;
				GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
					reinterpret_cast<LPCWSTR>(&hk_LoadLibraryExW), &self);
				logger::info("[D3D11On12] runtime asked for d3d11on12.dll - serving the embedded layer");
				return self;
			}
			return realLoadLibraryExW(a_name, a_file, a_flags);
		}

		HRESULT WINAPI CreateDeviceAndSwapChainShim(
			IDXGIAdapter* a_adapter,
			D3D_DRIVER_TYPE,
			HMODULE,
			UINT a_flags,
			const D3D_FEATURE_LEVEL* a_featureLevels,
			UINT a_featureLevelCount,
			UINT a_sdkVersion,
			const DXGI_SWAP_CHAIN_DESC* a_swapChainDesc,
			IDXGISwapChain** o_swapChain,
			ID3D11Device** o_device,
			D3D_FEATURE_LEVEL* o_featureLevel,
			ID3D11DeviceContext** o_context)
		{
			// Streamline D3D12, automatic interposition: slInit first, then create the
			// device and factory THROUGH sl.interposer.dll's drop-in exports — SL registers
			// the device and returns proxied factory/swapchain (sl.dlss_g owns present).
			// Every step degrades gracefully: SL absent -> system entry points, the game
			// still boots, just without DLSS/Reflex/FG.
			auto* streamline = Streamline::GetSingleton();
			streamline->InitializeD3D12();

			auto d3d12CreateDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(
				streamline->GetInterposerProc("D3D12CreateDevice"));
			if (!d3d12CreateDevice)
				d3d12CreateDevice = &D3D12CreateDevice;

			HRESULT hr = d3d12CreateDevice(a_adapter, D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(&d3d12Device));
			if (FAILED(hr)) {
				logger::critical("[D3D11On12] D3D12CreateDevice failed ({:X})", (uint32_t)hr);
				return hr;
			}

			D3D12_COMMAND_QUEUE_DESC queueDesc{};
			queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			if (FAILED(hr = d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&d3d12Queue)))) {
				logger::critical("[D3D11On12] CreateCommandQueue failed ({:X})", (uint32_t)hr);
				return hr;
			}
			d3d12Queue->SetName(L"CS::GameQueue");

			// SL 2.12.0's D3D12CreateDevice proxy does not self-register the device (its
			// plugin init logs "did you forget to call slSetD3DDevice"): register explicitly.
			if (auto slSetD3DDevice = reinterpret_cast<int (*)(void*)>(streamline->GetInterposerProc("slSetD3DDevice"))) {
				const int res = slSetD3DDevice(d3d12Device);
				logger::info("[D3D11On12] slSetD3DDevice: {}", res == 0 ? "ok" : "FAILED");
			}

			// The system runtime builds the D3D11 device; its driver DDI resolves to the
			// embedded layer through the LoadLibrary redirect installed in Load().
			IUnknown* queues[] = { d3d12Queue };
			hr = D3D11On12CreateDevice(d3d12Device, a_flags, a_featureLevels, a_featureLevelCount,
				queues, 1, 0, o_device, o_context, o_featureLevel);
			if (FAILED(hr)) {
				logger::critical("[D3D11On12] D3D11On12CreateDevice failed ({:X})", (uint32_t)hr);
				return hr;
			}

			if (a_swapChainDesc && o_swapChain) {
				// The game's ORIGINAL swapchain desc, created on the 11on12 D3D11 DEVICE —
				// DXGI resolves the underlying D3D12 queue itself and provides the blt-model
				// semantics Skyrim's renderer init depends on (GetBuffer/RTV behavior). A
				// hand-converted flip-model chain on the raw queue broke those expectations
				// (null-deref in Renderer::Init on the first boot).
				// SYSTEM factory on purpose: SL's proxied factory faults when CreateSwapChain
				// receives the 11on12 D3D11 device (sl.dlss_g expects queue-based D3D12
				// chains - crash in sl.dlss_g->sl.common->d3d11). Create the blt chain
				// natively (the proven boot shape), then hand the FINISHED swapchain to SL
				// below via slUpgradeInterface for present ownership.
				IDXGIFactory4* factory = nullptr;
				if (FAILED(hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
					return hr;

				DXGI_SWAP_CHAIN_DESC desc = *a_swapChainDesc;
				IDXGISwapChain* swapChain = nullptr;
				hr = factory->CreateSwapChain(*o_device, &desc, &swapChain);
				if (FAILED(hr)) {
					logger::warn("[D3D11On12] CreateSwapChain(device, original desc) failed ({:X}) - retrying on the queue", (uint32_t)hr);
					hr = factory->CreateSwapChain(d3d12Queue, &desc, &swapChain);
				}
				factory->Release();
				if (FAILED(hr)) {
					logger::critical("[D3D11On12] CreateSwapChain failed ({:X})", (uint32_t)hr);
					return hr;
				}
				// Present ownership: upgrade the finished swapchain so sl.dlss_g's proxy
				// wraps it (manual-hooking flow; IDXGISwapChain is a documented upgrade
				// target). Failure leaves the native chain - game still runs, no FG.
				if (auto slUpgradeInterface = reinterpret_cast<int (*)(void**)>(streamline->GetInterposerProc("slUpgradeInterface"))) {
					const int res = slUpgradeInterface(reinterpret_cast<void**>(&swapChain));
					logger::info("[D3D11On12] slUpgradeInterface(swapchain): {}", res == 0 ? "proxied" : "declined");
				}
				*o_swapChain = swapChain;
			}

			(void)a_sdkVersion;
			logger::info("[D3D11On12] game device online: D3D11 on native D3D12 (embedded layer)");
			return S_OK;
		}
	}

	bool IsRequested()
	{
		char buf[8]{};
		return GetEnvironmentVariableA("CS_D3D11ON12", buf, sizeof(buf)) && buf[0] == '1';
	}

	bool Load()
	{
		if (loaded)
			return true;

		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(&reinterpret_cast<PVOID&>(realLoadLibraryExW), hk_LoadLibraryExW);
		if (DetourTransactionCommit() != NO_ERROR) {
			logger::critical("[D3D11On12] LoadLibrary redirect failed");
			return false;
		}

		loaded = true;
		logger::info("[D3D11On12] embedded mapping layer armed (CS_D3D11ON12=1)");
		return true;
	}

	bool IsLoaded()
	{
		return loaded;
	}

	PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN GetD3D11CreateDeviceAndSwapChain()
	{
		return &CreateDeviceAndSwapChainShim;
	}

	void* GetD3D12Device() { return d3d12Device; }
	void* GetD3D12Queue() { return d3d12Queue; }
}
