#include "Streamline.h"

#include <dxgi.h>

#include "Util.h"

void LoggingCallback(sl::LogType type, const char* msg)
{
	switch (type) {
	case sl::LogType::eInfo:
		logger::info("{}", msg);
		break;
	case sl::LogType::eWarn:
		logger::warn("{}", msg);
		break;
	case sl::LogType::eError:
		logger::error("{}", msg);
		break;
	}
}

void Streamline::Initialize_preDevice()
{
	logger::info("[Streamline] Initializing Streamline");

	interposer = LoadLibraryW(L"Data/SKSE/Plugins/Streamline/sl.interposer.dll");

	sl::Preferences pref;

	sl::Feature featuresToLoad[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex };
	pref.featuresToLoad = featuresToLoad;
	pref.numFeaturesToLoad = _countof(featuresToLoad);

	pref.logLevel = sl::LogLevel::eDefault;
	pref.logMessageCallback = LoggingCallback;

	const wchar_t* pathsToPlugins[] = { L"Data/SKSE/Plugins/Streamline" };
	pref.pathsToPlugins = pathsToPlugins;
	pref.numPathsToPlugins = _countof(pathsToPlugins);

	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0.0";
	pref.projectId = "f8776929-c969-43bd-ac2b-294b4de58aac";

	pref.renderAPI = sl::RenderAPI::eD3D11;
	pref.flags |= sl::PreferenceFlags::eUseManualHooking;

	// Hook up all of the functions exported by the SL Interposer Library
	slInit = (PFun_slInit*)GetProcAddress(interposer, "slInit");
	slShutdown = (PFun_slShutdown*)GetProcAddress(interposer, "slShutdown");
	slIsFeatureSupported = (PFun_slIsFeatureSupported*)GetProcAddress(interposer, "slIsFeatureSupported");
	slIsFeatureLoaded = (PFun_slIsFeatureLoaded*)GetProcAddress(interposer, "slIsFeatureLoaded");
	slSetFeatureLoaded = (PFun_slSetFeatureLoaded*)GetProcAddress(interposer, "slSetFeatureLoaded");
	slEvaluateFeature = (PFun_slEvaluateFeature*)GetProcAddress(interposer, "slEvaluateFeature");
	slAllocateResources = (PFun_slAllocateResources*)GetProcAddress(interposer, "slAllocateResources");
	slFreeResources = (PFun_slFreeResources*)GetProcAddress(interposer, "slFreeResources");
	slSetTag = (PFun_slSetTag*)GetProcAddress(interposer, "slSetTag");
	slGetFeatureRequirements = (PFun_slGetFeatureRequirements*)GetProcAddress(interposer, "slGetFeatureRequirements");
	slGetFeatureVersion = (PFun_slGetFeatureVersion*)GetProcAddress(interposer, "slGetFeatureVersion");
	slUpgradeInterface = (PFun_slUpgradeInterface*)GetProcAddress(interposer, "slUpgradeInterface");
	slSetConstants = (PFun_slSetConstants*)GetProcAddress(interposer, "slSetConstants");
	slGetNativeInterface = (PFun_slGetNativeInterface*)GetProcAddress(interposer, "slGetNativeInterface");
	slGetFeatureFunction = (PFun_slGetFeatureFunction*)GetProcAddress(interposer, "slGetFeatureFunction");
	slGetNewFrameToken = (PFun_slGetNewFrameToken*)GetProcAddress(interposer, "slGetNewFrameToken");
	slSetD3DDevice = (PFun_slSetD3DDevice*)GetProcAddress(interposer, "slSetD3DDevice");

	if (SL_FAILED(res, slInit(pref, sl::kSDKVersion))) {
		logger::error("[Streamline] Failed to initialize Streamline");
	} else {
		logger::info("[Streamline] Sucessfully initialized Streamline");
	}

	initialized = true;
}

