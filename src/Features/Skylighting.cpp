#include "Skylighting.h"
#include <Util.h>
#include <Deferred.h>

void Skylighting::DrawSettings()
{
}

void Skylighting::Draw(const RE::BSShader*, const uint32_t)
{
	
}

void Skylighting::SetupResources()
{
	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DEFAULT;
		sbDesc.CPUAccessFlags = 0;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.FirstElement = 0;
		uavDesc.Buffer.Flags = 0;

		std::uint32_t numElements = 1;

		sbDesc.StructureByteStride = sizeof(PerGeometry);
		sbDesc.ByteWidth = sizeof(PerGeometry) * numElements;
		perShadow = new Buffer(sbDesc);
		srvDesc.Buffer.NumElements = numElements;
		perShadow->CreateSRV(srvDesc);
		uavDesc.Buffer.NumElements = numElements;
		perShadow->CreateUAV(uavDesc);

		copyShadowCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ShadowTest\\CopyShadowData.hlsl", {}, "cs_5_0");
	}

	GetSkylightingCS();

	{
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();

		
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

		main.texture->GetDesc(&texDesc);
		main.SRV->GetDesc(&srvDesc);
		main.UAV->GetDesc(&uavDesc);
		
		texDesc.Format = DXGI_FORMAT_R16_UNORM;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		skylightingTexture = new Texture2D(texDesc);
		skylightingTexture->CreateSRV(srvDesc);
		skylightingTexture->CreateUAV(uavDesc);

	}

	{
		perFrameCB = new ConstantBuffer(ConstantBufferDesc<PerFrameCB>());
	}
}

void Skylighting::Reset()
{
}

void Skylighting::Load(json& o_json)
{
	Feature::Load(o_json);
}

void Skylighting::Save(json&)
{
}

void Skylighting::RestoreDefaultSettings()
{
}

ID3D11ComputeShader* Skylighting::GetSkylightingCS()
{
	if (!skylightingCS) {
		logger::debug("Compiling SkylightingCS");
		skylightingCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\Skylighting\\SkylightingCS.hlsl", {}, "cs_5_0");
	}
	return skylightingCS;
}

void Skylighting::ClearShaderCache()
{
	if (skylightingCS) {
		skylightingCS->Release();
		skylightingCS = nullptr;
	}
}

void Skylighting::CopyShadowData()
{
	if (!loaded)
		return;

	auto& context = State::GetSingleton()->context;

	ID3D11UnorderedAccessView* uavs[1]{ perShadow->uav.get() };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	ID3D11Buffer* buffers[1];
	context->PSGetConstantBuffers(2, 1, buffers);
	context->CSSetConstantBuffers(0, 1, buffers);

	context->PSGetConstantBuffers(12, 1, buffers);
	context->CSSetConstantBuffers(1, 1, buffers);

	context->PSGetConstantBuffers(0, 1, buffers);
	context->CSSetConstantBuffers(2, 1, buffers);

	context->PSGetShaderResources(4, 1, &shadowView);

	context->CSSetSamplers(0, 1, &Deferred::GetSingleton()->linearSampler);

	context->CSSetShader(copyShadowCS, nullptr, 0);

	context->Dispatch(1, 1, 1);

	uavs[0] = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	buffers[0] = nullptr;
	context->CSSetConstantBuffers(0, 1, buffers);
	context->CSSetConstantBuffers(1, 1, buffers);
	context->CSSetConstantBuffers(2, 1, buffers);

	context->CSSetShader(nullptr, nullptr, 0);
}

void Skylighting::Compute()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto state = State::GetSingleton();
	auto& context = state->context;
	auto viewport = RE::BSGraphics::State::GetSingleton();

	float resolutionX = state->screenWidth * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
	float resolutionY = state->screenHeight * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

	{
		PerFrameCB data{};

		data.BufferDim.x = state->screenWidth;
		data.BufferDim.y = state->screenHeight;
		data.BufferDim.z = 1.0f / data.BufferDim.x;
		data.BufferDim.w = 1.0f / data.BufferDim.y;

		data.DynamicRes.x = viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
		data.DynamicRes.y = viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;
		data.DynamicRes.z = 1.0f / data.DynamicRes.x;
		data.DynamicRes.w = 1.0f / data.DynamicRes.y;

		auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();

		auto useTAA = !REL::Module::IsVR() ? imageSpaceManager->GetRuntimeData().BSImagespaceShaderISTemporalAA->taaEnabled : imageSpaceManager->GetVRRuntimeData().BSImagespaceShaderISTemporalAA->taaEnabled;
		data.FrameCount = useTAA || state->upscalerLoaded ? viewport->uiFrameCount : 0;

		perFrameCB->Update(data);
	}

	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

	ID3D11ShaderResourceView* srvs[3]{
		depth.depthSRV,
		shadowView,
		perShadow->srv.get(),
	};

	context->CSSetShaderResources(0, 3, srvs);

	ID3D11UnorderedAccessView* uavs[1]{ skylightingTexture->uav.get() };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	auto buffer = perFrameCB->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	auto sampler = Deferred::GetSingleton()->linearSampler;
	context->CSSetSamplers(0, 1, &sampler);

	context->CSSetShader(GetSkylightingCS(), nullptr, 0);

	uint32_t dispatchX = (uint32_t)std::ceil(resolutionX / 8.0f);
	uint32_t dispatchY = (uint32_t)std::ceil(resolutionY / 8.0f);

	context->Dispatch(dispatchX, dispatchY, 1);

	srvs[0] = nullptr;
	srvs[1] = nullptr;
	srvs[2] = nullptr;

	context->CSSetShaderResources(0, 3, srvs);

	uavs[0] = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &buffer);

	sampler = nullptr;
	context->CSSetSamplers(0, 1, &sampler);

	context->CSSetShader(nullptr, nullptr, 0);
}

void Skylighting::Bind()
{	
	if (!loaded)
		return;

	Compute();

	auto& context = State::GetSingleton()->context;

	ID3D11ShaderResourceView* srvs[3]{
		shadowView,
		perShadow->srv.get(),
		skylightingTexture->srv.get()
	};

	context->PSSetShaderResources(80, 3, srvs);
}
