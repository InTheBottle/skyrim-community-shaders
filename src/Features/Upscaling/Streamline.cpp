#include "Streamline.h"

#include "DXVKInterop.h"
#include "FrameGenController.h"

#include "../Upscaling.h"

#include "../../DxvkLoader.h"
#include "../../Globals.h"
#include "../../State.h"
#include "../../Utils/Game.h"

#include <cmath>
#include <cstring>
#include <filesystem>

#define NV_WINDOWS
#pragma warning(push)
#pragma warning(disable: 4471 5103)
#include <sl.h>
#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_device_wrappers.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_fsr.h>
#include <sl_fsr_g.h>
#include <sl_xess.h>
#include <sl_helpers_vk.h>
#include <sl_matrix_helpers.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_version.h>
#pragma warning(pop)

namespace
{
	struct SLState
	{
		HMODULE interposer = nullptr;

		PFun_slInit* slInit = nullptr;
		PFun_slShutdown* slShutdown = nullptr;
		PFun_slIsFeatureSupported* slIsFeatureSupported = nullptr;
		PFun_slGetFeatureRequirements* slGetFeatureRequirements = nullptr;
		PFun_slGetNewFrameToken* slGetNewFrameToken = nullptr;
		PFun_slSetTagForFrame* slSetTagForFrame = nullptr;
		PFun_slSetConstants* slSetConstants = nullptr;
		PFun_slEvaluateFeature* slEvaluateFeature = nullptr;
		PFun_slGetFeatureFunction* slGetFeatureFunction = nullptr;
		PFun_slAllocateResources* slAllocateResources = nullptr;
		PFun_slFreeResources* slFreeResources = nullptr;
		PFun_slSetFeatureLoaded* slSetFeatureLoaded = nullptr;

		PFun_slDLSSGetOptimalSettings* slDLSSGetOptimalSettings = nullptr;
		PFun_slDLSSSetOptions* slDLSSSetOptions = nullptr;
		PFun_slReflexSetOptions* slReflexSetOptions = nullptr;
		PFun_slReflexSleep* slReflexSleep = nullptr;
		PFun_slReflexGetState* slReflexGetState = nullptr;
		PFun_slPCLSetMarker* slPCLSetMarker = nullptr;
		PFun_slDLSSGSetOptions* slDLSSGSetOptions = nullptr;
		PFun_slDLSSGGetState* slDLSSGGetState = nullptr;
		PFun_slFSRSetOptions* slFSRSetOptions = nullptr;
		PFun_slFSRFrameGenerationSetOptions* slFSRFrameGenerationSetOptions = nullptr;
		PFun_slFSRGetFrameGenState* slFSRGetFrameGenState = nullptr;
		PFun_slXeSSSetOptions* slXeSSSetOptions = nullptr;

		sl::ViewportHandle viewport{ 0 };

		uint32_t renderFrameId = 0;

		void (*dxvkPushPresentAppFrameId)(uint64_t) = nullptr;

		// Disable dispatch after an SEH fault to prevent repeated crashes.
		bool dispatchFaulted = false;

		bool reflexCacheValid = false;
		sl::ReflexMode reflexCachedMode = sl::ReflexMode::eOff;
		uint32_t reflexCachedFrameLimitUs = 0;

		bool dlssgModeCached = false;
		bool dlssgModeOn = false;
		uint32_t dlssgCachedDisplayW = 0, dlssgCachedDisplayH = 0;
		uint32_t dlssgCachedNumFrames = 0;
		bool dlssgCachedAuto = false;
		bool dlssgCachedDynamic = false;
		float dlssgCachedDynamicFps = 0.0f;
		std::atomic<uint32_t> dlssgMaxFramesToGenerate = 0;
		std::atomic<bool> dlssgDynamicSupported = false;
		std::atomic<uint32_t> frameGenerationMultiplier = 1;

		// Completion semaphore for eValidUntilPresent inputs shared across threads.
		std::atomic<void*> dlssgInputFence{ nullptr };
		std::atomic<uint64_t> dlssgInputFenceValue{ 0 };
		std::atomic<uint64_t> dlssgInputFenceWaited{ 0 };
		PFN_vkWaitSemaphores vkWaitSemaphores = nullptr;

		// Present requires either a valid or passthrough tag every frame.
		bool dlssgTaggedThisFrame = false;

		// Views remain alive until DLSS-G consumes the tagged resources at present.
		struct {
			VkImage image = VK_NULL_HANDLE;
			VkImageView view = VK_NULL_HANDLE;
		} dlssgViewCache[3][4];
		uint32_t dlssgViewEvict[3] = {};
	} g_sl;

	// Feature load changes are applied only while the swapchain is torn down.
	std::atomic<bool> g_dlssgDesiredLoaded{ false };
	std::atomic<bool> g_dlssgCurrentlyLoaded{ false };
	std::atomic<bool> g_fsrfgDesiredLoaded{ false };
	std::atomic<bool> g_fsrfgCurrentlyLoaded{ false };
	std::atomic<bool> g_fsrfgOwnsPresent{ false };

