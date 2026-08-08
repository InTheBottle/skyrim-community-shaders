#pragma once

#include <cstdint>
#include <d3d11.h>

// Streamline and community upscalers run on DXVK's Vulkan device through full interposition.

class Streamline
{
public:
	static Streamline* GetSingleton();

	/** @brief Maps the interposer before DXVK creates its Vulkan instance. */
	void PreloadInterposer();

	/** @brief Initializes Streamline's Vulkan backend. */
	bool Initialize();

	/** @brief Resolves feature support after the DXVK device is available. */
	void SetVulkanDevice();

	/** @brief Shuts down Streamline and releases the interposer. */
	void Shutdown();

	[[nodiscard]] bool IsInitialized() const { return initialized; }
	/** @brief Returns whether feature support is final for this session. */
	[[nodiscard]] bool IsFeatureSupportResolved() const { return vulkanDeviceSet || disabledByConfig; }

	/** @brief Disables interposition when no Streamline feature is configured. */
	void SetDisabledByConfig() { disabledByConfig = true; }
	[[nodiscard]] bool IsDisabledByConfig() const { return disabledByConfig; }
	[[nodiscard]] bool IsDLSSSupported() const { return featureDLSS; }
	[[nodiscard]] bool IsReflexSupported() const { return featureReflex; }
	[[nodiscard]] bool IsDLSSGSupported() const { return featureDLSSG; }
	[[nodiscard]] bool IsXeSSSupported() const { return featureXeSS; }
	[[nodiscard]] bool IsFSRSupported() const { return featureFSR; }
	[[nodiscard]] bool IsFSRFGSupported() const { return featureFSRFG; }

	[[nodiscard]] bool EvaluateDLSS(ID3D11Resource* a_colorIn, ID3D11Resource* a_colorOut,
		ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
		uint32_t a_renderWidth, uint32_t a_renderHeight,
		uint32_t a_outputWidth, uint32_t a_outputHeight,
		uint32_t a_qualityMode,
		float a_jitterX, float a_jitterY);

	[[nodiscard]] bool EvaluateXeSS(ID3D11Resource* a_colorIn, ID3D11Resource* a_colorOut,
		ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
		uint32_t a_renderWidth, uint32_t a_renderHeight,
		uint32_t a_outputWidth, uint32_t a_outputHeight,
		uint32_t a_qualityMode, float a_sharpness,
		float a_jitterX, float a_jitterY);

	[[nodiscard]] bool EvaluateFSR(ID3D11Resource* a_colorIn, ID3D11Resource* a_colorOut,
		ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
		uint32_t a_renderWidth, uint32_t a_renderHeight,
		uint32_t a_outputWidth, uint32_t a_outputHeight,
		uint32_t a_qualityMode, float a_sharpness,
		float a_jitterX, float a_jitterY);

	/** @brief Prepares FSR frame generation independently of the active upscaler. */
	void EvaluateFSRFrameGen(ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
		ID3D11Resource* a_hudlessColor,
		uint32_t a_renderWidth, uint32_t a_renderHeight,
		uint32_t a_outputWidth, uint32_t a_outputHeight,
		float a_jitterX, float a_jitterY);

	[[nodiscard]] bool SetFSRFrameGen(bool a_enable, uint32_t a_renderWidth, uint32_t a_renderHeight,
		uint32_t a_displayWidth, uint32_t a_displayHeight, bool a_hdr,
		bool a_debugView = false, bool a_debugTearLines = false, bool a_debugPacingLines = false,
		bool a_onlyPresentGenerated = false);

	void LogFSRFrameGenStats();

	/** @brief Updates Reflex and its optional frame-limit interval in microseconds. */
	void UpdateReflex(bool a_enable, bool a_boost, uint32_t a_frameLimitUs = 0);

	enum class PclMarker : uint32_t
	{
		SimulationStart = 0,
		SimulationEnd = 1,
		RenderSubmitStart = 2,
		RenderSubmitEnd = 3,
		PresentStart = 4,
		PresentEnd = 5,
		TriggerFlash = 7,
		PCLatencyPing = 8,
	};

	void SetPCLMarker(PclMarker a_marker);

	/** @brief Updates DLSS-G mode and generated-frame count. */
	void SetDLSSGMode(bool a_enable, uint32_t a_displayWidth, uint32_t a_displayHeight,
		uint32_t a_numFramesToGenerate = 1, bool a_autoMode = false, bool a_dynamic = false,
		float a_dynamicTargetFps = 0.0f);

	/** @brief Establishes the Streamline frame ID at render-frame start. */
	void BeginRenderFrame();

	/** @brief Returns whether present markers are emitted by DXVK's submit thread. */
	[[nodiscard]] bool PresentMarkersBridged() const;
	/** @brief Queues the current frame ID for bridged present markers. */
	void NotifyPresentQueued();

	/** @brief Queries and caches DLSS-G capabilities on the present thread. */
	void QueryDLSSGCapabilities();

	/** @brief Captures the completion fence protecting live frame-generation inputs. */
	void CaptureDLSSGInputFence();
	/** @brief Waits before overwriting live frame-generation inputs. */
	void WaitDLSSGInputFence();
	[[nodiscard]] uint32_t GetDLSSGMaxFramesToGenerate() const;

	/** @brief Returns the latest number of frames presented per rendered frame. */
	[[nodiscard]] uint32_t GetFrameGenerationMultiplier() const;
	[[nodiscard]] bool IsDLSSGDynamicSupported() const;

	/** @brief Sets the desired DLSS-G runtime load state. */
	void SetDLSSGDesiredLoaded(bool a_loaded);
	[[nodiscard]] bool IsDLSSGLoaded() const;
	[[nodiscard]] bool IsDLSSGLoadSettled() const;

	/** @brief Sets the desired FSR frame-generation runtime load state. */
	void SetFSRFGDesiredLoaded(bool a_loaded);
	[[nodiscard]] bool IsFSRFGLoaded() const;
	[[nodiscard]] bool IsFSRFGLoadSettled() const;

	void LogReflexStatus();

	void TagDLSSGResources(ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
		ID3D11Resource* a_hudlessColor, uint32_t a_renderWidth, uint32_t a_renderHeight,
		uint32_t a_displayWidth, uint32_t a_displayHeight);

	void ClearDLSSGTags();
	void EnsureDLSSGPresentTag();

	/** @brief Registers Streamline ownership of DXVK present pacing. */
	static void RegisterDxvkOwnershipPredicate();

	/** @brief Requests a Vulkan swapchain recreation. */
	static void RequestDxvkSwapchainRecreate(const char* a_reason = "FG method switch");

	/** @brief Enables synchronous present while a frame-generation proxy is active. */
	static void PushDxvkSyncPresent(bool a_sync);

private:
	Streamline() = default;

	bool triedInit = false;
	bool initialized = false;
	bool vulkanDeviceSet = false;
	bool disabledByConfig = false;

	bool featureDLSS = false;
	bool featureReflex = false;
	bool featureDLSSG = false;
	bool featureXeSS = false;
	bool featureFSR = false;
	bool featureFSRFG = false;

	// DLSS-G requires VK_NV_optical_flow before Streamline initialization.
	bool dlssgHardware = false;

	bool isNvidiaGPU = false;
	bool isRTXBelow40Series = false;
};
