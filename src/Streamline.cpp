#include "Streamline.h"
#include <Streamline/include/sl_matrix_helpers.h>
#include <Util.h>

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

void Streamline::Initialize()
{
	sl::Preferences pref;
	sl::Feature myFeatures[] = { sl::kFeatureDLSS_G };
	pref.featuresToLoad = myFeatures;
	pref.numFeaturesToLoad = _countof(myFeatures);

	pref.logLevel = sl::LogLevel::eVerbose;
	pref.logMessageCallback = LoggingCallback;

	// Inform SL that it is OK to use newer version of SL or NGX (if available)
	pref.flags |= sl::PreferenceFlags::eAllowOTA;

	if (SL_FAILED(res, slInit(pref))) {
		logger::error("Failed to initialize Streamline");
	} else {
		logger::info("Sucessfully initialized Streamline");
	}
}

HRESULT Streamline::CreateSwapchain(IDXGIAdapter* pAdapter,
	D3D_DRIVER_TYPE DriverType,
	HMODULE Software,
	UINT Flags,
	const D3D_FEATURE_LEVEL* pFeatureLevels,
	UINT FeatureLevels,
	UINT SDKVersion,
	const DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
	IDXGISwapChain** ppSwapChain,
	ID3D11Device** ppDevice,
	D3D_FEATURE_LEVEL* pFeatureLevel,
	ID3D11DeviceContext** ppImmediateContext)
{
	auto mod = LoadLibraryW(L"sl.interposer.dll");

	auto slD3D11CreateDeviceAndSwapChain = reinterpret_cast<decltype(&D3D11CreateDeviceAndSwapChain)>(GetProcAddress(mod, "D3D11CreateDeviceAndSwapChain"));

	auto hr = slD3D11CreateDeviceAndSwapChain(
		pAdapter,
		DriverType,
		Software,
		Flags,
		pFeatureLevels,
		FeatureLevels,
		SDKVersion,
		pSwapChainDesc,
		ppSwapChain,
		ppDevice,
		pFeatureLevel,
		ppImmediateContext);

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

	texDesc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	HUDLessBuffer = new Texture2D(texDesc);
	HUDLessBuffer->CreateSRV(srvDesc);
	HUDLessBuffer->CreateRTV(rtvDesc);
	HUDLessBuffer->CreateUAV(uavDesc);

	{
		const char textureName[] = "HUDLessBuffer";
		HUDLessBuffer->resource->SetPrivateData(WKPDID_D3DDebugObjectName, sizeof(textureName) - 1, textureName);
	}

	{
		const char textureName[] = "HUDLessBufferSRV";
		HUDLessBuffer->srv.get()->SetPrivateData(WKPDID_D3DDebugObjectName, sizeof(textureName) - 1, textureName);
	}

	{
		const char textureName[] = "HUDLessBufferSRV";
		HUDLessBuffer->srv.get()->SetPrivateData(WKPDID_D3DDebugObjectName, sizeof(textureName) - 1, textureName);
	}
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
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

	data.texture->GetDesc(&texDesc);
	data.SRV->GetDesc(&srvDesc);
	data.RTV->GetDesc(&rtvDesc);
	data.UAV->GetDesc(&uavDesc);

	data.SRV->Release();
	data.RTV->Release();
	data.UAV->Release();
	data.texture->Release();

	texDesc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, &data.texture));
	DX::ThrowIfFailed(device->CreateShaderResourceView(data.texture, &srvDesc, &data.SRV));
	DX::ThrowIfFailed(device->CreateRenderTargetView(data.texture, &rtvDesc, &data.RTV));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(data.texture, &uavDesc, &data.UAV));
}

