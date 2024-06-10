#include "Skylighting.h"
#include <DDSTextureLoader.h>
#include <Deferred.h>
#include <Util.h>

void Skylighting::DrawSettings()
{
	ImGui::Checkbox("Do occlusion", &GetSingleton()->doOcclusion);
	ImGui::Checkbox("Render trees", &renderTrees);

	ImGui::SliderFloat("Bound Size", &boundSize, 0, 512, "%.2f");
	ImGui::SliderFloat("Distance", &occlusionDistance, 0, 20000);
}

void Skylighting::Draw(const RE::BSShader*, const uint32_t)
{
}

void Skylighting::SetupResources()
{
	GetSkylightingCS();

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	{
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

		main.texture->GetDesc(&texDesc);
		main.SRV->GetDesc(&srvDesc);
		main.UAV->GetDesc(&uavDesc);

		texDesc.Format = DXGI_FORMAT_R8G8_UNORM;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		skylightingTexture = new Texture2D(texDesc);
		skylightingTexture->CreateSRV(srvDesc);
		skylightingTexture->CreateUAV(uavDesc);
	}

	{
		auto& precipitationOcclusion = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};

		precipitationOcclusion.texture->GetDesc(&texDesc);
		precipitationOcclusion.depthSRV->GetDesc(&srvDesc);
		precipitationOcclusion.views[0]->GetDesc(&dsvDesc);

		occlusionTexture = new Texture2D(texDesc);
		occlusionTexture->CreateSRV(srvDesc);
		occlusionTexture->CreateDSV(dsvDesc);

		occlusionTranslucentTexture = new Texture2D(texDesc);
		occlusionTranslucentTexture->CreateSRV(srvDesc);
		occlusionTranslucentTexture->CreateDSV(dsvDesc);
	}

	{
		perFrameCB = new ConstantBuffer(ConstantBufferDesc<PerFrameCB>());
	}

	{
		auto& device = State::GetSingleton()->device;
		auto& context = State::GetSingleton()->context;

		DirectX::CreateDDSTextureFromFile(device, context, L"Data\\Shaders\\Skylighting\\bluenoise.dds", nullptr, &noiseView);
	}

	{
		auto& device = State::GetSingleton()->device;

		D3D11_SAMPLER_DESC sampDesc;

		sampDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.MipLODBias = 0.0f;
		sampDesc.MaxAnisotropy = 1;
		sampDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
		sampDesc.BorderColor[0] = sampDesc.BorderColor[1] = sampDesc.BorderColor[2] = sampDesc.BorderColor[3] = 0;
		sampDesc.MinLOD = 0;
		sampDesc.MaxLOD = D3D11_FLOAT32_MAX;

		DX::ThrowIfFailed(device->CreateSamplerState(&sampDesc, &comparisonSampler));
	}
}

void Skylighting::Reset()
{
	translucent = false;
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
		data.OcclusionViewProj = viewProjMat;

		auto shadowSceneNode = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
		auto shadowDirLight = (RE::BSShadowDirectionalLight*)shadowSceneNode->GetRuntimeData().shadowDirLight;
		bool dirShadow = shadowDirLight && shadowDirLight->shadowLightIndex == 0;

		if (dirShadow) {
			data.ShadowDirection = float4(shadowDirLight->lightDirection.x, shadowDirLight->lightDirection.y, shadowDirLight->lightDirection.z, 0);
		}

		perFrameCB->Update(data);
	}

	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	auto normalRoughness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];

	ID3D11ShaderResourceView* srvs[7]{
		depth.depthSRV,
		Deferred::GetSingleton()->shadowView,
		Deferred::GetSingleton()->perShadow->srv.get(),
		noiseView,
		occlusionTexture->srv.get(),
		occlusionTranslucentTexture->srv.get(),
		normalRoughness.SRV
	};

	context->CSSetShaderResources(0, 7, srvs);

	ID3D11UnorderedAccessView* uavs[1]{ skylightingTexture->uav.get() };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	auto buffer = perFrameCB->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

	ID3D11SamplerState* samplers[2] = { Deferred::GetSingleton()->linearSampler, comparisonSampler };
	context->CSSetSamplers(0, 2, samplers);

	context->CSSetShader(GetSkylightingCS(), nullptr, 0);

	uint32_t dispatchX = (uint32_t)std::ceil(resolutionX / 8.0f);
	uint32_t dispatchY = (uint32_t)std::ceil(resolutionY / 8.0f);

	context->Dispatch(dispatchX, dispatchY, 1);

	srvs[0] = nullptr;
	srvs[1] = nullptr;
	srvs[2] = nullptr;
	srvs[3] = nullptr;
	srvs[4] = nullptr;
	srvs[5] = nullptr;
	srvs[6] = nullptr;

	context->CSSetShaderResources(0, 7, srvs);

	uavs[0] = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &buffer);

	samplers[0] = nullptr;
	samplers[1] = nullptr;
	context->CSSetSamplers(0, 2, samplers);

	context->CSSetShader(nullptr, nullptr, 0);
}