void Streamline::Initialize_postDevice()
{
	// Hook up all of the feature functions using the sl function slGetFeatureFunction
	slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", (void*&)slDLSSGGetState);
	slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", (void*&)slDLSSGSetOptions);

	slGetFeatureFunction(sl::kFeatureReflex, "slReflexGetState", (void*&)slReflexGetState);
	slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetMarker", (void*&)slReflexSetMarker);
	slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", (void*&)slReflexSleep);
	slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", (void*&)slReflexSetOptions);

	// We set reflex consts to a default config. This can be changed at runtime in the UI.
	auto reflexOptions = sl::ReflexOptions{};
	reflexOptions.mode = sl::ReflexMode::eLowLatencyWithBoost;
	reflexOptions.useMarkersToOptimize = false;
	reflexOptions.virtualKey = 0;
	reflexOptions.frameLimitUs = 0;

	if (SL_FAILED(res, slReflexSetOptions(reflexOptions))) {
		logger::error("[Streamline] Failed to set reflex options");
	} else {
		logger::info("[Streamline] Sucessfully set reflex options");
	}
}

HRESULT Streamline::CreateDXGIFactory(REFIID riid, void** ppFactory)
{
	if (!initialized)
		Initialize_preDevice();

	logger::info("[Streamline] Proxying CreateDXGIFactory");

	auto slCreateDXGIFactory1 = reinterpret_cast<decltype(&CreateDXGIFactory1)>(GetProcAddress(interposer, "CreateDXGIFactory1"));

	return slCreateDXGIFactory1(riid, ppFactory);
}

HRESULT Streamline::CreateSwapchain(IDXGIAdapter* pAdapter,
	D3D_DRIVER_TYPE DriverType,
	HMODULE Software,
	UINT Flags,
	const D3D_FEATURE_LEVEL*,
	UINT,
	UINT SDKVersion,
	const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
	IDXGISwapChain** ppSwapChain,
	ID3D11Device** ppDevice,
	D3D_FEATURE_LEVEL*,
	ID3D11DeviceContext** ppImmediateContext)
{
	if (!initialized)
		Initialize_preDevice();

	logger::info("[Streamline] Proxying D3D11CreateDeviceAndSwapChain");

	auto slD3D11CreateDeviceAndSwapChain = reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(GetProcAddress(interposer, "D3D11CreateDeviceAndSwapChain"));

	const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;  // Create a device with only the latest feature level

	//Flags |= D3D11_CREATE_DEVICE_DEBUG;

	auto hr = slD3D11CreateDeviceAndSwapChain(
		pAdapter,
		DriverType,
		Software,
		Flags,
		&featureLevel,
		1,
		SDKVersion,
		pSwapChainDesc,
		ppSwapChain,
		ppDevice,
		nullptr,
		ppImmediateContext);

	Initialize_postDevice();

	viewport = { 0 };

	return hr;
}

void Streamline::CreateFrameGenerationResources()
{
	logger::info("[Streamline] Creating frame generation resources");

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

	texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	colorBufferShared = new Texture2D(texDesc);
	colorBufferShared->CreateSRV(srvDesc);
	colorBufferShared->CreateRTV(rtvDesc);
	colorBufferShared->CreateUAV(uavDesc);

	texDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.Format = texDesc.Format;
	rtvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	depthBufferShared = new Texture2D(texDesc);
	depthBufferShared->CreateSRV(srvDesc);
	depthBufferShared->CreateRTV(rtvDesc);
	depthBufferShared->CreateUAV(uavDesc);

	copyDepthToSharedBufferCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\Streamline\\CopyDepthToSharedBufferCS.hlsl", {}, "cs_5_0");
}

void Streamline::UpgradeGameResource(RE::RENDER_TARGET a_target)
{
	logger::info("[Streamline] Upgrading game resource {}", magic_enum::enum_name(a_target));

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	auto& data = renderer->GetRuntimeData().renderTargets[a_target];

	auto& device = State::GetSingleton()->device;

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};

	data.texture->GetDesc(&texDesc);
	data.SRV->GetDesc(&srvDesc);
	data.RTV->GetDesc(&rtvDesc);

	data.SRV->Release();
	data.RTV->Release();
	data.texture->Release();

	texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, &data.texture));
	DX::ThrowIfFailed(device->CreateShaderResourceView(data.texture, &srvDesc, &data.SRV));
	DX::ThrowIfFailed(device->CreateRenderTargetView(data.texture, &rtvDesc, &data.RTV));
}

