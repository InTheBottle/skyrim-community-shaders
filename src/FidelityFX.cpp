#include "FidelityFX.h"

#include "State.h"

FfxErrorCode FidelityFX::Initialize(uint32_t a_maxContexts)
{
	auto state = State::GetSingleton();

	logger::info("[FidelityFX] Initialising");

	const auto fsrDevice = ffxGetDeviceDX11(state->device);
	const auto scratchSize = ffxGetScratchMemorySizeDX11(a_maxContexts);

   	size_t scratchBufferSize = ffxGetScratchMemorySizeDX11(a_maxContexts);
	void* scratchBuffer = malloc(scratchBufferSize);
	memset(scratchBuffer, 0, scratchBufferSize);

	auto errorCode = ffxGetInterfaceDX11(&ffxInterface, fsrDevice, scratchBuffer, scratchBufferSize, a_maxContexts);

	if (errorCode == FFX_OK)
	{
		logger::info("[FidelityFX] Successfully initialised");
	} else {
		logger::error("[FidelityFX] Failed to initialise!");
	}

	return errorCode;
}

 typedef enum Fsr3BackendTypes : uint32_t
{
	FSR3_BACKEND_SHARED_RESOURCES,
	FSR3_BACKEND_UPSCALING,
	FSR3_BACKEND_FRAME_INTERPOLATION,
	FSR3_BACKEND_COUNT
} Fsr3BackendTypes;

FfxErrorCode FidelityFX::InitializeFSR3()
{
	auto state = State::GetSingleton();

	//FfxFrameInterpolationContextDescription fgDescription;
	//fgDescription.flags = 0;
	//fgDescription.maxRenderSize = { (uint)State::GetSingleton()->screenSize.x, (uint)State::GetSingleton()->screenSize.y };
	//fgDescription.displaySize = fgDescription.maxRenderSize;
	//fgDescription.backBufferFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;
	//fgDescription.backendInterface = ffxInterface;

	//auto errorCode = ffxFrameInterpolationContextCreate(&fgContext, &fgDescription);

	//if (errorCode == FFX_OK) {
	//	logger::info("[FidelityFX] Successfully initialised Frame Generation");
	//} else {
	//	logger::error("[FidelityFX] Failed to initialise Frame Generation!");
	//	return errorCode;
	//}
	//return errorCode;

	FfxErrorCode errorCode = 0;
	FfxInterface ffxFsr3Backends_[FSR3_BACKEND_COUNT] = {};
	const auto fsrDevice = ffxGetDeviceDX11(state->device);

	int effectCounts[] = { 1, 1, 2 };
	for (auto i = 0; i < FSR3_BACKEND_COUNT; i++) {
		const size_t scratchBufferSize = ffxGetScratchMemorySizeDX11(effectCounts[i]);
		void* scratchBuffer = calloc(scratchBufferSize, 1);
		memset(scratchBuffer, 0, scratchBufferSize);
		errorCode |= ffxGetInterfaceDX11(&ffxFsr3Backends_[i], fsrDevice, scratchBuffer, scratchBufferSize, effectCounts[i]);
	}

	if (errorCode == FFX_OK) {
		logger::info("[FidelityFX] Sucessfully initialised FSR3 backend interfaces");
	} else {
		logger::error("[FidelityFX] Failed to initialise FSR3 backend interfaces!");
		return errorCode;
	}

	FfxFsr3ContextDescription contextDescription;
	contextDescription.maxRenderSize.width = (uint)state->screenSize.x;
	contextDescription.maxRenderSize.height = (uint)state->screenSize.y;
	contextDescription.upscaleOutputSize.width = (uint)state->screenSize.x;
	contextDescription.upscaleOutputSize.height = (uint)state->screenSize.y;
	contextDescription.displaySize.width = (uint)state->screenSize.x;
	contextDescription.displaySize.height = (uint)state->screenSize.y;
	contextDescription.flags = FFX_FSR3_ENABLE_AUTO_EXPOSURE;
	contextDescription.backBufferFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;

	contextDescription.backendInterfaceSharedResources = ffxFsr3Backends_[FSR3_BACKEND_SHARED_RESOURCES];
	contextDescription.backendInterfaceUpscaling = ffxFsr3Backends_[FSR3_BACKEND_UPSCALING];
	contextDescription.backendInterfaceFrameInterpolation = ffxFsr3Backends_[FSR3_BACKEND_FRAME_INTERPOLATION];

	errorCode = ffxFsr3ContextCreate(&fsrContext, &contextDescription);

	if (errorCode == FFX_OK) {
		logger::info("[FidelityFX] Sucessfully initialised FSR3 context");
	} else {
		logger::error("[FidelityFX] Failed to initialise FSR3 context!");
		return errorCode;
	}

	auto manager = RE::BSGraphics::Renderer::GetSingleton();

	FfxSwapchain ffxSwapChain = reinterpret_cast<void*>(manager->GetRuntimeData().renderWindows->swapChain);

	FfxFrameGenerationConfig frameGenerationConfig; 
	frameGenerationConfig.frameGenerationEnabled = true;
	frameGenerationConfig.frameGenerationCallback = ffxFsr3DispatchFrameGeneration;
	frameGenerationConfig.presentCallback = nullptr;
	frameGenerationConfig.swapChain = ffxSwapChain;
	frameGenerationConfig.HUDLessColor = FfxResource({});

	errorCode = ffxFsr3ConfigureFrameGeneration(&fsrContext, &frameGenerationConfig);
	
	if (errorCode == FFX_OK) {
		logger::info("[FidelityFX] Sucessfully initialised frame generation");
	} else {
		logger::error("[FidelityFX] Failed to initialise frame generation!");
		return errorCode;
	}

	return errorCode;
}
