#include "VariableRateShading.h"
#include "State.h"
#include <Util.h>

HMODULE hNVAPI_DLL;

void VariableRateShading::ClearShaderCache()
{
	if (computeNASDataCS) {
		computeNASDataCS->Release();
		computeNASDataCS = nullptr;
	}
	if (computeShadingRateCS) {
		computeShadingRateCS->Release();
		computeShadingRateCS = nullptr;
	}
}

ID3D11ComputeShader* VariableRateShading::GetComputeNASData()
{
	if (!computeNASDataCS) {
		logger::debug("Compiling ComputeNASData");
		computeNASDataCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\VariableRateShading\\ComputeNASData.hlsl", {}, "cs_5_0");
	}
	return computeNASDataCS;
}

ID3D11ComputeShader* VariableRateShading::GetComputeShadingRate()
{
	if (!computeShadingRateCS) {
		logger::debug("Compiling ComputeShadingRate");
		computeShadingRateCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\VariableRateShading\\ComputeShadingRate.hlsl", {}, "cs_5_0");
	}
	return computeShadingRateCS;
}

void VariableRateShading::DrawSettings()
{
	ImGui::Checkbox("Enable Adaptive Rate Shading", &enableFFR);
	ImGui::Spacing();

	ImGui::Image(reductionData->srv.get(), { (float)reductionData->desc.Width * 4, (float)reductionData->desc.Height * 4 });
}

void VariableRateShading::ComputeNASData()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMAIN];

	ID3D11ShaderResourceView* srvs[1]{
		main.SRV
	};

	context->CSSetShaderResources(0, 1, srvs);

	ID3D11UnorderedAccessView* uavs[1]{ reductionData->uav.get() };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	auto shader = GetComputeNASData();
	context->CSSetShader(shader, nullptr, 0);

	context->Dispatch(reductionData->desc.Width, reductionData->desc.Height, 1);

	srvs[0] = nullptr;
	context->CSSetShaderResources(0, 1, srvs);

	uavs[0] = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);

	ComputeShadingRate();
}

void VariableRateShading::ComputeShadingRate()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	ID3D11ShaderResourceView* srvs[1]{
		reductionData->srv.get()
	};

	context->CSSetShaderResources(0, 1, srvs);

	ID3D11UnorderedAccessView* uavs[1]{ singleEyeVRSUAV[0] };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	auto shader = GetComputeShadingRate();
	context->CSSetShader(shader, nullptr, 0);

	float resolutionX = (float)reductionData->desc.Width;
	float resolutionY = (float)reductionData->desc.Height;

	uint32_t dispatchX = (uint32_t)std::ceil(resolutionX / 32.0f);
	uint32_t dispatchY = (uint32_t)std::ceil(resolutionY / 32.0f);

	context->Dispatch(dispatchX, dispatchY, 1);

	srvs[0] = nullptr;
	context->CSSetShaderResources(0, 1, srvs);

	uavs[0] = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);
}

std::vector<uint8_t> CreateSingleEyeFixedFoveatedVRSPattern(int width, int height)
{
	std::vector<uint8_t> data(width * height);

	enum class ShadingRate
	{
		k1x1,
		k2x2dir,
		k2x2,
		k4x4dir,
		k4x4,
	};

for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			data[y * width + x] = 0;

			float2 uv = { (float)x / (float)width, (float)y / (float)height };

			float xDist = abs(0.5f - uv.x);
			float yDist = abs(0.5f - uv.y);

			bool xDir = xDist < yDist;

			float quality = 1.0f - Vector2::Distance(uv, float2(0.5f, 0.5f));
			ShadingRate shadingRate = ShadingRate::k1x1;

			if (quality < 0.25)
				shadingRate = ShadingRate::k4x4dir;
			else if (quality < 0.5)
				shadingRate = ShadingRate::k2x2;
			else if (quality < 0.75)
				shadingRate = ShadingRate::k2x2dir;

			if (shadingRate == ShadingRate::k2x2dir) {
				if (!xDir)
					data[y * width + x] = 1;
				else
					data[y * width + x] = 2;
			} else if (shadingRate == ShadingRate::k2x2) {
				data[y * width + x] = 3;
			} else if (shadingRate == ShadingRate::k4x4dir) {
				if (!xDir)
					data[y * width + x] = 4;
				else
					data[y * width + x] = 5;
			}
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

	{
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

		main.texture->GetDesc(&texDesc);
		main.SRV->GetDesc(&srvDesc);
		main.UAV->GetDesc(&uavDesc);

		texDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		texDesc.Width /= 16;
		texDesc.Height /= 16;

		reductionData = new Texture2D(texDesc);
		reductionData->CreateSRV(srvDesc);
		reductionData->CreateUAV(uavDesc);
	}


	{
		for (uint i = 0; i < 114; i++)
		{
			auto& target = renderer->GetRuntimeData().renderTargets[i];
			if (target.texture)
			{
				D3D11_TEXTURE2D_DESC texDesc{};
				target.texture->GetDesc(&texDesc);

				if (texDesc.Width == width && texDesc.Height == height)
				{
					screenTargets.insert(i);
				}
			}

		}
	}
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

	bool enableVRS = screenTargets.contains(state->GetRuntimeData().renderTargets[0]);

	if (state->GetRuntimeData().depthStencil == RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN || state->GetRuntimeData().depthStencil == RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY)
	{
		enableVRS = true;
	}

	enableVRS = enableVRS && enableFFR;

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
		memset(vsrd[i].shadingRateTable, NV_PIXEL_X0_CULL_RASTER_PIXELS, sizeof(vsrd[0].shadingRateTable));
	}

	uint viewportCount = 8;

	if (enableVRS) {
		for (uint i = 0; i < viewportCount; i++) {
			vsrd[i].enableVariablePixelShadingRate = true;
			memset(vsrd[i].shadingRateTable, NV_PIXEL_X1_PER_RASTER_PIXEL, sizeof(vsrd[i].shadingRateTable));
			vsrd[i].shadingRateTable[0] = NV_PIXEL_X1_PER_RASTER_PIXEL;
			vsrd[i].shadingRateTable[1] = NV_PIXEL_X1_PER_2X1_RASTER_PIXELS;
			vsrd[i].shadingRateTable[2] = NV_PIXEL_X1_PER_1X2_RASTER_PIXELS;
			vsrd[i].shadingRateTable[3] = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;
			vsrd[i].shadingRateTable[4] = NV_PIXEL_X1_PER_4X2_RASTER_PIXELS;
			vsrd[i].shadingRateTable[5] = NV_PIXEL_X1_PER_2X4_RASTER_PIXELS;
			vsrd[i].shadingRateTable[6] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;

			//vsrd[i].shadingRateTable[0] = NV_PIXEL_X1_PER_RASTER_PIXEL;
			//vsrd[i].shadingRateTable[0x1] = NV_PIXEL_X1_PER_1X2_RASTER_PIXELS;
			//vsrd[i].shadingRateTable[0x4] = NV_PIXEL_X1_PER_2X1_RASTER_PIXELS;
			//vsrd[i].shadingRateTable[0x5] = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;
			//vsrd[i].shadingRateTable[0x6] = NV_PIXEL_X1_PER_2X4_RASTER_PIXELS;
			//vsrd[i].shadingRateTable[0x9] = NV_PIXEL_X1_PER_4X2_RASTER_PIXELS;
			//vsrd[i].shadingRateTable[0xa] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
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