void Streamline::UpgradeGameResources()
{
	CreateFrameGenerationResources();
	UpgradeGameResource(RE::RENDER_TARGETS::RENDER_TARGET::kMOTION_VECTOR);

	sl::DLSSGOptions options{};
	options.mode = sl::DLSSGMode::eAuto;

	if (SL_FAILED(result, slDLSSGSetOptions(viewport, options))) {
		logger::error("[Streamline] Could not enable DLSSG");
	}
}

void Streamline::CopyColorToSharedBuffer()
{
	auto& context = State::GetSingleton()->context;
	auto& swapChain = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];

	ID3D11Resource* swapChainResource;
	swapChain.SRV->GetResource(&swapChainResource);

	context->CopyResource(colorBufferShared->resource.get(), swapChainResource);
}

void Streamline::CopyDepthToSharedBuffer()
{
	auto& context = State::GetSingleton()->context;

	ID3D11RenderTargetView* backupViews[8];
	ID3D11DepthStencilView* backupDsv;
	context->OMGetRenderTargets(8, backupViews, &backupDsv);  // Backup bound render targets
	context->OMSetRenderTargets(0, nullptr, nullptr);         // Unbind all bound render targets

	auto& depth = RE::BSGraphics::Renderer::GetSingleton()->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	auto dispatchCount = Util::GetScreenDispatchCount();

	{
		ID3D11ShaderResourceView* views[1] = { depth.depthSRV };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[1] = { depthBufferShared->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(copyDepthToSharedBufferCS, nullptr, 0);

		context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
	}

	ID3D11ShaderResourceView* views[1] = { nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(views), views);

	ID3D11UnorderedAccessView* uavs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

	ID3D11ComputeShader* shader = nullptr;
	context->CSSetShader(shader, nullptr, 0);

	context->OMSetRenderTargets(8, backupViews, backupDsv);  // Restore all bound render targets

	for (int i = 0; i < 8; i++) {
		if (backupViews[i])
			backupViews[i]->Release();
	}

	if (backupDsv)
		backupDsv->Release();
}

void Streamline::Present()
{
	static bool currentEnableFrameGeneration = enableFrameGeneration;
	if (currentEnableFrameGeneration != enableFrameGeneration) {
		currentEnableFrameGeneration = enableFrameGeneration;

		sl::DLSSGOptions options{};
		options.mode = currentEnableFrameGeneration ? sl::DLSSGMode::eAuto : sl::DLSSGMode::eOff;

		if (SL_FAILED(result, slDLSSGSetOptions(viewport, options))) {
			logger::error("[Streamline] Could not set DLSSG");
		}
	}

	// Fake NVIDIA Reflex useage to prevent DLSS FG errors
	slReflexSetMarker(sl::ReflexMarker::eInputSample, *currentFrame);
	slReflexSetMarker(sl::ReflexMarker::eSimulationStart, *currentFrame);
	slReflexSetMarker(sl::ReflexMarker::eSimulationEnd, *currentFrame);
	slReflexSetMarker(sl::ReflexMarker::eRenderSubmitStart, *currentFrame);
	slReflexSetMarker(sl::ReflexMarker::eRenderSubmitEnd, *currentFrame);
	slReflexSetMarker(sl::ReflexMarker::ePresentStart, *currentFrame);
	slReflexSetMarker(sl::ReflexMarker::ePresentEnd, *currentFrame);

	CopyDepthToSharedBuffer();

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	auto& motionVectorsBuffer = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::RENDER_TARGET::kMOTION_VECTOR];

	auto state = State::GetSingleton();

	sl::Extent fullExtent{ 0, 0, (uint)state->screenSize.x, (uint)state->screenSize.y };

	sl::Resource depth = { sl::ResourceType::eTex2d, depthBufferShared->resource.get(), 0 };
	sl::ResourceTag depthTag = sl::ResourceTag{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };

	sl::Resource mvec = { sl::ResourceType::eTex2d, motionVectorsBuffer.texture, 0 };
	sl::ResourceTag mvecTag = sl::ResourceTag{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };

	sl::Resource hudLess = { sl::ResourceType::eTex2d, colorBufferShared->resource.get(), 0 };
	sl::ResourceTag hudLessTag = sl::ResourceTag{ &hudLess, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };

	sl::Resource ui = { sl::ResourceType::eTex2d, nullptr, 0 };
	sl::ResourceTag uiTag = sl::ResourceTag{ &ui, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &fullExtent };

	sl::ResourceTag inputs[] = { depthTag, mvecTag, hudLessTag, uiTag };
	slSetTag(viewport, inputs, _countof(inputs), nullptr);
}

