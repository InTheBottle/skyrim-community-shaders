#include "FidelityFX.h"

#include "State.h"
#include <Util.h>

FfxErrorCode FidelityFX::Initialize(uint32_t a_maxContexts)
{
	auto state = State::GetSingleton();

	logger::info("[FidelityFX] Initialising");

	const auto fsrDevice = ffxGetDeviceDX11(state->device);

	size_t scratchBufferSize = ffxGetScratchMemorySizeDX11(a_maxContexts);
	void* scratchBuffer = malloc(scratchBufferSize);
	memset(scratchBuffer, 0, scratchBufferSize);

	auto errorCode = ffxGetInterfaceDX11(&ffxInterface, fsrDevice, scratchBuffer, scratchBufferSize, a_maxContexts);

	if (errorCode == FFX_OK) {
		logger::info("[FidelityFX] Successfully initialised");
	} else {
		logger::error("[FidelityFX] Failed to initialise!");
	}

	dx11CommandList = ffxGetCommandListDX11(state->context);

	return errorCode;
}

typedef enum Fsr3BackendTypes : uint32_t
{
	FSR3_BACKEND_SHARED_RESOURCES,
	FSR3_BACKEND_UPSCALING,
	FSR3_BACKEND_FRAME_INTERPOLATION,
	FSR3_BACKEND_COUNT
} Fsr3BackendTypes;

// register a DX11 resource to the backend
FfxResource ffxGetResource(ID3D11Resource* dx11Resource,
	[[maybe_unused]] wchar_t const* ffxResName,
	FfxResourceStates state /*=FFX_RESOURCE_STATE_COMPUTE_READ*/)
{
	FfxResource resource = {};
	resource.resource = reinterpret_cast<void*>(const_cast<ID3D11Resource*>(dx11Resource));
	resource.state = state;
	resource.description = GetFfxResourceDescriptionDX11(dx11Resource);

#ifdef _DEBUG
	if (ffxResName) {
		wcscpy_s(resource.name, ffxResName);
	}
#endif

	return resource;
}

void FidelityFX::SetupUpscalingResources()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

	main.texture->GetDesc(&texDesc);
	main.SRV->GetDesc(&srvDesc);
	main.RTV->GetDesc(&rtvDesc);
	main.UAV->GetDesc(&uavDesc);

	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.Format = texDesc.Format;
	rtvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	colorInputOutput = new Texture2D(texDesc);
	colorInputOutput->CreateSRV(srvDesc);
	colorInputOutput->CreateRTV(rtvDesc);
	colorInputOutput->CreateUAV(uavDesc);
}

FfxErrorCode FidelityFX::InitializeFSR3()
{
	auto state = State::GetSingleton();

	FfxInterface ffxFsrInterface;
	const auto fsrDevice = ffxGetDeviceDX11(state->device);

	const size_t scratchBufferSize = ffxGetScratchMemorySizeDX11(FFX_FSR3UPSCALER_CONTEXT_COUNT);
	void* scratchBuffer = calloc(scratchBufferSize, 1);
	memset(scratchBuffer, 0, scratchBufferSize);
	FfxErrorCode errorCode = ffxGetInterfaceDX11(&ffxFsrInterface, fsrDevice, scratchBuffer, scratchBufferSize, FFX_FSR3UPSCALER_CONTEXT_COUNT);
	
	if (errorCode == FFX_OK) {
		logger::info("[FidelityFX] Successfully initialised FSR3 backend interface");
	} else {
		logger::error("[FidelityFX] Failed to initialise FSR3 backend interface!");
		return errorCode;
	}

	FfxFsr3ContextDescription contextDescription;
	contextDescription.maxRenderSize.width = (uint)state->screenSize.x;
	contextDescription.maxRenderSize.height = (uint)state->screenSize.y;
	contextDescription.maxUpscaleSize.width = (uint)state->screenSize.x;
	contextDescription.maxUpscaleSize.height = (uint)state->screenSize.y;
	contextDescription.displaySize.width = (uint)state->screenSize.x;
	contextDescription.displaySize.height = (uint)state->screenSize.y;
	contextDescription.flags = FFX_FSR3_ENABLE_UPSCALING_ONLY | FFX_FSR3_ENABLE_AUTO_EXPOSURE;
	contextDescription.backBufferFormat = FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;

	contextDescription.backendInterfaceUpscaling = ffxFsrInterface;
	contextDescription.backendInterfaceFrameInterpolation = ffxFsrInterface;

	errorCode = ffxFsr3ContextCreate(&fsrContext, &contextDescription);

	if (errorCode == FFX_OK) {
		logger::info("[FidelityFX] Successfully initialised FSR3 context");
	} else {
		logger::error("[FidelityFX] Failed to initialise FSR3 context!");
		return errorCode;
	}

	SetupUpscalingResources();

	return errorCode;
}