	// Keep this free of C++ unwinding because it executes inside __try.
	void ReconcileFgFeatureLoad(sl::Feature a_feature, std::atomic<bool>& a_desired, std::atomic<bool>& a_current)
	{
		const bool want = a_desired.load(std::memory_order_acquire);
		if (want == a_current.load(std::memory_order_acquire) || !g_sl.slSetFeatureLoaded || g_sl.dispatchFaulted)
			return;
		__try {
			if (g_sl.slSetFeatureLoaded(a_feature, want) != sl::Result::eOk)
				return;
			a_current.store(want, std::memory_order_release);
			if (a_feature == sl::kFeatureFSR_G && !want)
				g_fsrfgOwnsPresent.store(false, std::memory_order_release);
			if (want) {
				if (a_feature == sl::kFeatureDLSS_G) {
					g_sl.slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", reinterpret_cast<void*&>(g_sl.slDLSSGSetOptions));
					g_sl.slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", reinterpret_cast<void*&>(g_sl.slDLSSGGetState));
				} else if (a_feature == sl::kFeatureFSR_G) {
					g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationSetOptions", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationSetOptions));
					g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRGetFrameGenState", reinterpret_cast<void*&>(g_sl.slFSRGetFrameGenState));
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			g_sl.dispatchFaulted = true;
		}
	}

	// Runs between DXVK swapchain destruction and creation.
	void DxvkSwapchainTornDownCallback()
	{
		// Per-swapchain options and semaphores are invalid after teardown.
		g_sl.dlssgModeCached = false;
		g_sl.dlssgModeOn = false;

		g_sl.dlssgInputFence.store(nullptr, std::memory_order_release);
		g_sl.dlssgInputFenceValue.store(0, std::memory_order_release);
		g_sl.dlssgInputFenceWaited.store(0, std::memory_order_release);

		ReconcileFgFeatureLoad(sl::kFeatureDLSS_G, g_dlssgDesiredLoaded, g_dlssgCurrentlyLoaded);
		ReconcileFgFeatureLoad(sl::kFeatureFSR_G, g_fsrfgDesiredLoaded, g_fsrfgCurrentlyLoaded);
	}

	bool DxvkFrameGenerationOwnsSwapchain(VkSwapchainKHR)
	{
		return g_dlssgCurrentlyLoaded.load(std::memory_order_acquire) ||
		       g_fsrfgOwnsPresent.load(std::memory_order_acquire);
	}

	// Suppress exact known-benign diagnostics; pass all other messages through.
	bool IsBenignSLWarning(const char* a_msg)
	{
		if (!a_msg)
			return false;
		static constexpr const char* kBenign[] = {
			"setAsyncFrameMarker is not implemented",
			"is NOT supported, plugin will not function properly",
			"RSync will not run because it was not initialized",
			"Invalid backbuffer resource extent",
			"some DX/VK APIs were invoked before slInit",
			"reseting frame timer",
		};
		for (const char* needle : kBenign) {
			if (std::strstr(a_msg, needle))
				return true;
		}
		return false;
	}

	void LogCallback(sl::LogType a_type, const char* a_msg)
	{
		static const bool s_verbose = [] {
			char v[2] = {};
			return GetEnvironmentVariableA("CS_SL_VERBOSE", v, sizeof(v)) && v[0] == 0x31;
		}();
		if (s_verbose) {
			logger::info("[Streamline/SL] {}", a_msg);
			return;
		}
		if (a_type == sl::LogType::eWarn && IsBenignSLWarning(a_msg))
			return;
		switch (a_type) {
		case sl::LogType::eError:
			logger::warn("[Streamline/SL] {}", a_msg);
			break;
		case sl::LogType::eWarn:
			logger::debug("[Streamline/SL] {}", a_msg);
			break;
		default:
			logger::trace("[Streamline/SL] {}", a_msg);
			break;
		}
	}

	std::filesystem::path GetStreamlineDir()
	{
		return DxvkLoader::GetRuntimeDir();
	}

	template <typename T>
	bool Resolve(T*& a_fn, const char* a_name)
	{
		a_fn = reinterpret_cast<T*>(GetProcAddress(g_sl.interposer, a_name));
		if (!a_fn)
			logger::warn("[Streamline] missing interposer export '{}'", a_name);
		return a_fn != nullptr;
	}
}

Streamline* Streamline::GetSingleton()
{
	static Streamline singleton;
	return &singleton;
}

void Streamline::PreloadInterposer()
{
	// Preload before DXVK creates VkInstance so its Vulkan loader aliases the interposer.
	if (disabledByConfig || g_sl.interposer)
		return;
	const auto slDir = GetStreamlineDir();
	if (slDir.empty())
		return;
	const auto interposerPath = (slDir / L"sl.interposer.dll").wstring();
	g_sl.interposer = LoadLibraryExW(interposerPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	logger::info("[Streamline] interposer preload for DXVK Vulkan interposition: {}",
		g_sl.interposer ? "mapped" : "FAILED (DXVK uses real driver)");
	if (!g_sl.interposer)
		return;
	// slInit must precede DXVK's VkInstance creation.
	Initialize();
}

// Probe with the system loader before slInit decides which FG plugin to load.
static bool ProbeDLSSGHardware()
{
	if (char v[2] = {}; GetEnvironmentVariableA("CS_FORCE_FSR_FG", v, sizeof(v)) && v[0] == '1') {
		logger::info("[Streamline] CS_FORCE_FSR_FG=1: hardware probe reports no DLSS-G (FSR-FG path forced)");
		return false;
	}

	wchar_t sysDir[MAX_PATH]{};
	if (!GetSystemDirectoryW(sysDir, MAX_PATH))
		return false;
	const auto vkPath = std::wstring(sysDir) + L"\\vulkan-1.dll";
	HMODULE vk = LoadLibraryExW(vkPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!vk) {
		logger::warn("[Streamline] system vulkan-1.dll unavailable - assuming no DLSS-G hardware");
		return false;
	}

	bool found = false;
	auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vk, "vkGetInstanceProcAddr"));
	auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(GetProcAddress(vk, "vkCreateInstance"));
	if (gipa && createInstance) {
		VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
		app.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo ici{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
		ici.pApplicationInfo = &app;
		VkInstance instance = VK_NULL_HANDLE;
		if (createInstance(&ici, nullptr, &instance) == VK_SUCCESS && instance) {
			auto enumDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
			auto enumExts = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(instance, "vkEnumerateDeviceExtensionProperties"));
			auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
			if (enumDevices && enumExts) {
				uint32_t count = 0;
				enumDevices(instance, &count, nullptr);
				std::vector<VkPhysicalDevice> devices(count);
				enumDevices(instance, &count, devices.data());
				for (auto dev : devices) {
					uint32_t extCount = 0;
					enumExts(dev, nullptr, &extCount, nullptr);
					std::vector<VkExtensionProperties> exts(extCount);
					enumExts(dev, nullptr, &extCount, exts.data());
					for (const auto& e : exts) {
						if (std::strcmp(e.extensionName, "VK_NV_optical_flow") == 0) {
							found = true;
							break;
						}
					}
					if (found)
						break;
				}
			}
			if (destroyInstance)
				destroyInstance(instance, nullptr);
		}
	}
	FreeLibrary(vk);
	logger::info("[Streamline] hardware probe: DLSS-G-class GPU (VK_NV_optical_flow) {}", found ? "present" : "absent");
	return found;
}

bool Streamline::Initialize()
{
	if (disabledByConfig)
		return false;
	if (triedInit)
		return initialized;
	triedInit = true;

	const auto slDir = GetStreamlineDir();
	if (slDir.empty()) {
		logger::warn("[Streamline] could not resolve plugin directory");
		return false;
	}

	const auto interposerPath = (slDir / L"sl.interposer.dll").wstring();
	if (!g_sl.interposer)
		g_sl.interposer = LoadLibraryExW(interposerPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!g_sl.interposer) {
		logger::info("[Streamline] sl.interposer.dll not present in '{}' — DLSS/Reflex disabled", slDir.string());
		return false;
	}

	const bool resolved =
		Resolve(g_sl.slInit, "slInit") &&
		Resolve(g_sl.slShutdown, "slShutdown") &&
		Resolve(g_sl.slIsFeatureSupported, "slIsFeatureSupported") &&
		Resolve(g_sl.slGetFeatureRequirements, "slGetFeatureRequirements") &&
		Resolve(g_sl.slGetNewFrameToken, "slGetNewFrameToken") &&
		Resolve(g_sl.slSetTagForFrame, "slSetTagForFrame") &&
		Resolve(g_sl.slSetConstants, "slSetConstants") &&
		Resolve(g_sl.slEvaluateFeature, "slEvaluateFeature") &&
		Resolve(g_sl.slGetFeatureFunction, "slGetFeatureFunction") &&
		Resolve(g_sl.slAllocateResources, "slAllocateResources") &&
		Resolve(g_sl.slFreeResources, "slFreeResources");

	Resolve(g_sl.slSetFeatureLoaded, "slSetFeatureLoaded");
	if (!resolved) {
		FreeLibrary(g_sl.interposer);
		g_sl.interposer = nullptr;
		return false;
	}

	const auto slDirWide = slDir.wstring();
	const wchar_t* pluginPaths[] = { slDirWide.c_str() };
	// The controller keeps at most one frame-generation feature loaded at runtime.
	dlssgHardware = ProbeDLSSGHardware();

	std::vector<sl::Feature> featuresToLoad = { sl::kFeatureDLSS, sl::kFeatureReflex, sl::kFeaturePCL,
		sl::kFeatureFSR, sl::kFeatureFSR_G, sl::kFeatureXeSS };
	if (dlssgHardware) {
		featuresToLoad.push_back(sl::kFeatureDLSS_G);
		g_dlssgDesiredLoaded.store(true, std::memory_order_release);
		g_dlssgCurrentlyLoaded.store(true, std::memory_order_release);
	}
	g_fsrfgDesiredLoaded.store(true, std::memory_order_release);
	g_fsrfgCurrentlyLoaded.store(true, std::memory_order_release);

	sl::Preferences pref{};
	pref.renderAPI = sl::RenderAPI::eVulkan;
	pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;
	pref.featuresToLoad = featuresToLoad.data();
	pref.numFeaturesToLoad = static_cast<uint32_t>(featuresToLoad.size());
	pref.pathsToPlugins = pluginPaths;
	pref.numPathsToPlugins = 1;
	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0";
	pref.projectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";
	if (char v[2] = {}; GetEnvironmentVariableA("CS_SL_VERBOSE", v, sizeof(v)) && v[0] == 0x31)
		pref.logLevel = sl::LogLevel::eVerbose;
	else
	pref.logLevel = sl::LogLevel::eDefault;
	pref.logMessageCallback = &LogCallback;

	const sl::Result res = g_sl.slInit(pref, sl::kSDKVersion);
	if (res != sl::Result::eOk) {
		logger::warn("[Streamline] slInit failed (result {}) — DLSS/Reflex disabled", static_cast<int>(res));
		FreeLibrary(g_sl.interposer);
		g_sl.interposer = nullptr;
		return false;
	}

	initialized = true;
	logger::info("[Streamline] initialized on Vulkan (SDK {}.{}.{})",
		SL_VERSION_MAJOR, SL_VERSION_MINOR, SL_VERSION_PATCH);
	return true;
}

void Streamline::SetVulkanDevice()
{
	if (!initialized || vulkanDeviceSet)
		return;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk || !dxvk->IsAvailable()) {
		logger::warn("[Streamline] DXVK interop unavailable — cannot hand Vulkan device to SL");
		return;
	}

	vulkanDeviceSet = true;

	// Probe support against DXVK's physical device.
	sl::AdapterInfo adapter{};
	adapter.vkPhysicalDevice = dxvk->GetPhysicalDevice();
	const auto supported = [&](sl::Feature f) {
		const sl::Result r = g_sl.slIsFeatureSupported(f, adapter);
		if (r != sl::Result::eOk)
			logger::info("[Streamline] feature {} unsupported (result {})", f, static_cast<int>(r));
		return r == sl::Result::eOk;
	};

	featureDLSS = supported(sl::kFeatureDLSS);
	featureReflex = supported(sl::kFeatureReflex);
	featureDLSSG = supported(sl::kFeatureDLSS_G);
	featureXeSS = supported(sl::kFeatureXeSS);
	featureFSR = supported(sl::kFeatureFSR);
	featureFSRFG = supported(sl::kFeatureFSR_G);

	if (featureDLSS) {
		g_sl.slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", reinterpret_cast<void*&>(g_sl.slDLSSGetOptimalSettings));
		g_sl.slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", reinterpret_cast<void*&>(g_sl.slDLSSSetOptions));
		featureDLSS = g_sl.slDLSSSetOptions != nullptr;
	}
	if (featureReflex) {
		g_sl.slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", reinterpret_cast<void*&>(g_sl.slReflexSetOptions));
		g_sl.slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", reinterpret_cast<void*&>(g_sl.slReflexSleep));
		g_sl.slGetFeatureFunction(sl::kFeatureReflex, "slReflexGetState", reinterpret_cast<void*&>(g_sl.slReflexGetState));
		featureReflex = g_sl.slReflexSetOptions != nullptr && g_sl.slReflexSleep != nullptr;
	}
	g_sl.slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", reinterpret_cast<void*&>(g_sl.slPCLSetMarker));
	logger::info("[Streamline] PCL latency markers {}", g_sl.slPCLSetMarker ? "available" : "unavailable");
	if (featureDLSSG) {
		g_sl.slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", reinterpret_cast<void*&>(g_sl.slDLSSGSetOptions));
		g_sl.slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", reinterpret_cast<void*&>(g_sl.slDLSSGGetState));
		featureDLSSG = g_sl.slDLSSGSetOptions != nullptr && g_sl.slDLSSGGetState != nullptr;
	}
	if (featureFSR) {
		g_sl.slGetFeatureFunction(sl::kFeatureFSR, "slFSRSetOptions", reinterpret_cast<void*&>(g_sl.slFSRSetOptions));
		featureFSR = g_sl.slFSRSetOptions != nullptr;
	}
	if (featureFSRFG) {
		g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRFrameGenerationSetOptions", reinterpret_cast<void*&>(g_sl.slFSRFrameGenerationSetOptions));
		g_sl.slGetFeatureFunction(sl::kFeatureFSR_G, "slFSRGetFrameGenState", reinterpret_cast<void*&>(g_sl.slFSRGetFrameGenState));
		featureFSRFG = g_sl.slFSRFrameGenerationSetOptions != nullptr;
	}
	if (featureXeSS) {
		g_sl.slGetFeatureFunction(sl::kFeatureXeSS, "slXeSSSetOptions", reinterpret_cast<void*&>(g_sl.slXeSSSetOptions));
		featureXeSS = g_sl.slXeSSSetOptions != nullptr;
	}