// https://github.com/PureDark/Skyrim-Upscaler/blob/fa057bb088cf399e1112c1eaba714590c881e462/src/SkyrimUpscaler.cpp#L88
float GetVerticalFOVRad()
{
	static float& fac = (*(float*)(REL::RelocationID(513786, 388785).address()));
	const auto base = fac;
	const auto x = base / 1.30322540f;
	auto state = State::GetSingleton();
	const auto vFOV = 2 * atan(x / (state->screenSize.x / state->screenSize.y));
	return vFOV;
}

void Streamline::SetConstants()
{
	auto state = State::GetSingleton();

	auto cameraData = Util::GetCameraData(0);
	auto eyePosition = Util::GetEyePosition(0);

	auto clipToCameraView = cameraData.viewMat.Invert();
	auto cameraToWorld = cameraData.viewProjMatrixUnjittered.Invert();
	auto cameraToWorldPrev = cameraData.previousViewProjMatrixUnjittered.Invert();

	float4x4 cameraToPrevCamera;

	calcCameraToPrevCamera(*(sl::float4x4*)&cameraToPrevCamera, *(sl::float4x4*)&cameraToWorld, *(sl::float4x4*)&cameraToWorldPrev);

	float4x4 prevCameraToCamera = cameraToPrevCamera;

	prevCameraToCamera.Invert();

	sl::Constants slConstants = {};

	slConstants.cameraAspectRatio = state->screenSize.x / state->screenSize.y;
	slConstants.cameraFOV = GetVerticalFOVRad();
	slConstants.cameraFar = (*(float*)(REL::RelocationID(517032, 403540).address() + 0x44));
	slConstants.cameraMotionIncluded = sl::Boolean::eTrue;
	slConstants.cameraNear = (*(float*)(REL::RelocationID(517032, 403540).address() + 0x40));
	slConstants.cameraPinholeOffset = { 0.f, 0.f };
	slConstants.cameraPos = *(sl::float3*)&eyePosition;
	slConstants.cameraFwd = *(sl::float3*)&cameraData.viewForward;
	slConstants.cameraUp = *(sl::float3*)&cameraData.viewUp;
	slConstants.cameraRight = *(sl::float3*)&cameraData.viewRight;
	slConstants.cameraViewToClip = *(sl::float4x4*)&cameraData.viewMat;
	slConstants.clipToCameraView = *(sl::float4x4*)&clipToCameraView;
	slConstants.clipToPrevClip = *(sl::float4x4*)&cameraToPrevCamera;
	slConstants.depthInverted = sl::Boolean::eFalse;
	slConstants.jitterOffset = { 0, 0 };
	slConstants.mvecScale = { 1, 1 };
	slConstants.prevClipToClip = *(sl::float4x4*)&prevCameraToCamera;
	slConstants.reset = sl::Boolean::eFalse;
	slConstants.motionVectors3D = sl::Boolean::eFalse;
	slConstants.motionVectorsInvalidValue = FLT_MIN;
	slConstants.orthographicProjection = sl::Boolean::eFalse;
	slConstants.motionVectorsDilated = sl::Boolean::eFalse;
	slConstants.motionVectorsJittered = sl::Boolean::eFalse;

	if (SL_FAILED(res, slGetNewFrameToken(currentFrame, nullptr))) {
		logger::error("[Streamline] Could not get frame token");
	}

	if (SL_FAILED(res, slSetConstants(slConstants, *currentFrame, viewport))) {
		logger::error("[Streamline] Could not set constants");
	}
}