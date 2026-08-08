#pragma once

// DXVK COM interfaces used to access its Vulkan device, queue, and backing images.
// Keep the declarations ABI-compatible with DXVK's dxgi_interfaces.h.

#include <d3d11.h>
#include <dxgi1_6.h>
#include <vector>
#include <vulkan/vulkan.h>
#include <winrt/base.h>

struct IDXGIVkInteropDevice;

MIDL_INTERFACE("5546cf8c-77e7-4341-b05d-8d4d5000e77d")
IDXGIVkInteropSurface : public IUnknown
{
	virtual HRESULT STDMETHODCALLTYPE GetDevice(IDXGIVkInteropDevice * *ppDevice) = 0;
	virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(
		VkImage * pHandle,
		VkImageLayout * pLayout,
		VkImageCreateInfo * pInfo) = 0;
};

MIDL_INTERFACE("e2ef5fa5-dc21-4af7-90c4-f67ef6a09323")
IDXGIVkInteropDevice : public IUnknown
{
	virtual void STDMETHODCALLTYPE GetVulkanHandles(
		VkInstance * pInstance,
		VkPhysicalDevice * pPhysDev,
		VkDevice * pDevice) = 0;
	virtual void STDMETHODCALLTYPE GetSubmissionQueue(
		VkQueue * pQueue,
		uint32_t* pQueueFamilyIndex) = 0;
	virtual void STDMETHODCALLTYPE TransitionSurfaceLayout(
		IDXGIVkInteropSurface * pSurface,
		const VkImageSubresourceRange* pSubresources,
		VkImageLayout OldLayout,
		VkImageLayout NewLayout) = 0;
	virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
	virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
	virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
};

MIDL_INTERFACE("4c5e1b0d-b0c8-4131-bfd8-9b2476f7f408")
IDXGIVkInteropFactory : public IUnknown
{
	virtual void STDMETHODCALLTYPE GetVulkanInstance(
		VkInstance * pInstance,
		PFN_vkGetInstanceProcAddr * ppfnVkGetInstanceProcAddr) = 0;
};

/** @brief Accesses DXVK's Vulkan device through its D3D11 interop interfaces. */
class DXVKInterop
{
public:
	static DXVKInterop* GetSingleton();

	/** @brief Resolves DXVK's interop interfaces and Vulkan handles. */
	bool Initialize();

	/** @brief Whether the DXVK Vulkan device was resolved successfully. */
	bool IsAvailable() const { return available; }

	VkInstance GetInstance() const { return instance; }
	VkPhysicalDevice GetPhysicalDevice() const { return physicalDevice; }
	VkDevice GetDevice() const { return device; }
	PFN_vkGetInstanceProcAddr GetInstanceProcAddr() const { return vkGetInstanceProcAddr; }
	PFN_vkGetDeviceProcAddr GetDeviceProcAddr() const { return vkGetDeviceProcAddr; }

	/** @brief Maps a D3D11 resource to its backing DXVK image. */
	bool GetVkImage(ID3D11Resource* a_resource, VkImage* a_outImage,
		VkImageLayout* a_outLayout = nullptr, VkImageCreateInfo* a_outInfo = nullptr) const;

	/** @brief Flush outstanding D3D11 rendering before foreign Vulkan submits. */
	void FlushRenderingCommands() const;
	/** @brief Drains DXVK submissions without racing its queue thread. */
	void WaitDeviceIdle() const;
	/** @brief Lock DXVK's submission queue around a foreign Vulkan submit (drains pending work). */
	void LockSubmissionQueue() const;
	/** @brief Release DXVK's submission queue after a foreign Vulkan submit. */
	void ReleaseSubmissionQueue() const;

	/** @brief Creates the Streamline command-buffer ring. */
	bool CreateCommandResources(uint32_t a_framesInFlight = 3);

	/** @brief Destroys the command pool, command buffers and fences. */
	void DestroyCommandResources();

	/** @brief Drains ring submissions before interop resources are destroyed. */
	void DrainCommandRing();

	/** @brief Whether the command ring is ready (CreateCommandResources succeeded). */
	bool CommandResourcesReady() const { return commandPool != VK_NULL_HANDLE; }

	/** @brief Begins an available command buffer from the ring. */
	VkCommandBuffer BeginFrameCommandBuffer();

	/** @brief Submits a ring command buffer on DXVK's queue. */
	bool SubmitFrameCommandBuffer(VkCommandBuffer a_commandBuffer, bool a_waitIdle = false);

	/** @brief Defers image-view destruction until the current ring slot completes. */
	void QueueViewsForDeferredDelete(const VkImageView* a_views, uint32_t a_count);

private:
	DXVKInterop() = default;

	bool available = false;

	winrt::com_ptr<IDXGIVkInteropDevice> interopDevice;

	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queueFamilyIndex = 0;

	PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
	PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = nullptr;
	PFN_vkDestroyImageView vkDestroyImageView = nullptr;

	VkCommandPool commandPool = VK_NULL_HANDLE;
	std::vector<VkCommandBuffer> commandBuffers;
	std::vector<VkFence> commandFences;
	// Indexed with commandBuffers and commandFences.
	std::vector<std::vector<VkImageView>> pendingViewDeletes;
	uint32_t framesInFlight = 0;
	uint32_t commandFrameIndex = 0;
};