void Skylighting::EnableTranslucentDepth()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& precipitation = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];

	precipitation.depthSRV = occlusionTranslucentTexture->srv.get();
	precipitation.texture = occlusionTranslucentTexture->resource.get();
	precipitation.views[0] = occlusionTranslucentTexture->dsv.get();

	auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
	GET_INSTANCE_MEMBER(stateUpdateFlags, shadowState)
	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void Skylighting::DisableTranslucentDepth()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& precipitation = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];

	precipitation.depthSRV = occlusionTexture->srv.get();
	precipitation.texture = occlusionTexture->resource.get();
	precipitation.views[0] = occlusionTexture->dsv.get();

	auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
	GET_INSTANCE_MEMBER(stateUpdateFlags, shadowState)
	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void Skylighting::UpdateDepthStencilView(RE::BSRenderPass* a_pass)
{
	if (inOcclusion) {
		auto currentTranslucent = a_pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim);
		if (translucent != currentTranslucent) {
			translucent = currentTranslucent;
			if (translucent) {
				EnableTranslucentDepth();
			} else {
				DisableTranslucentDepth();
			}
		}
	}
}

void Skylighting::Bind()
{
	if (!loaded)
		return;

	auto& context = State::GetSingleton()->context;

	ID3D11ShaderResourceView* srvs[8];
	context->PSGetShaderResources(0, 8, srvs);

	ID3D11ShaderResourceView* srvsCS[8];
	context->CSGetShaderResources(0, 8, srvsCS);

	ID3D11UnorderedAccessView* uavsCS[8];
	context->CSGetUnorderedAccessViews(0, 8, uavsCS);

	ID3D11UnorderedAccessView* nullUavs[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 8, nullUavs, nullptr);

	ID3D11ShaderResourceView* nullSrvs[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	context->PSSetShaderResources(0, 8, nullSrvs);
	context->CSSetShaderResources(0, 8, nullSrvs);

	ID3D11RenderTargetView* views[8];
	ID3D11DepthStencilView* dsv;
	context->OMGetRenderTargets(8, views, &dsv);

	ID3D11RenderTargetView* nullViews[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	ID3D11DepthStencilView* nullDsv = nullptr;
	context->OMSetRenderTargets(8, nullViews, nullDsv);

	Compute();

	context->PSSetShaderResources(0, 8, srvs);
	context->CSSetShaderResources(0, 8, srvsCS);
	context->CSSetUnorderedAccessViews(0, 8, uavsCS, nullptr);
	context->OMSetRenderTargets(8, views, dsv);

	for (int i = 0; i < 8; i++) {
		if (srvs[i])
			srvs[i]->Release();
		if (srvsCS[i])
			srvsCS[i]->Release();
	}

	for (int i = 0; i < 8; i++) {
		if (views[i])
			views[i]->Release();
	}

	if (dsv)
		dsv->Release();
}