	featureDLSSG = featureDLSSG && dlssgHardware;

	logger::info("[Streamline] feature support: DLSS={} Reflex={} DLSS-G={} FSR={} FSR-G={} XeSS={} (FSR-FG fns {})",
		featureDLSS, featureReflex, featureDLSSG, featureFSR, featureFSRFG, featureXeSS,
		g_sl.slFSRFrameGenerationSetOptions ? "ok" : "missing");

	// Use Vulkan IDs because the D3D create hook may not see the adapter.
	if (auto getProps = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
			dxvk->GetInstanceProcAddr()(dxvk->GetInstance(), "vkGetPhysicalDeviceProperties"))) {
		VkPhysicalDeviceProperties props{};
		getProps(dxvk->GetPhysicalDevice(), &props);
		isNvidiaGPU = props.vendorID == 0x10DE;
		isRTXBelow40Series = isNvidiaGPU &&
		                     ((props.deviceID >= 0x2200 && props.deviceID <= 0x2600) ||   // RTX 30 (Ampere)
								(props.deviceID >= 0x1E00 && props.deviceID <= 0x1FFF));   // RTX 20 (Turing w/ RT)
		logger::info("[Streamline] GPU vendor=0x{:04X} device=0x{:04X} -> DLSS preset group: {}",
			props.vendorID, props.deviceID,
			isNvidiaGPU ? (isRTXBelow40Series ? "RTX 20/30 (J)" : "RTX 40+ (M)") : "non-NVIDIA (default)");
	}
}

void Streamline::Shutdown()
{
	if (auto* dxvk = DXVKInterop::GetSingleton(); dxvk && dxvk->IsAvailable()) {
		VkDevice vkDevice = dxvk->GetDevice();
		if (auto vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(
				dxvk->GetDeviceProcAddr()(vkDevice, "vkDestroyImageView"))) {
			for (auto& slot : g_sl.dlssgViewCache) {
				for (auto& c : slot) {
					if (c.view != VK_NULL_HANDLE)
						vkDestroyImageView(vkDevice, c.view, nullptr);
					c.view = VK_NULL_HANDLE;
					c.image = VK_NULL_HANDLE;
				}
			}
		}
	}

	if (g_sl.interposer) {
		if (initialized && g_sl.slShutdown)
			g_sl.slShutdown();
		FreeLibrary(g_sl.interposer);
		g_sl.interposer = nullptr;
	}
	initialized = false;
	vulkanDeviceSet = false;
	featureDLSS = featureReflex = featureDLSSG = featureXeSS = featureFSR = featureFSRFG = false;
}

static sl::FrameToken* TokenForFrame(uint32_t a_frameId)
{
	sl::FrameToken* token = nullptr;
	if (g_sl.slGetNewFrameToken(token, &a_frameId) != sl::Result::eOk)
		return nullptr;
	return token;
}

static sl::FrameToken* RenderFrameToken()
{
	return TokenForFrame(g_sl.renderFrameId);
}

static uint32_t SimFrameId()
{
	return globals::state->frameCountAtomic.load(std::memory_order_relaxed) + 1;
}

// Fires bridged PCL markers around the actual vkQueuePresentKHR call.
static void CS_DxvkPresentMarkerBridge(uint64_t a_appFrameId, uint32_t a_phase)
{
	if (!g_sl.slPCLSetMarker || g_sl.dispatchFaulted)
		return;
	__try {
		if (sl::FrameToken* token = TokenForFrame(static_cast<uint32_t>(a_appFrameId)))
			g_sl.slPCLSetMarker(a_phase == 0u ? sl::PCLMarker::ePresentStart : sl::PCLMarker::ePresentEnd, *token);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
	}
}

void Streamline::BeginRenderFrame()
{
	g_sl.renderFrameId = globals::state->frameCount;
	g_sl.dlssgTaggedThisFrame = false;

	WaitDLSSGInputFence();
}

