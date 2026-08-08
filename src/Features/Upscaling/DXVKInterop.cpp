#include "DXVKInterop.h"

#include "Globals.h"

DXVKInterop* DXVKInterop::GetSingleton()
{
	static DXVKInterop singleton;
	return &singleton;
}

bool DXVKInterop::Initialize()
{
	if (available)
		return true;

	auto d3dDevice = globals::d3d::device;
	if (!d3dDevice) {
		logger::warn("[DXVKInterop] No D3D11 device available yet");
		return false;
	}

	winrt::com_ptr<IDXGIVkInteropDevice> dev;
	if (FAILED(d3dDevice->QueryInterface(__uuidof(IDXGIVkInteropDevice), dev.put_void()))) {
		logger::info("[DXVKInterop] IDXGIVkInteropDevice not present — not running under DXVK");
		return false;
	}

	interopDevice = dev;
	interopDevice->GetVulkanHandles(&instance, &physicalDevice, &device);
	interopDevice->GetSubmissionQueue(&queue, &queueFamilyIndex);

	if (!instance || !physicalDevice || !device || !queue) {
		logger::error("[DXVKInterop] DXVK returned null Vulkan handles");
		interopDevice = nullptr;
		return false;
	}

	if (HMODULE vk = GetModuleHandleW(L"vulkan-1.dll")) {
		vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
			reinterpret_cast<void*>(GetProcAddress(vk, "vkGetInstanceProcAddr")));
	}
	if (!vkGetInstanceProcAddr) {
		logger::error("[DXVKInterop] Could not resolve vkGetInstanceProcAddr from vulkan-1.dll");
		interopDevice = nullptr;
		return false;
	}

	vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
		vkGetInstanceProcAddr(instance, "vkGetDeviceProcAddr"));
	if (!vkGetDeviceProcAddr) {
		logger::error("[DXVKInterop] Could not resolve vkGetDeviceProcAddr");
		interopDevice = nullptr;
		return false;
	}

	vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(
		vkGetDeviceProcAddr(device, "vkDestroyImageView"));

	if (auto pfnProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
			vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties"))) {
		VkPhysicalDeviceProperties props{};
		pfnProps(physicalDevice, &props);
		logger::info("[DXVKInterop] Bridged to DXVK Vulkan device: '{}' (API {}.{}.{}), queueFamily {}",
			props.deviceName,
			VK_API_VERSION_MAJOR(props.apiVersion),
			VK_API_VERSION_MINOR(props.apiVersion),
			VK_API_VERSION_PATCH(props.apiVersion),
			queueFamilyIndex);
	} else {
		logger::info("[DXVKInterop] Bridged to DXVK Vulkan device (queueFamily {})", queueFamilyIndex);
	}

	available = true;
	return true;
}

