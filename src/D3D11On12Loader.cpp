#include "D3D11On12Loader.h"

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
			HRESULT hr = D3D12CreateDevice(a_adapter, D3D_FEATURE_LEVEL_11_1, IID_PPV_ARGS(&d3d12Device));
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