void Streamline::CaptureDLSSGInputFence()
{
	// Capture the completion point before the next frame overwrites live inputs.
	if (!initialized || !featureDLSSG || g_sl.dispatchFaulted ||
		!g_dlssgCurrentlyLoaded.load(std::memory_order_acquire) || !g_sl.dlssgModeOn)
		return;
	__try {
		sl::DLSSGState state{};
		if (g_sl.slDLSSGGetState(g_sl.viewport, state, nullptr) == sl::Result::eOk) {
			g_sl.frameGenerationMultiplier.store(
				std::max(state.numFramesActuallyPresented, 1u), std::memory_order_release);
			static uint32_t s_logCounter = 0;
			if ((s_logCounter++ % 120) == 0) {
				logger::info("[Streamline] DLSS-G state: presentedPerFrame={} status={} vram={} MB",
					state.numFramesActuallyPresented,
					static_cast<int>(state.status),
					state.estimatedVRAMUsageInBytes >> 20);
			}

			if (g_sl.dlssgInputFence.exchange(state.inputsProcessingCompletionFence, std::memory_order_acq_rel) !=
				state.inputsProcessingCompletionFence)
				g_sl.dlssgInputFenceWaited.store(0, std::memory_order_release);
			g_sl.dlssgInputFenceValue.store(state.lastPresentInputsProcessingCompletionFenceValue, std::memory_order_release);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
	}
}

void Streamline::WaitDLSSGInputFence()
{
	if (!initialized || g_sl.dispatchFaulted || !g_sl.dlssgModeOn)
		return;
	void* fence = g_sl.dlssgInputFence.load(std::memory_order_acquire);
	const uint64_t value = g_sl.dlssgInputFenceValue.load(std::memory_order_acquire);
	if (!fence || value == 0 || value <= g_sl.dlssgInputFenceWaited.load(std::memory_order_acquire))
		return;

	auto* dxvk = DXVKInterop::GetSingleton();
	VkDevice device = dxvk->GetDevice();
	if (device == VK_NULL_HANDLE)
		return;
	if (!g_sl.vkWaitSemaphores) {
		g_sl.vkWaitSemaphores = reinterpret_cast<PFN_vkWaitSemaphores>(
			dxvk->GetDeviceProcAddr()(device, "vkWaitSemaphores"));
		if (!g_sl.vkWaitSemaphores)
			return;
	}

	__try {
		VkSemaphore sem = reinterpret_cast<VkSemaphore>(fence);
		uint64_t waitValue = value;
		VkSemaphoreWaitInfo wi{ VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
		wi.semaphoreCount = 1;
		wi.pSemaphores = &sem;
		wi.pValues = &waitValue;
		// Never block the render thread indefinitely on a stalled plugin.
		const VkResult wr = g_sl.vkWaitSemaphores(device, &wi, 8ull * 1000ull * 1000ull);
		if (wr == VK_SUCCESS)
			g_sl.dlssgInputFenceWaited.store(value, std::memory_order_release);
		else if (wr == VK_TIMEOUT) {
			static bool s_warned = false;
			if (!s_warned) {
				s_warned = true;
				logger::warn("[Streamline] DLSS-G input fence wait timed out (value {}) — proceeding", value);
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
	}
}

void Streamline::UpdateReflex(bool a_enable, bool a_boost, uint32_t a_frameLimitUs)
{
	if (!initialized || !featureReflex || g_sl.dispatchFaulted)
		return;

	const sl::ReflexMode mode = !a_enable ? sl::ReflexMode::eOff :
	                            a_boost   ? sl::ReflexMode::eLowLatencyWithBoost :
	                                        sl::ReflexMode::eLowLatency;

	__try {
		if (!g_sl.reflexCacheValid || g_sl.reflexCachedMode != mode || g_sl.reflexCachedFrameLimitUs != a_frameLimitUs) {
			sl::ReflexOptions options{};
			options.mode = mode;
			options.frameLimitUs = a_frameLimitUs;
			if (g_sl.slReflexSetOptions(options) == sl::Result::eOk) {
				g_sl.reflexCachedMode = mode;
				g_sl.reflexCachedFrameLimitUs = a_frameLimitUs;
				g_sl.reflexCacheValid = true;
			}
		}
		if (mode != sl::ReflexMode::eOff) {
			// PollInputDevices can run more than once per rendered frame.
			static uint32_t s_lastSleepFrame = UINT32_MAX;
			const uint32_t simFrame = SimFrameId();
			if (s_lastSleepFrame != simFrame) {
				s_lastSleepFrame = simFrame;
				if (sl::FrameToken* token = TokenForFrame(simFrame))
					g_sl.slReflexSleep(*token);
			}
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] Reflex dispatch faulted — disabling for this session");
	}
}

void Streamline::SetPCLMarker(PclMarker a_marker)
{
	if (!initialized || !g_sl.slPCLSetMarker || g_sl.dispatchFaulted)
		return;

	// Emit SimulationStart once per simulated frame, not once per input poll.
	uint32_t simFrame = 0;
	if (a_marker == PclMarker::SimulationStart) {
		static uint32_t s_lastSimFrame = UINT32_MAX;
		simFrame = SimFrameId();
		if (s_lastSimFrame == simFrame)
			return;
		s_lastSimFrame = simFrame;
	}

	__try {
		const bool renderThreadMarker =
			a_marker == PclMarker::RenderSubmitStart || a_marker == PclMarker::RenderSubmitEnd ||
			a_marker == PclMarker::PresentStart || a_marker == PclMarker::PresentEnd ||
			a_marker == PclMarker::TriggerFlash ||
			a_marker == PclMarker::SimulationEnd;
		sl::FrameToken* token = renderThreadMarker ?
		                            RenderFrameToken() :
		                            TokenForFrame(simFrame ? simFrame : SimFrameId());
		if (token)
			g_sl.slPCLSetMarker(static_cast<sl::PCLMarker>(a_marker), *token);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] PCL marker faulted — disabling for this session");
	}
}

// Returns false until the engine camera matrices are finite and invertible.
static bool cs_IsFiniteMatrix(const Matrix& a_matrix)
{
	return std::isfinite(a_matrix._11) && std::isfinite(a_matrix._12) &&
	       std::isfinite(a_matrix._13) && std::isfinite(a_matrix._14) &&
	       std::isfinite(a_matrix._21) && std::isfinite(a_matrix._22) &&
	       std::isfinite(a_matrix._23) && std::isfinite(a_matrix._24) &&
	       std::isfinite(a_matrix._31) && std::isfinite(a_matrix._32) &&
	       std::isfinite(a_matrix._33) && std::isfinite(a_matrix._34) &&
	       std::isfinite(a_matrix._41) && std::isfinite(a_matrix._42) &&
	       std::isfinite(a_matrix._43) && std::isfinite(a_matrix._44);
}

static bool cs_BuildConstants(sl::Constants& a_consts, uint32_t a_outputWidth, uint32_t a_outputHeight,
	float a_jitterX, float a_jitterY)
{
	a_consts = {};
	a_consts.cameraAspectRatio = static_cast<float>(a_outputWidth) / static_cast<float>(a_outputHeight);
	a_consts.cameraFOV = Util::GetVerticalFOVRad();
	a_consts.cameraNear = *globals::game::cameraNear;
	a_consts.cameraFar = *globals::game::cameraFar;

	auto viewMatrix = globals::game::frameBufferCached.GetCameraViewInverse().Transpose();
	auto cameraViewToClip = globals::game::frameBufferCached.GetCameraProjUnjittered().Transpose();

	a_consts.cameraMotionIncluded = sl::Boolean::eTrue;
	a_consts.cameraPinholeOffset = { 0.f, 0.f };
	a_consts.cameraRight = { viewMatrix._11, viewMatrix._12, viewMatrix._13 };
	a_consts.cameraUp = { viewMatrix._21, viewMatrix._22, viewMatrix._23 };
	a_consts.cameraFwd = { viewMatrix._31, viewMatrix._32, viewMatrix._33 };
	const auto cameraPosAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();
	a_consts.cameraPos = *reinterpret_cast<const sl::float3*>(&cameraPosAdjust);
	a_consts.cameraViewToClip = *reinterpret_cast<const sl::float4x4*>(&cameraViewToClip);
	a_consts.depthInverted = sl::Boolean::eFalse;

	sl::recalculateCameraMatrices(a_consts);

	// Translate between the current and previous camera-relative origins before reprojection.
	// Streamline applies jitter separately, so both matrices remain unjittered.
	Matrix curVP = globals::game::frameBufferCached.GetCameraViewProjUnjittered().Transpose();
	Matrix prevVP = globals::game::frameBufferCached.GetCameraPreviousViewProjUnjittered().Transpose();
	const auto& posAdj = globals::game::frameBufferCached.GetCameraPosAdjust();
	const auto& prevPosAdj = globals::game::frameBufferCached.GetCameraPreviousPosAdjust();
	Matrix camDelta = Matrix::CreateTranslation(posAdj.x - prevPosAdj.x, posAdj.y - prevPosAdj.y, posAdj.z - prevPosAdj.z);
	Matrix clipToPrevClip = curVP.Invert() * camDelta * prevVP;
	Matrix prevClipToClip = clipToPrevClip.Invert();
	a_consts.clipToPrevClip = *reinterpret_cast<const sl::float4x4*>(&clipToPrevClip);
	a_consts.prevClipToClip = *reinterpret_cast<const sl::float4x4*>(&prevClipToClip);

	a_consts.jitterOffset = { -a_jitterX, -a_jitterY };
	// Reset temporal history after leaving a loading screen.
	{
		static bool s_wasLoading = false;
		const bool loading = globals::state->isLoadingMenuOpen;
		a_consts.reset = (!loading && s_wasLoading) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		s_wasLoading = loading;
	}
	a_consts.mvecScale = { 1.0f, 1.0f };
	a_consts.motionVectors3D = sl::Boolean::eFalse;
	a_consts.motionVectorsInvalidValue = FLT_MIN;
	a_consts.orthographicProjection = sl::Boolean::eFalse;
	a_consts.motionVectorsDilated = sl::Boolean::eFalse;
	a_consts.motionVectorsJittered = sl::Boolean::eFalse;

	// Reject singular engine matrices before passing constants to Streamline.
	const bool matricesFinite = cs_IsFiniteMatrix(cameraViewToClip) &&
	                            cs_IsFiniteMatrix(clipToPrevClip) &&
	                            cs_IsFiniteMatrix(prevClipToClip);
	const bool basisFinite = std::isfinite(a_consts.cameraRight.x) &&
	                         std::isfinite(a_consts.cameraRight.y) &&
	                         std::isfinite(a_consts.cameraRight.z) &&
	                         std::isfinite(a_consts.cameraUp.x) &&
	                         std::isfinite(a_consts.cameraUp.y) &&
	                         std::isfinite(a_consts.cameraUp.z) &&
	                         std::isfinite(a_consts.cameraFwd.x) &&
	                         std::isfinite(a_consts.cameraFwd.y) &&
	                         std::isfinite(a_consts.cameraFwd.z);
	const bool scalarsFinite = std::isfinite(a_consts.cameraAspectRatio) &&
	                           std::isfinite(a_consts.cameraFOV) &&
	                           std::isfinite(a_consts.cameraNear) &&
	                           std::isfinite(a_consts.cameraFar);
	static bool s_cameraDataInvalid = false;
	if (!matricesFinite || !basisFinite || !scalarsFinite) {
		if (!s_cameraDataInvalid) {
			s_cameraDataInvalid = true;
			logger::warn("[Streamline] skipping evaluate: invalid camera constants "
				"(matrices={} basis={} scalars={} proj=[{},{},{},{},{},{}])",
				matricesFinite, basisFinite, scalarsFinite,
				cameraViewToClip._11, cameraViewToClip._22, cameraViewToClip._33,
				cameraViewToClip._34, cameraViewToClip._43, cameraViewToClip._44);
		}
		return false;
	}
	if (s_cameraDataInvalid) {
		s_cameraDataInvalid = false;
		logger::info("[Streamline] camera constants became valid; evaluations resumed");
	}

	return true;
}

// Streamline's Vulkan backend requires a matching VkImageView for every resource.
static bool cs_WrapInteropImage(DXVKInterop* a_dxvk, VkDevice a_device, PFN_vkCreateImageView a_createView,
	ID3D11Resource* a_res, sl::Resource& a_out, uint32_t a_w, uint32_t a_h, VkImageView& a_outView)
{
	a_outView = VK_NULL_HANDLE;
	VkImage image = VK_NULL_HANDLE;
	VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
	if (!a_dxvk->GetVkImage(a_res, &image, &layout, &info) || image == VK_NULL_HANDLE)
		return false;
	VkImageView view = VK_NULL_HANDLE;
	if (!a_createView)
		return false;
	VkImageViewCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	ci.image = image;
	ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	ci.format = info.format;
	ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	if (info.format == VK_FORMAT_D32_SFLOAT || info.format == VK_FORMAT_D24_UNORM_S8_UINT ||
		info.format == VK_FORMAT_D16_UNORM || info.format == VK_FORMAT_D32_SFLOAT_S8_UINT)
		ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
	ci.subresourceRange.levelCount = 1;
	ci.subresourceRange.layerCount = 1;
	if (a_createView(a_device, &ci, nullptr, &view) != VK_SUCCESS || view == VK_NULL_HANDLE)
		return false;
	a_outView = view;
	a_out = sl::Resource{ sl::ResourceType::eTex2d, image, nullptr, view, static_cast<uint32_t>(layout) };
	a_out.width = a_w;
	a_out.height = a_h;
	a_out.nativeFormat = static_cast<uint32_t>(info.format);
	a_out.mipLevels = info.mipLevels;
	a_out.arrayLayers = info.arrayLayers;
	a_out.usage = static_cast<uint32_t>(info.usage);
	a_out.flags = static_cast<uint32_t>(info.flags);
	return true;
}

static void cs_DestroyViews(DXVKInterop* a_dxvk, VkDevice a_device, const VkImageView* a_views, int a_count)
{
	if (auto vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(
			a_dxvk->GetDeviceProcAddr()(a_device, "vkDestroyImageView"))) {
		for (int i = 0; i < a_count; ++i)
			if (a_views[i] != VK_NULL_HANDLE)
				vkDestroyImageView(a_device, a_views[i], nullptr);
	}
}

// Color evaluations wait for D3D11 consumption; frame-generation preparation remains asynchronous.
static sl::Result cs_EvaluateFeatureCore(sl::Feature a_feature, const sl::ViewportHandle& a_viewport,
	ID3D11Resource* a_colorIn, ID3D11Resource* a_colorOut, ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	uint32_t a_renderWidth, uint32_t a_renderHeight, uint32_t a_outputWidth, uint32_t a_outputHeight,
	float a_jitterX, float a_jitterY, ID3D11Resource* a_hudlessColor = nullptr,
	bool* a_outputReady = nullptr, ID3D11Resource* a_uiColor = nullptr)
{
	if (a_outputReady)
		*a_outputReady = false;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk)
		return sl::Result::eErrorNotInitialized;

	sl::FrameToken* token = RenderFrameToken();
	if (!token)
		return sl::Result::eErrorMissingInputParameter;

	// Pair constants and evaluation once per frame and viewport.
	static uint32_t s_evalFrameByVp[2] = { UINT32_MAX, UINT32_MAX };
	static uint32_t s_constFrameByVp[2] = { UINT32_MAX, UINT32_MAX };
	const uint32_t vpId = a_viewport;
	if (vpId < 2) {
		if (s_evalFrameByVp[vpId] == g_sl.renderFrameId)
			return sl::Result::eOk;
		if (s_constFrameByVp[vpId] != g_sl.renderFrameId) {
			sl::Constants consts;
			if (!cs_BuildConstants(consts, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY))
				return sl::Result::eOk;
			const sl::Result constantsRes = g_sl.slSetConstants(consts, *token, a_viewport);
			if (constantsRes != sl::Result::eOk) {
				logger::error("[Streamline] slSetConstants failed for viewport {} (result {})",
					vpId, static_cast<int>(constantsRes));
				return constantsRes;
			}
			s_constFrameByVp[vpId] = g_sl.renderFrameId;
		}
	} else {
		sl::Constants consts;
		if (!cs_BuildConstants(consts, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY))
			return sl::Result::eOk;
		const sl::Result constantsRes = g_sl.slSetConstants(consts, *token, a_viewport);
		if (constantsRes != sl::Result::eOk)
			return constantsRes;
	}

	VkDevice vkDevice = dxvk->GetDevice();
	auto vkCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(
		dxvk->GetDeviceProcAddr()(vkDevice, "vkCreateImageView"));
	VkImageView views[6] = {};
	int nv = 0;
	const auto wrap = [&](ID3D11Resource* a_res, sl::Resource& a_out, uint32_t a_w, uint32_t a_h) -> bool {
		VkImageView v = VK_NULL_HANDLE;
		if (!cs_WrapInteropImage(dxvk, vkDevice, vkCreateImageView, a_res, a_out, a_w, a_h, v))
			return false;
		if (v != VK_NULL_HANDLE && nv < 6)
			views[nv++] = v;
		return true;
	};

	const bool haveColor = (a_colorIn && a_colorOut);
	const bool haveHudless = (a_hudlessColor != nullptr);
	const bool haveUI = (a_uiColor != nullptr);
	sl::Resource colorInRes{}, colorOutRes{}, depthRes{}, mvecRes{}, hudlessRes{}, uiRes{};
	bool ok = wrap(a_depth, depthRes, a_renderWidth, a_renderHeight) &&
	          wrap(a_motionVectors, mvecRes, a_renderWidth, a_renderHeight);
	if (ok && haveColor)
		ok = wrap(a_colorIn, colorInRes, a_renderWidth, a_renderHeight) &&
		     wrap(a_colorOut, colorOutRes, a_outputWidth, a_outputHeight);
	if (ok && haveHudless)
		ok = wrap(a_hudlessColor, hudlessRes, a_outputWidth, a_outputHeight);
	if (ok && haveUI)
		ok = wrap(a_uiColor, uiRes, a_outputWidth, a_outputHeight);
	if (!ok) {
		cs_DestroyViews(dxvk, vkDevice, views, nv);
		return sl::Result::eErrorMissingInputParameter;
	}

	// Streamline reconstructs motion on the depth grid.
	mvecRes.width = depthRes.width;
	mvecRes.height = depthRes.height;

	sl::Extent renderExtent{ 0, 0, a_renderWidth, a_renderHeight };
	sl::Extent outputExtent{ 0, 0, a_outputWidth, a_outputHeight };
	sl::ResourceTag tags[6];
	uint32_t nt = 0;
	if (haveColor) {
		tags[nt++] = sl::ResourceTag{ &colorInRes, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilPresent, &renderExtent };
		tags[nt++] = sl::ResourceTag{ &colorOutRes, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilPresent, &outputExtent };
	}
	// Snapshot frame-generation inputs because their resources may change before present.
	tags[nt++] = sl::ResourceTag{ &depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent };
	tags[nt++] = sl::ResourceTag{ &mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &renderExtent };
	if (haveHudless)
		tags[nt++] = sl::ResourceTag{ &hudlessRes, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, &outputExtent };
	if (haveUI)
		tags[nt++] = sl::ResourceTag{ &uiRes, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &outputExtent };

	sl::Result evalRes = sl::Result::eErrorNotInitialized;
	VkCommandBuffer cmd = dxvk->BeginFrameCommandBuffer();
	if (cmd != VK_NULL_HANDLE) {
		const sl::Result tagRes = g_sl.slSetTagForFrame(*token, a_viewport, tags, nt, cmd);
		if (tagRes == sl::Result::eOk) {
			const sl::BaseStructure* inputs[] = { &a_viewport };
			evalRes = g_sl.slEvaluateFeature(a_feature, *token, inputs, static_cast<uint32_t>(std::size(inputs)), cmd);
		} else {
			evalRes = tagRes;
			logger::error("[Streamline] slSetTagForFrame failed for feature {} viewport {} (result {})",
				static_cast<uint32_t>(a_feature), vpId, static_cast<int>(tagRes));
		}
		// DXVK cannot infer the dependency between this Vulkan submit and D3D11 CopyResource.
		const bool waitForOutput = haveColor;
		if (dxvk->SubmitFrameCommandBuffer(cmd, waitForOutput)) {
			if (a_outputReady && haveColor && evalRes == sl::Result::eOk)
				*a_outputReady = true;
			if (waitForOutput) {
				cs_DestroyViews(dxvk, vkDevice, views, nv);
				static bool s_loggedSynchronizedOutput = false;
				if (!s_loggedSynchronizedOutput) {
					s_loggedSynchronizedOutput = true;
					logger::info("[Streamline] Vulkan upscaler output synchronized before D3D11 copy-back");
				}
			} else {
				dxvk->QueueViewsForDeferredDelete(views, static_cast<uint32_t>(nv));
			}
			if (evalRes == sl::Result::eOk && vpId < 2)
				s_evalFrameByVp[vpId] = g_sl.renderFrameId;
		} else {
			cs_DestroyViews(dxvk, vkDevice, views, nv);
			evalRes = sl::Result::eErrorExceptionHandler;
		}
	} else {
		cs_DestroyViews(dxvk, vkDevice, views, nv);
	}
	return evalRes;
}

bool Streamline::EvaluateDLSS(ID3D11Resource* a_colorIn, ID3D11Resource* a_colorOut,
	ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_outputWidth, uint32_t a_outputHeight,
	uint32_t a_qualityMode,
	float a_jitterX, float a_jitterY)
{
	bool outputReady = false;
	if (!initialized || !featureDLSS || g_sl.dispatchFaulted)
		return false;
	if (!a_colorIn || !a_colorOut || !a_depth || !a_motionVectors)
		return false;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk->CommandResourcesReady())
		return false;
	// Submit pending D3D11 work before recording Streamline commands against its images.
	dxvk->FlushRenderingCommands();

	__try {
		sl::DLSSMode dlssMode = sl::DLSSMode::eMaxQuality;
		switch (a_qualityMode) {
		case 0:
			dlssMode = sl::DLSSMode::eDLAA;
			break;
		case 1:
			dlssMode = sl::DLSSMode::eMaxQuality;
			break;
		case 2:
			dlssMode = sl::DLSSMode::eBalanced;
			break;
		case 3:
			dlssMode = sl::DLSSMode::eMaxPerformance;
			break;
		case 4:
			dlssMode = sl::DLSSMode::eUltraPerformance;
			break;
		default:
			dlssMode = sl::DLSSMode::eMaxQuality;
			break;
		}

		sl::DLSSOptions options{};
		options.mode = dlssMode;
		options.outputWidth = a_outputWidth;
		options.outputHeight = a_outputHeight;
		// The Vulkan scene chain is always FP16 HDR.
		options.colorBuffersHDR = sl::Boolean::eTrue;
		options.useAutoExposure = sl::Boolean::eTrue;

		// Use the recommended preset for the detected NVIDIA architecture.
		if (isRTXBelow40Series) {
			options.dlaaPreset = sl::DLSSPreset::ePresetJ;
			options.ultraQualityPreset = sl::DLSSPreset::ePresetJ;
			options.qualityPreset = sl::DLSSPreset::ePresetJ;
			options.balancedPreset = sl::DLSSPreset::ePresetJ;
			options.performancePreset = sl::DLSSPreset::ePresetJ;
			options.ultraPerformancePreset = sl::DLSSPreset::ePresetM;
		} else if (isNvidiaGPU) {
			options.dlaaPreset = sl::DLSSPreset::ePresetJ;
			options.ultraQualityPreset = sl::DLSSPreset::ePresetJ;
			options.qualityPreset = sl::DLSSPreset::ePresetM;
			options.balancedPreset = sl::DLSSPreset::ePresetM;
			options.performancePreset = sl::DLSSPreset::ePresetM;
			options.ultraPerformancePreset = sl::DLSSPreset::ePresetL;
		}

		static bool s_loggedPreset = false;
		if (!s_loggedPreset) {
			s_loggedPreset = true;
			logger::info("[Streamline] DLSS presets set (mode {}): quality={} (RTX40+={} below40={})",
				static_cast<int>(dlssMode), static_cast<int>(options.qualityPreset),
				isNvidiaGPU && !isRTXBelow40Series, isRTXBelow40Series);
		}

		if (g_sl.slDLSSSetOptions(g_sl.viewport, options) != sl::Result::eOk)
			return false;

		const sl::Result evalRes = cs_EvaluateFeatureCore(sl::kFeatureDLSS, g_sl.viewport,
			a_colorIn, a_colorOut, a_depth, a_motionVectors,
			a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY,
			nullptr, &outputReady);

		static sl::Result s_loggedRes = sl::Result::eErrorNotInitialized;
		static uint32_t s_loggedDims = 0;
		const uint32_t dims = (a_renderWidth << 16) | (a_outputWidth & 0xFFFF);
		if (evalRes != s_loggedRes || dims != s_loggedDims) {
			s_loggedRes = evalRes;
			s_loggedDims = dims;
			logger::info("[Streamline] DLSS evaluate result={} render={}x{} output={}x{}",
				static_cast<int>(evalRes), a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] DLSS dispatch faulted — disabling for this session");
	}
	return outputReady;
}

bool Streamline::EvaluateXeSS(ID3D11Resource* a_colorIn, ID3D11Resource* a_colorOut,
	ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_outputWidth, uint32_t a_outputHeight,
	uint32_t a_qualityMode, float a_sharpness,
	float a_jitterX, float a_jitterY)
{
	bool outputReady = false;
	if (!initialized || !featureXeSS || g_sl.dispatchFaulted)
		return false;
	if (!a_colorIn || !a_colorOut || !a_depth || !a_motionVectors)
		return false;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk->CommandResourcesReady())
		return false;
	dxvk->FlushRenderingCommands();

	__try {
		sl::XeSSMode xessMode = sl::XeSSMode::eQuality;
		switch (a_qualityMode) {
		case 0:
			xessMode = sl::XeSSMode::eNativeAA;
			break;
		case 1:
			xessMode = sl::XeSSMode::eQuality;
			break;
		case 2:
			xessMode = sl::XeSSMode::eBalanced;
			break;
		case 3:
			xessMode = sl::XeSSMode::ePerformance;
			break;
		case 4:
			xessMode = sl::XeSSMode::eUltraPerformance;
			break;
		}

		sl::XeSSOptions xessOpts{};
		xessOpts.mode = xessMode;
		xessOpts.outputWidth = a_outputWidth;
		xessOpts.outputHeight = a_outputHeight;
		xessOpts.sharpness = a_sharpness;
		xessOpts.colorBuffersHDR = sl::Boolean::eTrue;
		if (g_sl.slXeSSSetOptions(g_sl.viewport, xessOpts) != sl::Result::eOk)
			return false;

		const sl::Result evalRes = cs_EvaluateFeatureCore(sl::kFeatureXeSS, g_sl.viewport,
			a_colorIn, a_colorOut, a_depth, a_motionVectors,
			a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY,
			nullptr, &outputReady);

		static sl::Result s_loggedRes = sl::Result::eErrorNotInitialized;
		static uint32_t s_loggedDims = 0;
		const uint32_t dims = (a_renderWidth << 16) | (a_outputWidth & 0xFFFF);
		if (evalRes != s_loggedRes || dims != s_loggedDims) {
			s_loggedRes = evalRes;
			s_loggedDims = dims;
			logger::info("[Streamline] XeSS evaluate result={} render={}x{} output={}x{}",
				static_cast<int>(evalRes), a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] XeSS dispatch faulted — disabling for this session");
	}
	return outputReady;
}

bool Streamline::EvaluateFSR(ID3D11Resource* a_colorIn, ID3D11Resource* a_colorOut,
	ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_outputWidth, uint32_t a_outputHeight,
	uint32_t a_qualityMode, float a_sharpness,
	float a_jitterX, float a_jitterY)
{
	bool outputReady = false;
	if (!initialized || !featureFSR || g_sl.dispatchFaulted)
		return false;
	if (!a_colorIn || !a_colorOut || !a_depth || !a_motionVectors)
		return false;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk->CommandResourcesReady())
		return false;
	dxvk->FlushRenderingCommands();

	__try {
		sl::FSRMode fsrMode = sl::FSRMode::eMaxQuality;
		switch (a_qualityMode) {
		case 0:
			fsrMode = sl::FSRMode::eNativeAA;
			break;
		case 1:
			fsrMode = sl::FSRMode::eMaxQuality;
			break;
		case 2:
			fsrMode = sl::FSRMode::eBalanced;
			break;
		case 3:
			fsrMode = sl::FSRMode::eMaxPerformance;
			break;
		case 4:
			fsrMode = sl::FSRMode::eUltraPerformance;
			break;
		}

		sl::FSROptions fsrOpts{};
		fsrOpts.mode = fsrMode;
		fsrOpts.outputWidth = a_outputWidth;
		fsrOpts.outputHeight = a_outputHeight;
		fsrOpts.sharpness = a_sharpness;
		fsrOpts.colorBuffersHDR = sl::Boolean::eTrue;
		if (g_sl.slFSRSetOptions(g_sl.viewport, fsrOpts) != sl::Result::eOk)
			return false;

		const sl::Result evalRes = cs_EvaluateFeatureCore(sl::kFeatureFSR, g_sl.viewport,
			a_colorIn, a_colorOut, a_depth, a_motionVectors,
			a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY,
			nullptr, &outputReady);

		static sl::Result s_loggedRes = sl::Result::eErrorNotInitialized;
		static uint32_t s_loggedDims = 0;
		const uint32_t dims = (a_renderWidth << 16) | (a_outputWidth & 0xFFFF);
		if (evalRes != s_loggedRes || dims != s_loggedDims) {
			s_loggedRes = evalRes;
			s_loggedDims = dims;
			logger::info("[Streamline] FSR evaluate result={} render={}x{} output={}x{}",
				static_cast<int>(evalRes), a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] FSR dispatch faulted — disabling for this session");
	}
	return outputReady;
}

void Streamline::EvaluateFSRFrameGen(ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	ID3D11Resource* a_hudlessColor, ID3D11Resource* a_uiColor,
	uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_outputWidth, uint32_t a_outputHeight,
	float a_jitterX, float a_jitterY)
{
	// Isolate FSR frame-generation preparation from viewport 0 upscaling tags and constants.
	if (!initialized || !featureFSRFG || g_sl.dispatchFaulted)
		return;
	if (!a_depth || !a_motionVectors)
		return;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk->CommandResourcesReady())
		return;
	dxvk->FlushRenderingCommands();

	__try {
		const sl::ViewportHandle fgViewport{ 1 };
		const sl::Result evalRes = cs_EvaluateFeatureCore(sl::kFeatureFSR_G, fgViewport,
			nullptr, nullptr, a_depth, a_motionVectors,
			a_renderWidth, a_renderHeight, a_outputWidth, a_outputHeight, a_jitterX, a_jitterY,
			a_hudlessColor, nullptr, a_uiColor);

		static sl::Result s_loggedRes = sl::Result::eErrorNotInitialized;
		if (evalRes != s_loggedRes) {
			s_loggedRes = evalRes;
			logger::info("[Streamline] FSR FG-prepare result={} render={}x{}", static_cast<int>(evalRes), a_renderWidth, a_renderHeight);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] FSR FG-prepare faulted — disabling for this session");
	}
}

void Streamline::SetDLSSGMode(bool a_enable, uint32_t a_displayWidth, uint32_t a_displayHeight,
	uint32_t a_numFramesToGenerate, bool a_autoMode, bool a_dynamic, float a_dynamicTargetFps)
{
	if (!initialized || !featureDLSSG || g_sl.dispatchFaulted)
		return;

	// Do not call the options entry point while DLSS-G is runtime-unloaded.
	if (!g_dlssgCurrentlyLoaded.load(std::memory_order_acquire))
		return;

	// Clamp the requested multiplier to the reported hardware limit.
	const uint32_t maxFrames = g_sl.dlssgMaxFramesToGenerate.load(std::memory_order_acquire);
	uint32_t numFrames = a_numFramesToGenerate < 1u ? 1u : a_numFramesToGenerate;
	if (maxFrames > 0u && numFrames > maxFrames)
		numFrames = maxFrames;

	// Reissue options each frame; cached values only suppress duplicate logging.
	const bool changed = !(g_sl.dlssgModeCached && g_sl.dlssgModeOn == a_enable &&
		g_sl.dlssgCachedNumFrames == numFrames && g_sl.dlssgCachedAuto == a_autoMode &&
		g_sl.dlssgCachedDynamic == a_dynamic && g_sl.dlssgCachedDynamicFps == a_dynamicTargetFps &&
		g_sl.dlssgCachedDisplayW == a_displayWidth && g_sl.dlssgCachedDisplayH == a_displayHeight);

	// Disable DXVK frame-latency waits while Streamline owns present pacing.
	{
		static void (*s_setSkip)(uint32_t) = nullptr;
		static bool s_resolved = false;
		if (!s_resolved) {
			s_resolved = true;
			if (HMODULE m = GetModuleHandleW(L"dxvk_d3d11.dll"))
				s_setSkip = reinterpret_cast<void (*)(uint32_t)>(GetProcAddress(m, "dxvkSetSkipFrameLatencySync"));
			if (!s_setSkip)
				logger::warn("[Streamline] dxvkSetSkipFrameLatencySync not found - DLSS-G blocking mode may deadlock");
		}
		if (s_setSkip)
			s_setSkip(a_enable ? 1u : 0u);
	}

	__try {
		sl::DLSSGOptions options{};
		options.mode = !a_enable ? sl::DLSSGMode::eOff :
		               a_dynamic ? sl::DLSSGMode::eDynamic :
		               a_autoMode ? sl::DLSSGMode::eAuto :
		                            sl::DLSSGMode::eOn;
		options.numFramesToGenerate = numFrames;
		if (a_dynamic)
			options.dynamicTargetFrameRate = a_dynamicTargetFps;
		// Retain resources across temporary loading-screen and menu disables.
		options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
		options.mvecDepthWidth = a_displayWidth;
		options.mvecDepthHeight = a_displayHeight;
		options.colorWidth = a_displayWidth;
		options.colorHeight = a_displayHeight;
		// eBlockNoClientQueues requires protecting live inputs with the completion fence.
		options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockNoClientQueues;
		const sl::Result res = g_sl.slDLSSGSetOptions(g_sl.viewport, options);
		if (res != sl::Result::eOk) {
			if (changed)
				logger::warn("[Streamline] slDLSSGSetOptions failed (result {})", static_cast<int>(res));
		} else {
			g_sl.dlssgModeCached = true;
			g_sl.dlssgModeOn = a_enable;
			g_sl.dlssgCachedNumFrames = numFrames;
			g_sl.dlssgCachedAuto = a_autoMode;
			g_sl.dlssgCachedDynamic = a_dynamic;
			g_sl.dlssgCachedDynamicFps = a_dynamicTargetFps;
			g_sl.dlssgCachedDisplayW = a_displayWidth;
			g_sl.dlssgCachedDisplayH = a_displayHeight;
			if (changed)
				logger::info("[Streamline] DLSS-G mode={} ({}) numFrames={} targetFps={} (max {}) display={}x{}", a_enable,
					!a_enable ? "off" : a_dynamic ? "dynamic" : a_autoMode ? "auto" : "on", numFrames, a_dynamicTargetFps, maxFrames,
					a_displayWidth, a_displayHeight);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] DLSS-G SetOptions faulted — disabling for this session");
	}
}

bool Streamline::SetFSRFrameGen(bool a_enable, uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_displayWidth, uint32_t a_displayHeight, bool a_hdr,
	bool a_debugView, bool a_debugTearLines, bool a_debugPacingLines, bool a_onlyPresentGenerated)
{
	// The caller retries until the runtime-loaded plugin accepts the option.
	if (!initialized || !featureFSRFG || !g_sl.slFSRFrameGenerationSetOptions || g_sl.dispatchFaulted)
		return false;
	if (!g_fsrfgCurrentlyLoaded.load(std::memory_order_acquire))
		return false;

	bool ok = false;
	__try {
		sl::FSRFrameGenOptions options{};
		options.enabled = a_enable ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		options.renderWidth = a_renderWidth;
		options.renderHeight = a_renderHeight;
		options.displayWidth = a_displayWidth;
		options.displayHeight = a_displayHeight;
		options.colorBuffersHDR = a_hdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		options.debugView = a_debugView ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		options.debugTearLines = a_debugTearLines ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		options.debugPacingLines = a_debugPacingLines ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		options.onlyPresentGenerated = a_onlyPresentGenerated ? sl::Boolean::eTrue : sl::Boolean::eFalse;
		const sl::Result res = g_sl.slFSRFrameGenerationSetOptions(g_sl.viewport, options);
		if (res != sl::Result::eOk) {
			logger::warn("[Streamline] slFSRFrameGenerationSetOptions failed (result {})", static_cast<int>(res));
		} else {
			ok = true;
			g_fsrfgOwnsPresent.store(a_enable, std::memory_order_release);
			if (!a_enable)
				g_sl.frameGenerationMultiplier.store(1, std::memory_order_release);
			logger::info("[Streamline] FSR frame generation {} (render {}x{} display {}x{})",
				a_enable ? "ENABLED" : "disabled", a_renderWidth, a_renderHeight, a_displayWidth, a_displayHeight);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] FSR SetFrameGen faulted — disabling for this session");
	}
	return ok;
}

void Streamline::LogFSRFrameGenStats()
{
	if (!initialized || !featureFSRFG || !g_sl.slFSRGetFrameGenState || g_sl.dispatchFaulted)
		return;
	__try {
		sl::FSRFrameGenState state{};
		if (g_sl.slFSRGetFrameGenState(g_sl.viewport, state) == sl::Result::eOk) {
			g_sl.frameGenerationMultiplier.store(
				std::max(state.numFramesActuallyPresented, 1u), std::memory_order_release);
			static uint32_t s_n = 0;
			if ((s_n++ % 120) == 0)
				logger::info("[Streamline] FSR-FG state: presentedPerFrame={} status={} vram={} MB",
					state.numFramesActuallyPresented, state.status,
					state.estimatedVRAMUsageInBytes >> 20);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
	}
}

void Streamline::QueryDLSSGCapabilities()
{
	// Streamline requires this state query on the present thread.
	if (!initialized || !featureDLSSG || g_sl.dispatchFaulted ||
		!g_dlssgCurrentlyLoaded.load(std::memory_order_acquire))
		return;
	if (g_sl.dlssgMaxFramesToGenerate.load(std::memory_order_acquire) != 0u)
		return;
	__try {
		sl::DLSSGState state{};
		if (g_sl.slDLSSGGetState(g_sl.viewport, state, nullptr) == sl::Result::eOk && state.numFramesToGenerateMax > 0u) {
			g_sl.dlssgMaxFramesToGenerate.store(state.numFramesToGenerateMax, std::memory_order_release);
			g_sl.dlssgDynamicSupported.store(state.bIsDynamicMFGSupported == sl::Boolean::eTrue, std::memory_order_release);
			logger::info("[Streamline] DLSS-G numFramesToGenerateMax = {} (max {}x multiplier), DynamicMFG supported = {}",
				state.numFramesToGenerateMax, state.numFramesToGenerateMax + 1u, state.bIsDynamicMFGSupported == sl::Boolean::eTrue);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
	}
}

uint32_t Streamline::GetDLSSGMaxFramesToGenerate() const
{
	return g_sl.dlssgMaxFramesToGenerate.load(std::memory_order_acquire);
}

uint32_t Streamline::GetFrameGenerationMultiplier() const
{
	return g_sl.frameGenerationMultiplier.load(std::memory_order_acquire);
}

bool Streamline::IsDLSSGDynamicSupported() const
{
	return g_sl.dlssgDynamicSupported.load(std::memory_order_acquire);
}

void Streamline::LogReflexStatus()
{
	if (g_sl.dispatchFaulted || !g_sl.slReflexGetState || !g_sl.dlssgModeOn)
		return;
	static uint32_t s_n = 0;
	if ((s_n++ % 120) != 0)
		return;
	__try {
		sl::ReflexState rstate{};
		if (g_sl.slReflexGetState(rstate) == sl::Result::eOk)
			logger::info("[Streamline] Reflex lowLatencyAvailable={} latencyReportAvailable={} flashDriverControlled={}",
				rstate.lowLatencyAvailable, rstate.latencyReportAvailable, rstate.flashIndicatorDriverControlled);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
	}
}

void Streamline::TagDLSSGResources(ID3D11Resource* a_depth, ID3D11Resource* a_motionVectors,
	ID3D11Resource* a_hudlessColor, uint32_t a_renderWidth, uint32_t a_renderHeight,
	uint32_t a_displayWidth, uint32_t a_displayHeight)
{
	if (!initialized || !featureDLSSG || g_sl.dispatchFaulted)
		return;
	if (!a_depth || !a_motionVectors)
		return;

	auto* dxvk = DXVKInterop::GetSingleton();
	if (!dxvk->CommandResourcesReady())
		return;

	__try {
		// Depth is snapshotted before its source is overwritten; motion remains valid until present.
		sl::FrameToken* token = RenderFrameToken();
		if (!token)
			return;

		// Cache views because DLSS-G consumes tagged resources at present time.
		VkDevice vkDevice = dxvk->GetDevice();
		auto vkCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(
			dxvk->GetDeviceProcAddr()(vkDevice, "vkCreateImageView"));
		auto vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(
			dxvk->GetDeviceProcAddr()(vkDevice, "vkDestroyImageView"));

		const auto cachedView = [&](int a_slot, VkImage a_image, VkFormat a_format) -> VkImageView {
			auto* entries = g_sl.dlssgViewCache[a_slot];
			for (int i = 0; i < 4; ++i)
				if (entries[i].image == a_image && entries[i].view != VK_NULL_HANDLE)
					return entries[i].view;
			int idx = -1;
			for (int i = 0; i < 4; ++i)
				if (entries[i].view == VK_NULL_HANDLE) {
					idx = i;
					break;
				}
			if (idx < 0) {
				idx = g_sl.dlssgViewEvict[a_slot];
				g_sl.dlssgViewEvict[a_slot] = (g_sl.dlssgViewEvict[a_slot] + 1) % 4;
				if (entries[idx].view != VK_NULL_HANDLE && vkDestroyImageView)
					vkDestroyImageView(vkDevice, entries[idx].view, nullptr);
			}
			auto& c = entries[idx];
			c.view = VK_NULL_HANDLE;
			c.image = a_image;
			if (vkCreateImageView) {
				VkImageViewCreateInfo ci{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
				ci.image = a_image;
				ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
				ci.format = a_format;
				ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				if (a_format == VK_FORMAT_D32_SFLOAT || a_format == VK_FORMAT_D24_UNORM_S8_UINT ||
					a_format == VK_FORMAT_D16_UNORM || a_format == VK_FORMAT_D32_SFLOAT_S8_UINT)
					ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
				ci.subresourceRange.levelCount = 1;
				ci.subresourceRange.layerCount = 1;
				vkCreateImageView(vkDevice, &ci, nullptr, &c.view);
			}
			return c.view;
		};

		const auto makeResource = [&](ID3D11Resource* a_res, sl::Resource& a_out, int a_slot) {
			VkImage image = VK_NULL_HANDLE;
			VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
			VkImageCreateInfo info{ VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
			if (!dxvk->GetVkImage(a_res, &image, &layout, &info) || image == VK_NULL_HANDLE)
				return false;
			a_out = sl::Resource{ sl::ResourceType::eTex2d, image, nullptr, cachedView(a_slot, image, info.format), static_cast<uint32_t>(layout) };
			// Resource dimensions describe the image; tag extents describe the valid subrect.
			a_out.width = info.extent.width;
			a_out.height = info.extent.height;
			a_out.nativeFormat = static_cast<uint32_t>(info.format);
			a_out.mipLevels = info.mipLevels;
			a_out.arrayLayers = info.arrayLayers;
			a_out.usage = static_cast<uint32_t>(info.usage);
			a_out.flags = static_cast<uint32_t>(info.flags);
			return true;
		};

		sl::Resource depthRes{}, mvecRes{};
		if (!makeResource(a_depth, depthRes, 0) ||
			!makeResource(a_motionVectors, mvecRes, 1))
			return;

		// DLSS-G reconstructs motion on the depth grid.
		mvecRes.width = depthRes.width;
		mvecRes.height = depthRes.height;

		sl::Extent extent{};
		extent.width = a_renderWidth;
		extent.height = a_renderHeight;

		sl::ResourceTag tags[3];
		uint32_t tagCount = 0;
		tags[tagCount++] = { &depthRes, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &extent };
		tags[tagCount++] = { &mvecRes, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extent };

		// HUD-less color uses display dimensions rather than the render subrect.
		sl::Extent displayExtent{};
		displayExtent.width = a_displayWidth;
		displayExtent.height = a_displayHeight;
		sl::Resource hudlessRes{};
		if (a_hudlessColor && makeResource(a_hudlessColor, hudlessRes, 2))
			tags[tagCount++] = { &hudlessRes, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &displayExtent };
		else
			// Clear stale HUD-less input when capture is unavailable.
			tags[tagCount++] = { nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, nullptr };

		VkCommandBuffer cmd = dxvk->BeginFrameCommandBuffer();
		if (cmd == VK_NULL_HANDLE)
			return;

		g_sl.slSetTagForFrame(*token, g_sl.viewport, tags, tagCount, cmd);
		dxvk->SubmitFrameCommandBuffer(cmd, false);
		g_sl.dlssgTaggedThisFrame = true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] DLSS-G tag faulted — disabling for this session");
	}
}

void Streamline::ClearDLSSGTags()
{
	if (!initialized || !featureDLSSG || g_sl.dispatchFaulted)
		return;

	__try {
		sl::FrameToken* token = RenderFrameToken();
		if (!token)
			return;

		// Null tags force passthrough when interpolation inputs are unavailable.
		sl::ResourceTag tags[] = {
			sl::ResourceTag{ nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, nullptr },
			sl::ResourceTag{ nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, nullptr },
			sl::ResourceTag{ nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, nullptr },
		};
		VkCommandBuffer cmd = VK_NULL_HANDLE;
		auto* dxvk = DXVKInterop::GetSingleton();
		if (dxvk->CommandResourcesReady())
			cmd = dxvk->BeginFrameCommandBuffer();
		g_sl.slSetTagForFrame(*token, g_sl.viewport, tags, static_cast<uint32_t>(std::size(tags)), cmd);
		if (cmd != VK_NULL_HANDLE)
			dxvk->SubmitFrameCommandBuffer(cmd, false);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_sl.dispatchFaulted = true;
		logger::error("[Streamline] DLSS-G clear-tags faulted — disabling for this session");
	}
}

void Streamline::EnsureDLSSGPresentTag()
{
	// Supply passthrough tags when the render pass did not provide interpolation inputs.
	if (!initialized || !featureDLSSG || g_sl.dispatchFaulted)
		return;
	if (g_sl.dlssgTaggedThisFrame)
		return;
	ClearDLSSGTags();
}

void Streamline::RegisterDxvkOwnershipPredicate()
{
	// Streamline-owned swapchains must bypass DXVK's present-wait worker.
	HMODULE dxvkModule = GetModuleHandleW(L"dxvk_d3d11.dll");
	if (!dxvkModule) {
		logger::warn("[Streamline] DXVK module not loaded — cannot register ownership predicate");
		return;
	}
	using SetQueryFn = void (*)(bool (*)(VkSwapchainKHR));
	auto setQuery = reinterpret_cast<SetQueryFn>(GetProcAddress(dxvkModule, "dxvkSetFrameGenOwnershipQuery"));
	if (!setQuery) {
		logger::warn("[Streamline] dxvkSetFrameGenOwnershipQuery not found in DXVK module");
		return;
	}
	setQuery(&DxvkFrameGenerationOwnsSwapchain);
	logger::info("[Streamline] registered DXVK frame-generation ownership predicate");

	// Streamline features may only be loaded or unloaded while no swapchain exists.
	using SetTornDownFn = void (*)(void (*)());
	if (auto setTornDown = reinterpret_cast<SetTornDownFn>(GetProcAddress(dxvkModule, "dxvkSetSwapchainTornDownCallback"))) {
		setTornDown(&DxvkSwapchainTornDownCallback);
		logger::info("[Streamline] registered DXVK swapchain-torn-down callback (DLSS-G load/unload)");
	} else {
		logger::warn("[Streamline] dxvkSetSwapchainTornDownCallback not found — DLSS-G stays resident when disabled");
	}

	// The optional bridge moves present markers to DXVK's submit thread.
	if (char v[2] = {}; GetEnvironmentVariableA("CS_SL_PRESENT_MARKER_BRIDGE", v, sizeof(v)) && v[0] == '1') {
		using SetPresentMarkerCbFn = void (*)(void (*)(uint64_t, uint32_t));
		auto setMarkerCb = reinterpret_cast<SetPresentMarkerCbFn>(GetProcAddress(dxvkModule, "dxvkSetPresentMarkerCallback"));
		auto pushFrameId = reinterpret_cast<void (*)(uint64_t)>(GetProcAddress(dxvkModule, "dxvkPushPresentAppFrameId"));
		if (setMarkerCb && pushFrameId) {
			setMarkerCb(&CS_DxvkPresentMarkerBridge);
			g_sl.dxvkPushPresentAppFrameId = pushFrameId;
			logger::info("[Streamline] registered DXVK present-marker bridge (markers at the real present)");
		} else {
			logger::warn("[Streamline] DXVK present-marker bridge exports not found — present markers stay on the render thread");
		}
	}
}

bool Streamline::PresentMarkersBridged() const
{
	return g_sl.dxvkPushPresentAppFrameId != nullptr;
}

void Streamline::NotifyPresentQueued()
{
	if (g_sl.dxvkPushPresentAppFrameId)
		g_sl.dxvkPushPresentAppFrameId(g_sl.renderFrameId);
}

void Streamline::SetDLSSGDesiredLoaded(bool a_loaded)
{
	g_dlssgDesiredLoaded.store(a_loaded, std::memory_order_release);
}

bool Streamline::IsDLSSGLoaded() const
{
	return g_dlssgCurrentlyLoaded.load(std::memory_order_acquire);
}

bool Streamline::IsDLSSGLoadSettled() const
{
	return g_dlssgDesiredLoaded.load(std::memory_order_acquire) ==
	       g_dlssgCurrentlyLoaded.load(std::memory_order_acquire);
}

void Streamline::SetFSRFGDesiredLoaded(bool a_loaded)
{
	g_fsrfgDesiredLoaded.store(a_loaded, std::memory_order_release);
}

bool Streamline::IsFSRFGLoaded() const
{
	return g_fsrfgCurrentlyLoaded.load(std::memory_order_acquire);
}

bool Streamline::IsFSRFGLoadSettled() const
{
	return g_fsrfgDesiredLoaded.load(std::memory_order_acquire) ==
	       g_fsrfgCurrentlyLoaded.load(std::memory_order_acquire);
}

void Streamline::RequestDxvkSwapchainRecreate(const char* a_reason)
{
	// Recreate the Vulkan swapchain to apply runtime feature load changes.
	static auto requestRecreate = []() -> void (*)() {
		HMODULE dxvkModule = GetModuleHandleW(L"dxvk_d3d11.dll");
		if (!dxvkModule)
			return nullptr;
		return reinterpret_cast<void (*)()>(GetProcAddress(dxvkModule, "dxvkRequestSwapchainRecreate"));
	}();
	if (requestRecreate) {
		requestRecreate();
		logger::info("[Streamline] requested DXVK swapchain recreate ({})", a_reason);
	} else {
		logger::warn("[Streamline] dxvkRequestSwapchainRecreate not found — {} cannot take effect", a_reason);
	}
}

void Streamline::PushDxvkSyncPresent(bool a_sync)
{
	// Frame-generation proxies require present to complete before the D3D11 hook returns.
	static auto setSync = []() -> void (*)(uint32_t) {
		HMODULE dxvkModule = GetModuleHandleW(L"dxvk_d3d11.dll");
		if (!dxvkModule)
			return nullptr;
		return reinterpret_cast<void (*)(uint32_t)>(GetProcAddress(dxvkModule, "dxvkSetSyncPresent"));
	}();
	if (setSync) {
		setSync(a_sync ? 1u : 0u);
	} else {
		static bool s_warned = false;
		if (!s_warned) {
			s_warned = true;
			logger::warn("[Streamline] dxvkSetSyncPresent not found - synchronous present control inactive");
		}
	}
}