bool DXVKInterop::GetVkImage(ID3D11Resource* a_resource, VkImage* a_outImage,
	VkImageLayout* a_outLayout, VkImageCreateInfo* a_outInfo) const
{
	if (!available || !a_resource)
		return false;

	winrt::com_ptr<IDXGIVkInteropSurface> surface;
	if (FAILED(a_resource->QueryInterface(__uuidof(IDXGIVkInteropSurface), surface.put_void())))
		return false;

	VkImageCreateInfo localInfo{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	VkImageCreateInfo* info = a_outInfo ? a_outInfo : &localInfo;
	info->sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	info->pNext = nullptr;
	info->queueFamilyIndexCount = 0;
	info->pQueueFamilyIndices = nullptr;

	return SUCCEEDED(surface->GetVulkanImageInfo(a_outImage, a_outLayout, info));
}

void DXVKInterop::FlushRenderingCommands() const
{
	if (interopDevice)
		interopDevice->FlushRenderingCommands();
}

void DXVKInterop::WaitDeviceIdle() const
{
	if (!interopDevice)
		return;
	// vkDeviceWaitIdle would race DXVK's submission thread; drain under DXVK's queue lock.
	FlushRenderingCommands();
	LockSubmissionQueue();
	ReleaseSubmissionQueue();
}

void DXVKInterop::LockSubmissionQueue() const
{
	if (interopDevice)
		interopDevice->LockSubmissionQueue();
}

void DXVKInterop::ReleaseSubmissionQueue() const
{
	if (interopDevice)
		interopDevice->ReleaseSubmissionQueue();
}

bool DXVKInterop::CreateCommandResources(uint32_t a_framesInFlight)
{
	if (!available)
		return false;
	if (commandPool != VK_NULL_HANDLE)
		return true;

	framesInFlight = a_framesInFlight ? a_framesInFlight : 1;

	VkCommandPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = queueFamilyIndex;
	if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
		logger::error("[DXVKInterop] vkCreateCommandPool failed");
		commandPool = VK_NULL_HANDLE;
		return false;
	}

	commandBuffers.resize(framesInFlight, VK_NULL_HANDLE);
	VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	allocInfo.commandPool = commandPool;
	allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocInfo.commandBufferCount = framesInFlight;
	if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
		logger::error("[DXVKInterop] vkAllocateCommandBuffers failed");
		DestroyCommandResources();
		return false;
	}

	// Fences start signaled so the first BeginFrameCommandBuffer doesn't block.
	commandFences.resize(framesInFlight, VK_NULL_HANDLE);
	VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	for (uint32_t i = 0; i < framesInFlight; ++i) {
		if (vkCreateFence(device, &fenceInfo, nullptr, &commandFences[i]) != VK_SUCCESS) {
			logger::error("[DXVKInterop] vkCreateFence failed");
			DestroyCommandResources();
			return false;
		}
	}

	commandFrameIndex = 0;
	pendingViewDeletes.assign(framesInFlight, {});
	logger::info("[DXVKInterop] Command ring created ({} frames in flight, queueFamily {})", framesInFlight, queueFamilyIndex);
	return true;
}

void DXVKInterop::DestroyCommandResources()
{
	if (device == VK_NULL_HANDLE)
		return;

	for (auto f : commandFences) {
		if (f != VK_NULL_HANDLE)
			vkWaitForFences(device, 1, &f, VK_TRUE, UINT64_MAX);
	}
	if (vkDestroyImageView) {
		for (auto& slot : pendingViewDeletes)
			for (VkImageView v : slot)
				if (v != VK_NULL_HANDLE)
					vkDestroyImageView(device, v, nullptr);
	}
	pendingViewDeletes.clear();
	for (auto f : commandFences) {
		if (f != VK_NULL_HANDLE)
			vkDestroyFence(device, f, nullptr);
	}
	commandFences.clear();

	if (commandPool != VK_NULL_HANDLE) {
		vkDestroyCommandPool(device, commandPool, nullptr);
		commandPool = VK_NULL_HANDLE;
	}
	commandBuffers.clear();
	framesInFlight = 0;
	commandFrameIndex = 0;
}

void DXVKInterop::DrainCommandRing()
{
	if (commandPool == VK_NULL_HANDLE || device == VK_NULL_HANDLE)
		return;

	// Leave fences signaled; BeginFrameCommandBuffer resets them on reuse.
	if (!commandFences.empty())
		vkWaitForFences(device, static_cast<uint32_t>(commandFences.size()), commandFences.data(), VK_TRUE, UINT64_MAX);

	if (vkDestroyImageView) {
		for (auto& slot : pendingViewDeletes) {
			for (VkImageView v : slot)
				if (v != VK_NULL_HANDLE)
					vkDestroyImageView(device, v, nullptr);
			slot.clear();
		}
	}
}