// https://github.com/PureDark/Skyrim-Upscaler/blob/fa057bb088cf399e1112c1eaba714590c881e462/src/SkyrimUpscaler.cpp#L88
float GetVerticalFOVRad()
{
	static float& fac = (*(float*)(RELOCATION_ID(513786, 388785).address()));
	const auto base = fac;
	const auto x = base / 1.30322540f;
	auto state = State::GetSingleton();
	const auto vFOV = 2 * atan(x / (state->screenSize.x / state->screenSize.y));
	return vFOV;
}

void BSGraphics_SetDirtyStates(bool isCompute);

extern decltype(&BSGraphics_SetDirtyStates) ptr_BSGraphics_SetDirtyStates;

void FidelityFX::Upscale()
{
	(ptr_BSGraphics_SetDirtyStates)(false);  // Our hook skips this call so we need to call manually

	auto state = State::GetSingleton();
	state->BeginPerfEvent("Upscaling");

	auto& context = state->context;

	ID3D11ShaderResourceView* inputTextureSRV;
	context->PSGetShaderResources(0, 1, &inputTextureSRV);

	inputTextureSRV->Release();

	ID3D11RenderTargetView* outputTextureRTV;
	ID3D11DepthStencilView* dsv;
	context->OMGetRenderTargets(1, &outputTextureRTV, &dsv);
	context->OMSetRenderTargets(0, nullptr, nullptr);

	outputTextureRTV->Release();

	if (dsv)
		dsv->Release();

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	auto& depthTexture = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	auto& motionVectorsTexture = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

	// Fix motion vectors not resetting when the game is paused
	if (RE::UI::GetSingleton()->GameIsPaused()) {
		float clearColor[4] = { 0, 0, 0, 0 };
		auto& motionVectorsBuffer = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::RENDER_TARGET::kMOTION_VECTOR];
		context->ClearRenderTargetView(motionVectorsBuffer.RTV, clearColor);
	}

	ID3D11Resource* inputTextureResource;
	inputTextureSRV->GetResource(&inputTextureResource);

	ID3D11Resource* outputTextureResource;
	outputTextureRTV->GetResource(&outputTextureResource);

	context->CopyResource(colorInputOutput->resource.get(), inputTextureResource);

	{
		FfxFsr3DispatchUpscaleDescription dispatchParameters{};

		dispatchParameters.commandList = dx11CommandList;
		dispatchParameters.color = ffxGetResource(colorInputOutput->resource.get(), L"FSR3_Input_OutputColor", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchParameters.depth = ffxGetResource(depthTexture.texture, L"FSR3_InputDepth", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchParameters.motionVectors = ffxGetResource(motionVectorsTexture.texture, L"FSR3_InputMotionVectors", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchParameters.exposure = ffxGetResource(nullptr, L"FSR3_InputExposure", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchParameters.upscaleOutput = dispatchParameters.color;
		dispatchParameters.reactive = ffxGetResource(nullptr, L"FSR3_InputReactiveMap", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);
		dispatchParameters.transparencyAndComposition = ffxGetResource(nullptr, L"FSR3_TransparencyAndCompositionMap", FFX_RESOURCE_STATE_PIXEL_COMPUTE_READ);

		static auto gameViewport = RE::BSGraphics::State::GetSingleton();
		dispatchParameters.jitterOffset.x = gameViewport->projectionPosScaleX;
		dispatchParameters.jitterOffset.y = gameViewport->projectionPosScaleY;

		dispatchParameters.motionVectorScale.x = state->screenSize.x;
		dispatchParameters.motionVectorScale.y = state->screenSize.y;

		dispatchParameters.reset = false;

		dispatchParameters.enableSharpening = true;
		dispatchParameters.sharpness = 1.0f;

		static float* deltaTime = (float*)REL::RelocationID(523660, 410199).address();  // 2F6B948, 30064C8
		dispatchParameters.frameTimeDelta = *deltaTime * 1000.f;                        // Milliseconds!

		dispatchParameters.preExposure = 1.0f;

		dispatchParameters.renderSize.width = (uint)state->screenSize.x;
		dispatchParameters.renderSize.height = (uint)state->screenSize.y;

		dispatchParameters.cameraFovAngleVertical = GetVerticalFOVRad();

		static float& cameraNear = (*(float*)(REL::RelocationID(517032, 403540).address() + 0x40));  // 2F26FC0, 2FC1A90
		static float& cameraFar = (*(float*)(REL::RelocationID(517032, 403540).address() + 0x44));   // 2F26FC4, 2FC1A94

		dispatchParameters.cameraFar = cameraFar;
		dispatchParameters.cameraNear = cameraNear;

		dispatchParameters.viewSpaceToMetersFactor = viewScale;

		FfxErrorCode errorCode = ffxFsr3ContextDispatchUpscale(&fsrContext, &dispatchParameters);
		if (errorCode != FFX_OK) {
			logger::error("[FidelityFX] Failed to dispatch upscaling!");
		}
	}

	context->CopyResource(outputTextureResource, colorInputOutput->resource.get());

	state->EndPerfEvent();

}
