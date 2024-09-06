#include "FidelityFX.h"

#include "State.h"
#include <Util.h>

void FidelityFX::DrawSettings()
{
	if (ImGui::CollapsingHeader("Fidelity FX", ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick)) {
		ImGui::Checkbox("Super Resolution", &enableSuperResolution);
		ImGui::SliderFloat("Sharpness", &sharpness, 0.0f, 1.0f, "%.1f");
	}
}

FfxErrorCode FidelityFX::Initialize()
{
	auto state = State::GetSingleton();

	logger::info("[FidelityFX] Initializing");

	const size_t scratchBufferSize = ffxFsr2GetScratchMemorySizeDX11();
	void* scratchBuffer = malloc(scratchBufferSize);
	FfxErrorCode errorCode = ffxFsr2GetInterfaceDX11(&initializationParameters.callbacks, state->device, scratchBuffer, scratchBufferSize);
	FFX_ASSERT(errorCode == FFX_OK);

	initializationParameters.device = ffxGetDeviceDX11(state->device);
	initializationParameters.maxRenderSize.width = (uint)state->screenSize.x;
	initializationParameters.maxRenderSize.height = (uint)state->screenSize.y;
	initializationParameters.displaySize.width = (uint)state->screenSize.x;
	initializationParameters.displaySize.height = (uint)state->screenSize.y;
	initializationParameters.flags = FFX_FSR2_ENABLE_AUTO_EXPOSURE;

	errorCode = ffxFsr2ContextCreate(&fsrContext, &initializationParameters);

	if (errorCode == FFX_OK) {
		logger::info("[FidelityFX] Successfully initialized FSR2 backend interfaces");
	} else {
		logger::error("[FidelityFX] Failed to initialize FSR2 backend interfaces!");
		return errorCode;
	}

	CreateUpscalingResources();

	return errorCode;
}

void FidelityFX::CreateUpscalingResources()
{
	logger::info("[FidelityFX] Creating upscaling resources");

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

	upscalingTempTexture = new Texture2D(texDesc);
	upscalingTempTexture->CreateSRV(srvDesc);
	upscalingTempTexture->CreateRTV(rtvDesc);
	upscalingTempTexture->CreateUAV(uavDesc);
}

void BSGraphics_SetDirtyStates(bool isCompute);

extern decltype(&BSGraphics_SetDirtyStates) ptr_BSGraphics_SetDirtyStates;

void FidelityFX::ReplaceJitter()
{
	auto state = State::GetSingleton();

	const int32_t jitterPhaseCount = ffxFsr2GetJitterPhaseCount((uint)state->screenSize.x, (uint)state->screenSize.x);
	
	auto viewport = RE::BSGraphics::State::GetSingleton();

	float jitterX = 0;
	float jitterY = 0;
	ffxFsr2GetJitterOffset(&jitterX, &jitterY, viewport->frameCount, jitterPhaseCount);
	
	viewport->projectionPosScaleX = 2.0f * jitterX / state->screenSize.x;
	viewport->projectionPosScaleY = -2.0f * jitterY / state->screenSize.y;
}

void FidelityFX::DispatchUpscaling()
{
	(ptr_BSGraphics_SetDirtyStates)(false); // Our hook skips this call so we need to call manually
	
	auto state = State::GetSingleton();
	state->BeginPerfEvent("DispatchUpscaling");

	auto& context = state->context;

	ID3D11ShaderResourceView* backupSrv;
	context->PSGetShaderResources(0, 1, &backupSrv);
		
	backupSrv->Release();

	ID3D11RenderTargetView* backupRtv;
	ID3D11DepthStencilView* backupDsv;
	context->OMGetRenderTargets(1, &backupRtv, &backupDsv);
	context->OMSetRenderTargets(0, nullptr, nullptr);

	backupRtv->Release();

	if (backupDsv)
		backupDsv->Release();
	
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto& motionVectors = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

	ID3D11Resource* inputTexture;
	backupSrv->GetResource(&inputTexture);

	ID3D11Resource* outputTexture;
	backupRtv->GetResource(&outputTexture);

	{
		FfxFsr2DispatchDescription dispatchParameters = {};
		dispatchParameters.commandList = context;
		dispatchParameters.color = ffxGetResourceDX11(&fsrContext, inputTexture, L"FSR2_InputColor");
		dispatchParameters.depth = ffxGetResourceDX11(&fsrContext, depth.texture, L"FSR2_InputDepth");
		dispatchParameters.motionVectors = ffxGetResourceDX11(&fsrContext, motionVectors.texture, L"FSR2_InputMotionVectors");
		dispatchParameters.exposure = ffxGetResourceDX11(&fsrContext, nullptr, L"FSR2_InputExposure");

		dispatchParameters.reactive = ffxGetResourceDX11(&fsrContext, nullptr, L"FSR2_ReactiveMap");
		dispatchParameters.transparencyAndComposition = ffxGetResourceDX11(&fsrContext, nullptr, L"FSR2_TransparencyAndCompositionMap");
				
		dispatchParameters.output = ffxGetResourceDX11(&fsrContext, upscalingTempTexture->resource.get(), L"FSR2_OutputUpscaledColor", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
			
		auto viewport = RE::BSGraphics::State::GetSingleton();
		dispatchParameters.jitterOffset.x = viewport->projectionPosScaleX;
		dispatchParameters.jitterOffset.y = viewport->projectionPosScaleY;

		dispatchParameters.motionVectorScale.x = state->screenSize.x;
		dispatchParameters.motionVectorScale.y = state->screenSize.y;
		dispatchParameters.reset = false;
		dispatchParameters.enableSharpening = sharpness > 0.0f;
		dispatchParameters.sharpness = sharpness;

		static float* g_deltaTime = (float*)REL::RelocationID(523660, 410199).address();
		dispatchParameters.frameTimeDelta = *g_deltaTime * 1000.0f;

		dispatchParameters.preExposure = 1.0f;
		dispatchParameters.renderSize.width = (uint)state->screenSize.x;
		dispatchParameters.renderSize.height = (uint)state->screenSize.y;
		dispatchParameters.cameraFar = (*(float*)(REL::RelocationID(517032, 403540).address() + 0x44));
		dispatchParameters.cameraNear = (*(float*)(REL::RelocationID(517032, 403540).address() + 0x40));
		dispatchParameters.cameraFovAngleVertical = Util::GetVerticalFOVRad();
		dispatchParameters.viewSpaceToMetersFactor = 0.01428222656f;

		FfxErrorCode errorCode = ffxFsr2ContextDispatch(&fsrContext, &dispatchParameters);
		FFX_ASSERT(errorCode == FFX_OK);
	}

	context->CopyResource(outputTexture, upscalingTempTexture->resource.get());

	state->EndPerfEvent();	
}