void Streamline::UpgradeGameResourceDepth(RE::RENDER_TARGET_DEPTHSTENCIL a_target)
{
	logger::info("Upgrading game resource {}", magic_enum::enum_name(a_target));

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	auto& data = renderer->GetDepthStencilData().depthStencils[a_target];

	auto& device = State::GetSingleton()->device;

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC depthSRVDesc = {};
	D3D11_SHADER_RESOURCE_VIEW_DESC stencilSRVDesc = {};

	D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
	D3D11_DEPTH_STENCIL_VIEW_DESC dsvReadOnlyDesc = {};

	data.texture->GetDesc(&texDesc);
	data.depthSRV->GetDesc(&depthSRVDesc);
	data.stencilSRV->GetDesc(&stencilSRVDesc);
	data.views[0]->GetDesc(&dsvDesc);
	data.readOnlyViews[0]->GetDesc(&dsvReadOnlyDesc);

	data.depthSRV->Release();
	data.stencilSRV->Release();
	data.views[0]->Release();
	data.readOnlyViews[0]->Release();

	data.texture->Release();

	texDesc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, &data.texture));
	DX::ThrowIfFailed(device->CreateShaderResourceView(data.texture, &depthSRVDesc, &data.depthSRV));
	DX::ThrowIfFailed(device->CreateShaderResourceView(data.texture, &stencilSRVDesc, &data.stencilSRV));
	DX::ThrowIfFailed(device->CreateDepthStencilView(data.texture, &dsvDesc, &data.views[0]));
	DX::ThrowIfFailed(device->CreateDepthStencilView(data.texture, &dsvReadOnlyDesc, &data.readOnlyViews[0]));
}

void Streamline::UpgradeGameResources()
{
	UpgradeGameResource(RE::RENDER_TARGETS::RENDER_TARGET::kMOTION_VECTOR);
	UpgradeGameResourceDepth(RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN);

	sl::DLSSGOptions options{};
	options.mode = sl::DLSSGMode::eOn;

	if (SL_FAILED(result, slDLSSGSetOptions(viewport, options))) {
		logger::error("Coulld not enable DLSSG");
	}
}

void Streamline::SetTags()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	auto& depthBuffer = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN];
	auto& motionVectorsBuffer = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::RENDER_TARGET::kMOTION_VECTOR];

	sl::Resource depth = { sl::ResourceType::eTex2d, depthBuffer.texture };
	sl::ResourceTag depthTag = sl::ResourceTag{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, NULL };

	sl::Resource mvec = { sl::ResourceType::eTex2d, motionVectorsBuffer.texture };
	sl::ResourceTag mvecTag = sl::ResourceTag{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, NULL };

	sl::Resource hudLess = { sl::ResourceType::eTex2d, HUDLessBuffer->resource.get() };
	sl::ResourceTag hudLessTag = sl::ResourceTag{ &hudLess, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, NULL };

	sl::Resource ui = { sl::ResourceType::eTex2d, nullptr };
	sl::ResourceTag uiTag = sl::ResourceTag{ &hudLess, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, NULL };

	auto& cmdList = State::GetSingleton()->context;

	sl::ResourceTag inputs[] = { depthTag, mvecTag, hudLessTag, uiTag };
	slSetTag(viewport, inputs, _countof(inputs), cmdList);
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

	consts.cameraFOV = atan(1.0f / cameraData.projMat.m[0][0]) * 2.0f * (180.0f / 3.14159265359f);

	consts.depthInverted = sl::Boolean::eFalse;
	consts.cameraMotionIncluded = sl::Boolean::eTrue;
	consts.motionVectors3D = sl::Boolean::eFalse;
	consts.reset = sl::Boolean::eFalse;
	consts.orthographicProjection = sl::Boolean::eFalse;
	consts.motionVectorsDilated = sl::Boolean::eFalse;
	consts.motionVectorsJittered = sl::Boolean::eFalse;

	float cameraAspectRatio = state->screenSize.x / state->screenSize.y;

	sl::FrameToken* frameToken{};
	if (SL_FAILED(res, slGetNewFrameToken(frameToken))) {
		logger::error("Coulld not get frame token");
	}

	if (SL_FAILED(res, slSetConstants(consts, *frameToken, viewport))) {
		logger::error("Coulld not set constants");
	}
}