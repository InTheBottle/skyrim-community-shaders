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

		// The D3D11 runtime resolves its 11on12 driver as a DELAY-LOAD import of
		// d3d11on12.dll!OpenAdapter_D3D11On12 inside d3d11.dll — NOT via LoadLibrary or
		// LdrLoadDll (which is why the earlier exported-entry detours never fired). Pre-write
		// our embedded OpenAdapter symbol into d3d11.dll's delay-import IAT slot: the call
		// site is `call [__imp_OpenAdapter_D3D11On12]`, so a resolved pointer short-circuits
		// LdrResolveDelayLoadedAPI entirely and the system d3d11on12.dll is never loaded.
		bool PatchDelayLoadedD3D11On12()
		{
			HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
			if (!d3d11)
				d3d11 = LoadLibraryW(L"d3d11.dll");  // CS statically imports d3d11, so this just bumps the ref
			if (!d3d11)
				return false;

			auto* base = reinterpret_cast<BYTE*>(d3d11);
			auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
			auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
			if (!dir.VirtualAddress || !dir.Size)
				return false;

			for (auto* desc = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dir.VirtualAddress);
				 desc->DllNameRVA; ++desc) {
				if (!desc->Attributes.RvaBased)  // modern Windows delay descriptors are RVA-based
					continue;
				const char* dllName = reinterpret_cast<const char*>(base + desc->DllNameRVA);
				if (_stricmp(dllName, "d3d11on12.dll") != 0)
					continue;

				auto* iat = reinterpret_cast<void**>(base + desc->ImportAddressTableRVA);
				auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->ImportNameTableRVA);
				for (size_t i = 0; names[i].u1.AddressOfData; ++i) {
					if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG)
						continue;
					auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
					if (strcmp(ibn->Name, "OpenAdapter_D3D11On12") != 0)
						continue;

					DWORD oldProt = 0;
					if (!VirtualProtect(&iat[i], sizeof(void*), PAGE_READWRITE, &oldProt))
						return false;
					iat[i] = reinterpret_cast<void*>(&OpenAdapter_D3D11On12);
					VirtualProtect(&iat[i], sizeof(void*), oldProt, &oldProt);
					logger::info("[D3D11On12] patched d3d11.dll delay-import slot -> embedded OpenAdapter_D3D11On12");
					return true;
				}
			}
			logger::warn("[D3D11On12] d3d11on12.dll delay-import slot not found in d3d11.dll");
			return false;
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

		// Serve OUR embedded layer by patching d3d11.dll's delay-import slot for
		// OpenAdapter_D3D11On12 (must run before the shim's first D3D11On12CreateDevice
		// triggers the delay resolve; Load() runs in InstallEarlyHooks, well before).
		// Gated on CS_D3D11ON12_EMBED so the D3D12 device/SL/evaluate path — which works
		// identically on the SYSTEM d3d11on12.dll (its ID3D11On12Device2 also exposes
		// UnwrapUnderlyingResource) — can be validated on the proven system layer first.
		char embed[8]{};
		if (GetEnvironmentVariableA("CS_D3D11ON12_EMBED", embed, sizeof(embed)) && embed[0] == '1') {
			if (!PatchDelayLoadedD3D11On12())
				logger::warn("[D3D11On12] delay-import patch failed - the SYSTEM d3d11on12.dll will serve the DDI");
		} else {
			logger::info("[D3D11On12] using the SYSTEM d3d11on12.dll (set CS_D3D11ON12_EMBED=1 for the embedded layer)");
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
