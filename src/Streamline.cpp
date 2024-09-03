#include "Streamline.h"

#include <Util.h>
#include <dxgi.h>

void LoggingCallback(sl::LogType type, const char* msg)
{
	switch (type)
	{
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
	interposer = LoadLibraryW(L"Data/SKSE/Plugins/Streamline/sl.interposer.dll");

	sl::Preferences pref;
	sl::Feature myFeatures[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex };
	pref.featuresToLoad = myFeatures;
	pref.numFeaturesToLoad = _countof(myFeatures);

	pref.logLevel = sl::LogLevel::eVerbose;
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
		logger::error("Failed to initialize Streamline");
	} else {
		logger::info("Sucessfully initialized Streamline");
	}

	initialized = true;
}

void Streamline::Initialize_postDevice()
{
	// Hook up all of the feature functions using the sl function slGetFeatureFunction 
	slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", (void*&)slDLSSGGetState);
	slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", (void*&)slDLSSGSetOptions);
}

HRESULT Streamline::CreateDXGIFactory(REFIID riid, void** ppFactory)
{
	if (!initialized)
		Initialize_preDevice();

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

	auto slD3D11CreateDeviceAndSwapChain = reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(GetProcAddress(interposer, "D3D11CreateDeviceAndSwapChain"));
	
	const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;  // Create a device with only the latest feature level
	
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

void Streamline::SetupFrameGenerationResources()
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

	texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

	HUDLessBuffer = new Texture2D(texDesc);
	HUDLessBuffer->CreateSRV(srvDesc);
	HUDLessBuffer->CreateRTV(rtvDesc);
	HUDLessBuffer->CreateUAV(uavDesc);

	texDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.Format = texDesc.Format;
	rtvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	depthBuffer = new Texture2D(texDesc);
	depthBuffer->CreateSRV(srvDesc);
	depthBuffer->CreateRTV(rtvDesc);
	depthBuffer->CreateUAV(uavDesc);

	//copyDepthToSharedBuffer = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\Streamline\\CopyDepthToSharedBufferCS.hlsl", {}, "cs_5_0");
}

void Streamline::UpgradeGameResource(RE::RENDER_TARGET a_target)
{
	logger::info("Upgrading game resource {}", magic_enum::enum_name(a_target));

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

	texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

	DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, &data.texture));
	DX::ThrowIfFailed(device->CreateShaderResourceView(data.texture, &srvDesc, &data.SRV));
	DX::ThrowIfFailed(device->CreateRenderTargetView(data.texture, &rtvDesc, &data.RTV));
}

void Streamline::UpgradeGameResources()
{
	SetupFrameGenerationResources();
	UpgradeGameResource(RE::RENDER_TARGETS::RENDER_TARGET::kMOTION_VECTOR);

	sl::DLSSGOptions options{};
	options.mode = sl::DLSSGMode::eOn;

	if (SL_FAILED(result, slDLSSGSetOptions(viewport, options))) {
		logger::error("Could not enable DLSSG");
	}
}

void Streamline::SetTags()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	{
		// Copy HUD-less texture which is automatically used to mask the UI
		auto& swapChain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];

		auto state = State::GetSingleton();

		auto& context = state->context;

		ID3D11Resource* swapChainResource;
		swapChain.SRV->GetResource(&swapChainResource);

		state->BeginPerfEvent("HudLessColor Copy");
		context->CopyResource(GetSingleton()->HUDLessBuffer->resource.get(), swapChainResource);
		state->EndPerfEvent();		
	}

	auto& motionVectorsBuffer = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::RENDER_TARGET::kMOTION_VECTOR];

	sl::Resource depth = { sl::ResourceType::eTex2d, depthBuffer->resource.get() };
	sl::ResourceTag depthTag = sl::ResourceTag{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, NULL };
	
	sl::Resource mvec = { sl::ResourceType::eTex2d, motionVectorsBuffer.texture };
	sl::ResourceTag mvecTag = sl::ResourceTag{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, NULL };

	sl::Resource hudLess = { sl::ResourceType::eTex2d, HUDLessBuffer->resource.get() };
	sl::ResourceTag hudLessTag = sl::ResourceTag{ &hudLess, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, NULL };

	sl::Resource ui = { sl::ResourceType::eTex2d, nullptr };
	sl::ResourceTag uiTag = sl::ResourceTag{ &hudLess, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, NULL };

	sl::ResourceTag inputs[] = { depthTag, mvecTag, hudLessTag, uiTag };
	slSetTag(viewport, inputs, _countof(inputs), nullptr);
}

void Streamline::SetConstants()
{
	auto state = State::GetSingleton();

	sl::Constants consts = {};

	auto cameraData = Util::GetCameraData(0);

	consts.cameraViewToClip = *(sl::float4x4*)&cameraData.viewMat;

	auto clipToCameraView = cameraData.viewMat.Invert();
	consts.clipToCameraView = *(sl::float4x4*)&clipToCameraView;

	auto cameraToWorld = cameraData.viewProjMatrixUnjittered.Invert();
	auto cameraToWorldPrev = cameraData.previousViewProjMatrixUnjittered.Invert();

	sl::float4x4 cameraToPrevCamera;

	calcCameraToPrevCamera(cameraToPrevCamera, *(sl::float4x4*)&cameraToWorld, *(sl::float4x4*)&cameraToWorldPrev);
	
	sl::float4x4 prevCameraToCamera;

	calcCameraToPrevCamera(prevCameraToCamera, *(sl::float4x4*)&cameraToWorldPrev, *(sl::float4x4*)&cameraToWorld);

	consts.clipToPrevClip = cameraToPrevCamera;
	consts.prevClipToClip = prevCameraToCamera;

	consts.jitterOffset = { 0, 0 };

	consts.mvecScale = { 1, 1 };

	auto eyePosition = -Util::GetEyePosition(0);
	consts.cameraPos = *(sl::float3*)&eyePosition;

	consts.cameraUp = *(sl::float3*)&cameraData.viewUp;
	consts.cameraRight = *(sl::float3*)&cameraData.viewRight;
	consts.cameraFwd = *(sl::float3*)&cameraData.viewForward;

	consts.cameraNear = (*(float*)(REL::RelocationID(517032, 403540).address() + 0x40));
	consts.cameraFar = (*(float*)(REL::RelocationID(517032, 403540).address() + 0x44));

	consts.cameraFOV = atan(1.0f / cameraData.projMatrixUnjittered.m[0][0]) * 2.0f * (180.0f / 3.14159265359f);

	consts.depthInverted = sl::Boolean::eFalse;
	consts.cameraMotionIncluded = sl::Boolean::eTrue;
	consts.motionVectors3D = sl::Boolean::eFalse;
	consts.reset = sl::Boolean::eFalse;
	consts.orthographicProjection = sl::Boolean::eFalse;
	consts.motionVectorsDilated = sl::Boolean::eFalse;
	consts.motionVectorsJittered = sl::Boolean::eFalse;
	consts.cameraAspectRatio = state->screenSize.x / state->screenSize.y;

    sl::FrameToken* frameToken{};
	if(SL_FAILED(res, slGetNewFrameToken(frameToken, nullptr)))
    {
		logger::error("Coulld not get frame token");
    }

	if (SL_FAILED(res, slSetConstants(consts, *frameToken, viewport))) 
	{
		logger::error("Coulld not set constants");
	}
}