#include "SSVGI.h"

#include "Util.h"
#include "Deferred.h"
#include "Skylighting.h"

void SSVGI::DrawSettings()
{
}


void SSVGI::Draw(const RE::BSShader* , const uint32_t)
{
	
}

void SSVGI::SetupResources()
{	
	{
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();

		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

		main.texture->GetDesc(&texDesc);
		main.SRV->GetDesc(&srvDesc);
		main.UAV->GetDesc(&uavDesc);

		texDesc.MipLevels = 0;
		texDesc.MiscFlags |= D3D11_RESOURCE_MISC_GENERATE_MIPS;

		srvDesc.Texture2D.MipLevels = 1 + (uint)std::floor(log2(std::max(texDesc.Width, texDesc.Height)));
		srvDesc.Texture2D.MostDetailedMip = 0;

		blurredRadiance = new Texture2D(texDesc);
		blurredRadiance->CreateSRV(srvDesc);
		blurredRadiance->CreateUAV(uavDesc);

		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;
	
		blurredDepth = new Texture2D(texDesc);
		blurredDepth->CreateSRV(srvDesc);
		blurredDepth->CreateUAV(uavDesc);
	}
}

void SSVGI::Load(json& o_json)
{
	Feature::Load(o_json);
}

void SSVGI::Save(json&)
{
}

void SSVGI::RestoreDefaultSettings()
{
}

ID3D11ComputeShader* SSVGI::GetCopyBuffersCS()
{
	if (!copyBuffersCS) {
		copyBuffersCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SSVGI\\CopyBuffer.hlsl", {}, "cs_5_0");
	}
	return copyBuffersCS;
}

ID3D11ComputeShader* SSVGI::GetIndirectLightingCS()
{
	if (!indirectLightingCS) {
		indirectLightingCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SSVGI\\IndirectLighting.hlsl", {}, "cs_5_0");
	}
	return indirectLightingCS;
}

void SSVGI::ComputeIndirectLighting(Texture2D* giTexture)
{
	auto& context = State::GetSingleton()->context;
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	// Downsample
	{
		auto& main = renderer->GetRuntimeData().renderTargets[Deferred::GetSingleton()->forwardRenderTargets[0]];
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

		ID3D11ShaderResourceView* srvs[2]{ main.SRV, depth.depthSRV };
		context->CSSetShaderResources(0, 2, srvs);

		ID3D11UnorderedAccessView* uavs[2]{ blurredRadiance->uav.get(), blurredDepth->uav.get() };
		context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

		context->CSSetShader(GetCopyBuffersCS(), nullptr, 0);

		auto state = State::GetSingleton();
		context->Dispatch(((uint)state->screenWidth + 7u) >> 3, ((uint)state->screenHeight + 7u) >> 3, 1);

		srvs[0] = nullptr;
		srvs[1] = nullptr;
		context->CSSetShaderResources(0, 2, srvs);

		uavs[0] = nullptr;
		uavs[1] = nullptr;
		context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

		context->CSSetShader(nullptr, nullptr, 0);

		context->GenerateMips(blurredRadiance->srv.get());
		context->GenerateMips(blurredDepth->srv.get());
	}

	// Indirect Lighting
	{
		auto& normalGlossiness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

		ID3D11ShaderResourceView* srvs[5]{ blurredRadiance->srv.get(), blurredDepth->srv.get(), normalGlossiness.SRV, depth.depthSRV, Skylighting::GetSingleton()->shadowView };
		context->CSSetShaderResources(0, 5, srvs);
		
		ID3D11UnorderedAccessView* uavs[1]{ giTexture->uav.get() };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

		context->CSSetShader(GetIndirectLightingCS(), nullptr, 0);

		ID3D11SamplerState* samplers[1] = { Deferred::GetSingleton()->linearSampler };
		context->CSSetSamplers(0, 1, samplers);

		auto state = State::GetSingleton();
		context->Dispatch(((uint)state->screenWidth + 7u) >> 3, ((uint)state->screenHeight + 7u) >> 3, 1);

		srvs[0] = nullptr;
		srvs[1] = nullptr;
		srvs[2] = nullptr;
		srvs[3] = nullptr;
		srvs[4] = nullptr;

		context->CSSetShaderResources(0, 5, srvs);

		uavs[0] = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

		context->CSSetShader(nullptr, nullptr, 0);
	
		samplers[0] = nullptr;		
		context->CSSetSamplers(0, 1, samplers);
	}
}

void SSVGI::ClearShaderCache()
{
	if (copyBuffersCS) {
		copyBuffersCS->Release();
		copyBuffersCS = nullptr;
	}
	if (indirectLightingCS) {
		indirectLightingCS->Release();
		indirectLightingCS = nullptr;
	}
}