VkCommandBuffer DXVKInterop::BeginFrameCommandBuffer()
{
	if (commandPool == VK_NULL_HANDLE)
		return VK_NULL_HANDLE;

	// Avoid waiting while Streamline owns the presenting queue; grow the ring if needed.
	constexpr uint32_t kMaxRingDepth = 64;
	uint32_t next = (commandFrameIndex + 1) % framesInFlight;
	if (vkGetFenceStatus(device, commandFences[next]) != VK_SUCCESS) {
		uint32_t freeSlot = UINT32_MAX;
		for (uint32_t i = 0; i < framesInFlight; ++i) {
			const uint32_t cand = (next + i) % framesInFlight;
			if (vkGetFenceStatus(device, commandFences[cand]) == VK_SUCCESS) {
				freeSlot = cand;
				break;
			}
		}
		if (freeSlot != UINT32_MAX) {
			next = freeSlot;
		} else if (framesInFlight < kMaxRingDepth) {
			VkCommandBuffer newCb = VK_NULL_HANDLE;
			VkCommandBufferAllocateInfo allocInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
			allocInfo.commandPool = commandPool;
			allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocInfo.commandBufferCount = 1;
			VkFence newFence = VK_NULL_HANDLE;
			VkFenceCreateInfo fenceInfo{ VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
			fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			if (vkAllocateCommandBuffers(device, &allocInfo, &newCb) == VK_SUCCESS &&
				vkCreateFence(device, &fenceInfo, nullptr, &newFence) == VK_SUCCESS) {
				commandBuffers.push_back(newCb);
				commandFences.push_back(newFence);
				pendingViewDeletes.emplace_back();
				next = framesInFlight;
				++framesInFlight;
				logger::info("[DXVKInterop] Command ring grown to {} (all slots in flight)", framesInFlight);
			} else {
				logger::warn("[DXVKInterop] ring growth failed — falling back to fence wait");
				vkWaitForFences(device, 1, &commandFences[next], VK_TRUE, UINT64_MAX);
			}
		} else {
			static bool s_warned = false;
			if (!s_warned) {
				s_warned = true;
				logger::warn("[DXVKInterop] ring at max depth {} with all slots in flight — waiting (deadlock risk under SL queue-block)", kMaxRingDepth);
			}
			vkWaitForFences(device, 1, &commandFences[next], VK_TRUE, UINT64_MAX);
		}
	}
	commandFrameIndex = next;
	VkFence fence = commandFences[commandFrameIndex];
	VkCommandBuffer cb = commandBuffers[commandFrameIndex];

	// Reusing a signaled slot makes its deferred views safe to destroy.
	if (commandFrameIndex < pendingViewDeletes.size()) {
		auto& dead = pendingViewDeletes[commandFrameIndex];
		if (vkDestroyImageView) {
			for (VkImageView v : dead)
				if (v != VK_NULL_HANDLE)
					vkDestroyImageView(device, v, nullptr);
		}
		dead.clear();
	}

	vkResetFences(device, 1, &fence);
	vkResetCommandBuffer(cb, 0);

	VkCommandBufferBeginInfo beginInfo{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (vkBeginCommandBuffer(cb, &beginInfo) != VK_SUCCESS) {
		logger::error("[DXVKInterop] vkBeginCommandBuffer failed");
		return VK_NULL_HANDLE;
	}
	return cb;
}

bool DXVKInterop::SubmitFrameCommandBuffer(VkCommandBuffer a_commandBuffer, bool a_waitIdle)
{
	if (commandPool == VK_NULL_HANDLE || a_commandBuffer == VK_NULL_HANDLE)
		return false;

	if (vkEndCommandBuffer(a_commandBuffer) != VK_SUCCESS) {
		logger::error("[DXVKInterop] vkEndCommandBuffer failed");
		return false;
	}

	VkFence fence = commandFences[commandFrameIndex];

	VkSubmitInfo submitInfo{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &a_commandBuffer;

	// Serialize the foreign submit with DXVK's worker thread.
	FlushRenderingCommands();
	LockSubmissionQueue();
	VkResult vr = vkQueueSubmit(queue, 1, &submitInfo, fence);
	ReleaseSubmissionQueue();

	if (vr != VK_SUCCESS) {
		logger::error("[DXVKInterop] vkQueueSubmit failed ({})", (int)vr);
		return false;
	}

	if (a_waitIdle) {
		vr = vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX);
		if (vr != VK_SUCCESS) {
			logger::error("[DXVKInterop] upscaler completion wait failed ({})", static_cast<int>(vr));
			return false;
		}
	}

	return true;
}

void DXVKInterop::QueueViewsForDeferredDelete(const VkImageView* a_views, uint32_t a_count)
{
	if (!a_views || commandFrameIndex >= pendingViewDeletes.size())
		return;
	auto& slot = pendingViewDeletes[commandFrameIndex];
	for (uint32_t i = 0; i < a_count; ++i)
		if (a_views[i] != VK_NULL_HANDLE)
			slot.push_back(a_views[i]);
}
