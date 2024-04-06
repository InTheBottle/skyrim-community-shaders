#include "VariableRateShading.h"
#include "State.h"

HMODULE hNVAPI_DLL;

std::vector<uint8_t> CreateSingleEyeFixedFoveatedVRSPattern(int width, int height)
{
	std::vector<uint8_t> data(width * height);

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			data[y * width + x] = 3;
		}
	}

	return data;
}

void VariableRateShading::Setup()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto device = renderer->GetRuntimeData().forwarder;

	logger::info("Trying to load NVAPI...");

	hNVAPI_DLL = LoadLibraryA("nvapi64.dll");

	if (!hNVAPI_DLL)
		return;

	NvAPI_Status result = NvAPI_Initialize();
	if (result != NVAPI_OK) {
		return;
	}

	NV_D3D1x_GRAPHICS_CAPS caps;
	memset(&caps, 0, sizeof(NV_D3D1x_GRAPHICS_CAPS));
	NvAPI_Status status = NvAPI_D3D1x_GetGraphicsCapabilities(device, NV_D3D1x_GRAPHICS_CAPS_VER, &caps);
	if (status != NVAPI_OK || !caps.bVariablePixelRateShadingSupported) {
		logger::info("Variable rate shading is not available.");
		return;
	}

	vrsActive = true;
	logger::info("Successfully initialized NVAPI; Variable Rate Shading is available.");

	auto width = State::GetSingleton()->screenWidth;
	auto height = State::GetSingleton()->screenHeight;
	SetupSingleEyeVRS(0, (int)width, (int)height);
}

void VariableRateShading::SetupSingleEyeVRS(int eye, int width, int height)
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto device = renderer->GetRuntimeData().forwarder;

	int vrsWidth = width / NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH;
	int vrsHeight = height / NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT;

	logger::info("Creating VRS pattern texture for eye");

	D3D11_TEXTURE2D_DESC texDesc = {};
	texDesc.Width = vrsWidth;
	texDesc.Height = vrsHeight;
	texDesc.ArraySize = 1;
	texDesc.Format = DXGI_FORMAT_R8_UINT;
	texDesc.SampleDesc.Count = 1;
	texDesc.SampleDesc.Quality = 0;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	texDesc.CPUAccessFlags = 0;
	texDesc.MiscFlags = 0;
	texDesc.MipLevels = 1;
	auto data = CreateSingleEyeFixedFoveatedVRSPattern(vrsWidth, vrsHeight);
	D3D11_SUBRESOURCE_DATA subresourceData;
	subresourceData.pSysMem = data.data();
	subresourceData.SysMemPitch = vrsWidth;
	subresourceData.SysMemSlicePitch = 0;
	DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, &subresourceData, &singleEyeVRSTex[eye]));

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	uavDesc.Format = texDesc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	uavDesc.Texture2D.MipSlice = 0;
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(singleEyeVRSTex[eye], &uavDesc, &singleEyeVRSUAV[eye]));

	logger::info("Creating shading rate resource view for eye");
	NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC vd = {};
	vd.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
	vd.Format = texDesc.Format;
	vd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
	vd.Texture2D.MipSlice = 0;
	NvAPI_Status status = NvAPI_D3D11_CreateShadingRateResourceView(device, singleEyeVRSTex[eye], &vd, &singleEyeVRSView[eye]);
	if (status != NVAPI_OK) {
		logger::info("Failed to create VRS pattern view for eye");
		return;
	}
}

void VariableRateShading::UpdateViews()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();

	bool enableVRS = state->GetRuntimeData().renderTargets[0] == RE::RENDER_TARGET::kMAIN;

	if (state->GetRuntimeData().depthStencil == RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN || state->GetRuntimeData().depthStencil == RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY)
	{
		enableVRS = true;
	}

	if (enableVRS) {
		ID3D11NvShadingRateResourceView* shadingRateView = singleEyeVRSView[0];

		NvAPI_Status statusSRRV = NvAPI_D3D11_RSSetShadingRateResourceView(context, shadingRateView);
		if (statusSRRV != NVAPI_OK)
			logger::info("Setting the shading rate resource view failed");
	}

	NV_D3D11_VIEWPORT_SHADING_RATE_DESC vsrd[8];
	for (uint i = 0; i < 8; i++)
	{
		vsrd[i].enableVariablePixelShadingRate = false;
		memset(vsrd[i].shadingRateTable, 0, sizeof(vsrd[0].shadingRateTable));
	}

	uint viewportCount = 8;

	if (enableVRS) {
		for (uint i = 0; i < viewportCount; i++) {
			vsrd[i].enableVariablePixelShadingRate = true;
			memset(vsrd[i].shadingRateTable, 5, sizeof(vsrd[i].shadingRateTable));
			vsrd[i].shadingRateTable[0] = NV_PIXEL_X1_PER_RASTER_PIXEL;
			vsrd[i].shadingRateTable[1] = NV_PIXEL_X1_PER_2X1_RASTER_PIXELS;
			vsrd[i].shadingRateTable[2] = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;
			vsrd[i].shadingRateTable[3] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
		}
	}

	NV_D3D11_VIEWPORTS_SHADING_RATE_DESC srd;
	srd.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
	srd.numViewports = viewportCount;
	srd.pViewports = vsrd;

	{
		NvAPI_Status statusVPSR = NvAPI_D3D11_RSSetViewportsPixelShadingRates(context, &srd);
		if (statusVPSR != NVAPI_OK)
			logger::info("Setting the viewport pixel shading rate failed");
	}
}