#pragma once

#include <d3d11.h>

/**
 * @brief Boots the game's D3D11 on a CS-owned native D3D12 device via the embedded
 *        D3D11On12 mapping layer (extern/D3D11On12, linked as a static library).
 *
 * The system d3d11.dll runtime provides the D3D11 API front-end; its 11on12 driver DDI
 * (`OpenAdapter_D3D11On12`) is exported by CommunityShaders.dll itself, and a
 * LoadLibrary redirect makes the runtime's LoadLibrary("d3d11on12.dll") resolve to this
 * module. See docs/development/d3d11on12-port.md.
 *
 * Bring-up gate: env CS_D3D11ON12=1 selects this path; otherwise the DXVK path loads.
 */
namespace D3D11On12Loader
{
	/// @brief True when env CS_D3D11ON12=1 requests the 11on12 path.
	bool IsRequested();

	/// @brief Installs the LoadLibrary redirect and resolves system entry points.
	///        Must run before the game creates its device. Returns false on failure
	///        (caller falls back to the DXVK path).
	bool Load();

	/// @brief True after a successful Load().
	bool IsLoaded();

	/// @brief Drop-in replacement for D3D11CreateDeviceAndSwapChain: creates the D3D12
	///        device + direct queue, the 11on12 D3D11 device on top, and a native
	///        flip-model DXGI swapchain on the queue.
	PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN GetD3D11CreateDeviceAndSwapChain();

	/// @brief The CS-owned D3D12 device/queue (null until the device exists).
	void* GetD3D12Device();
	void* GetD3D12Queue();
}
