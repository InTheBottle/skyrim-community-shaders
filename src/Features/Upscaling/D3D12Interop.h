#pragma once

#include <cstdint>
#include <vector>

#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <winrt/base.h>

/**
 * @brief The D3D11On12 analogue of DxvkInterop.
 *
 * On the native-D3D12 (D3D11On12) path there is no Vulkan interop. Instead the game's D3D11
 * textures are unwrapped to their underlying ID3D12Resource via
 * ID3D11On12Device2::UnwrapUnderlyingResource, and Streamline's D3D12 evaluate records into a
 * CS-owned D3D12 command list on the single shared game queue (D3D11On12Loader's queue). One
 * queue for the translated game work, the unwrap flush, the SL evaluate, and present, so all
 * three serialise naturally with no cross-queue semaphores.
 *
 * Only meaningful when the game runs on the 11on12 layer; IsAvailable() stays false on native
 * D3D11 / DXVK.
 */
class D3D12Interop
{
public:
	static D3D12Interop* GetSingleton();

	/// @brief QI ID3D11On12Device2 from the game device; cache the CS D3D12 device/queue and
	///        the 11on12 immediate context. Returns false when not on the 11on12 path.
	bool Initialize();
	[[nodiscard]] bool IsAvailable() const { return available; }

	bool CreateCommandResources(uint32_t a_framesInFlight = kFrames);
	[[nodiscard]] bool CommandResourcesReady() const { return fence != nullptr; }
	void DestroyCommandResources();

	/// @brief Flush the 11on12 immediate context so queued D3D11 work reaches the shared
	///        queue before SL records compute (unwrap already flushes per-resource; this is
	///        the global belt-and-suspenders flush called once at the top of an evaluate).
	void FlushImmediateContext();

	/// @brief Unwrap a game D3D11 texture to its underlying ID3D12Resource (returned in
	///        D3D12_RESOURCE_STATE_COMMON, with an owning ref the caller must return).
	bool UnwrapResource(ID3D11Resource* a_res11, ID3D12Resource** a_out12);

	/// @brief Begin this frame's eval command list: wait the slot fence, release the prior
	///        slot's held resources, reset the allocator + list. Null on failure.
	ID3D12GraphicsCommandList* BeginFrameCommandList();

	/// @brief Close + execute the eval list on the shared queue, signal the slot fence, and
	///        issue ReturnUnderlyingResource for the batch (holding the unwrapped refs until
	///        the fence signals). a_return11[i] corresponds to a_return12[i].
	void SubmitAndReturn(ID3D12GraphicsCommandList* a_list, ID3D11Resource* const* a_return11,
		ID3D12Resource* const* a_return12, uint32_t a_count, bool a_waitIdle = false);

	/// @brief Abort path: no eval work was submitted, so return each unwrapped resource with
	///        no sync and release its ref immediately.
	void CancelAndReturn(ID3D11Resource* const* a_return11, ID3D12Resource* const* a_return12, uint32_t a_count);

	/// @brief Block until the most recent submission's GPU work has completed (present-hook
	///        bound under DLSS-G; the 11on12 analogue of DxvkInterop::WaitLastSubmission).
	void WaitLastSubmission();

private:
	static constexpr uint32_t kFrames = 3;

	bool available = false;
	winrt::com_ptr<ID3D11On12Device2> dev11on12;
	ID3D12Device* device12 = nullptr;   ///< owned by D3D11On12Loader (not addref'd)
	ID3D12CommandQueue* queue = nullptr; ///< owned by D3D11On12Loader (the single game queue)
	ID3D11DeviceContext* immediate = nullptr;

	winrt::com_ptr<ID3D12CommandAllocator> allocators[kFrames];
	winrt::com_ptr<ID3D12GraphicsCommandList> lists[kFrames];
	winrt::com_ptr<ID3D12Fence> fence;
	uint64_t fenceValues[kFrames] = {};
	uint64_t lastSignaled = 0;
	void* fenceEvent = nullptr;
	uint32_t frameIndex = 0;
	std::vector<winrt::com_ptr<ID3D12Resource>> pendingReturns[kFrames];
};
