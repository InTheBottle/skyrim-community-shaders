#include "D3D12Interop.h"

#include "../../D3D11On12Loader.h"
#include "../../Globals.h"

D3D12Interop* D3D12Interop::GetSingleton()
{
	static D3D12Interop singleton;
	return &singleton;
}

bool D3D12Interop::Initialize()
{
	if (available)
		return true;
	if (!D3D11On12Loader::IsLoaded())
		return false;  // native D3D11 / DXVK path — no 11on12 interop

	device12 = static_cast<ID3D12Device*>(D3D11On12Loader::GetD3D12Device());
	queue = static_cast<ID3D12CommandQueue*>(D3D11On12Loader::GetD3D12Queue());
	immediate = globals::d3d::context;
	if (!device12 || !queue || !globals::d3d::device) {
		logger::warn("[D3D12Interop] missing D3D12 device/queue - 11on12 interop unavailable");
		return false;
	}

	// The game device is an 11on12 device; QI its ID3D11On12Device2 for the unwrap API.
	if (FAILED(globals::d3d::device->QueryInterface(__uuidof(ID3D11On12Device2), dev11on12.put_void()))) {
		logger::warn("[D3D12Interop] game device is not an ID3D11On12Device2 - cannot unwrap resources");
		return false;
	}

	available = true;
	logger::info("[D3D12Interop] initialized (native D3D12 evaluate path)");
	return true;
}

bool D3D12Interop::CreateCommandResources(uint32_t)
{
	if (!available || fence)
		return fence != nullptr;

	for (uint32_t i = 0; i < kFrames; ++i) {
		if (FAILED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocators[i].put())))) {
			logger::error("[D3D12Interop] CreateCommandAllocator {} failed", i);
			return false;
		}
		if (FAILED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].get(),
				nullptr, IID_PPV_ARGS(lists[i].put())))) {
			logger::error("[D3D12Interop] CreateCommandList {} failed", i);
			return false;
		}
		lists[i]->Close();  // start closed; BeginFrameCommandList resets before use
		allocators[i]->SetName(L"D3D12Interop::EvalAllocator");
		lists[i]->SetName(L"D3D12Interop::EvalCmdList");
	}

	if (FAILED(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())))) {
		logger::error("[D3D12Interop] CreateFence failed");
		return false;
	}
	fence->SetName(L"D3D12Interop::EvalFence");
	fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	frameIndex = 0;
	logger::info("[D3D12Interop] command ring ready ({} frames)", kFrames);
	return true;
}

void D3D12Interop::DestroyCommandResources()
{
	if (fence && fenceEvent) {
		// Drain: wait the highest signaled value so no held resource is freed under the GPU.
		if (fence->GetCompletedValue() < lastSignaled) {
			fence->SetEventOnCompletion(lastSignaled, fenceEvent);
			WaitForSingleObject(fenceEvent, INFINITE);
		}
	}
	for (uint32_t i = 0; i < kFrames; ++i) {
		pendingReturns[i].clear();
		lists[i] = nullptr;
		allocators[i] = nullptr;
		fenceValues[i] = 0;
	}
	fence = nullptr;
	if (fenceEvent) {
		CloseHandle(fenceEvent);
		fenceEvent = nullptr;
	}
	lastSignaled = 0;
	frameIndex = 0;
}

void D3D12Interop::FlushImmediateContext()
{
	if (immediate)
		immediate->Flush();
}

bool D3D12Interop::UnwrapResource(ID3D11Resource* a_res11, ID3D12Resource** a_out12)
{
	if (!available || !a_res11 || !a_out12)
		return false;
	// Flushes the immediate context's outstanding GPU work touching a_res11 onto the queue
	// and hands back the underlying ID3D12Resource in D3D12_RESOURCE_STATE_COMMON.
	return SUCCEEDED(dev11on12->UnwrapUnderlyingResource(a_res11, queue, IID_PPV_ARGS(a_out12)));
}

ID3D12GraphicsCommandList* D3D12Interop::BeginFrameCommandList()
{
	if (!CommandResourcesReady())
		return nullptr;

	frameIndex = (frameIndex + 1) % kFrames;
	const uint32_t slot = frameIndex;

	if (fence->GetCompletedValue() < fenceValues[slot] && fenceEvent) {
		fence->SetEventOnCompletion(fenceValues[slot], fenceEvent);
		WaitForSingleObject(fenceEvent, INFINITE);
	}
	// GPU is done with this slot: releasing the held unwrapped resources is now safe.
	pendingReturns[slot].clear();

	if (FAILED(allocators[slot]->Reset()) || FAILED(lists[slot]->Reset(allocators[slot].get(), nullptr)))
		return nullptr;
	return lists[slot].get();
}

void D3D12Interop::SubmitAndReturn(ID3D12GraphicsCommandList* a_list, ID3D11Resource* const* a_return11,
	ID3D12Resource* const* a_return12, uint32_t a_count, bool a_waitIdle)
{
	if (!a_list || !CommandResourcesReady())
		return;
	const uint32_t slot = frameIndex;

	a_list->Close();
	ID3D12CommandList* lists12[] = { a_list };
	queue->ExecuteCommandLists(1, lists12);

	const uint64_t v = ++lastSignaled;
	queue->Signal(fence.get(), v);
	fenceValues[slot] = v;

	// Hand each unwrapped resource back to the 11on12 device, gated on this submission's
	// fence value (it re-establishes 11on12 ownership only after the GPU finishes reading).
	ID3D12Fence* f = fence.get();
	uint64_t signalValue = v;
	for (uint32_t i = 0; i < a_count; ++i) {
		if (a_return11[i])
			dev11on12->ReturnUnderlyingResource(a_return11[i], 1, &signalValue, &f);
		if (a_return12[i]) {
			// Hold the owning ref until the slot fence signals (BeginFrameCommandList frees it).
			winrt::com_ptr<ID3D12Resource> held;
			held.attach(a_return12[i]);  // adopt the ref UnwrapUnderlyingResource added
			pendingReturns[slot].push_back(std::move(held));
		}
	}

	if (a_waitIdle && fenceEvent && fence->GetCompletedValue() < v) {
		fence->SetEventOnCompletion(v, fenceEvent);
		WaitForSingleObject(fenceEvent, INFINITE);
	}
}

void D3D12Interop::CancelAndReturn(ID3D11Resource* const* a_return11, ID3D12Resource* const* a_return12, uint32_t a_count)
{
	for (uint32_t i = 0; i < a_count; ++i) {
		if (a_return11[i])
			dev11on12->ReturnUnderlyingResource(a_return11[i], 0, nullptr, nullptr);  // no pending GPU work
		if (a_return12[i])
			a_return12[i]->Release();  // adopt + drop the unwrap ref
	}
}

void D3D12Interop::WaitLastSubmission()
{
	if (fence && fenceEvent && fence->GetCompletedValue() < lastSignaled) {
		fence->SetEventOnCompletion(lastSignaled, fenceEvent);
		WaitForSingleObject(fenceEvent, INFINITE);
	}
}
