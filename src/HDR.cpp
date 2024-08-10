#include "HDR.h"

#include "State.h"

void HDR::SetupResources()
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

	uiTexture = new Texture2D(texDesc);
	uiTexture->CreateSRV(srvDesc);
	uiTexture->CreateRTV(rtvDesc);
	uiTexture->CreateUAV(uavDesc);

	texDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
	srvDesc.Format = texDesc.Format;
	rtvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	hdrTexture = new Texture2D(texDesc);
	hdrTexture->CreateSRV(srvDesc);
	hdrTexture->CreateRTV(rtvDesc);
	hdrTexture->CreateUAV(uavDesc);
}

void SetDirtyStates(bool isCompute);

void HDR::ClearShaderCache()
{
	if (hdrOutputCS) {
		hdrOutputCS->Release();
		hdrOutputCS = nullptr;
	}
}

ID3D11ComputeShader* HDR::GetBlendingShader()
{
	if (!hdrOutputCS) {
		logger::debug("Compiling hdrOutputCS");
		hdrOutputCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\HDROutputCS.hlsl", {}, "cs_5_0"));
	}
	return hdrOutputCS;
}

void HDR::HDROutput()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& context = State::GetSingleton()->context;
	auto& swapChain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];

	if (swapChain.SRV) 
	{
		context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

		{
			ID3D11ShaderResourceView* srvs[2]{
				swapChain.SRV,
				uiTexture->srv.get()
			};

			context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

			ID3D11UnorderedAccessView* uavs[1]{ hdrTexture->uav.get() };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			context->CSSetShader(GetBlendingShader(), nullptr, 0);

			float2 resolution = State::GetSingleton()->screenSize;
			uint dispatchX = (uint)std::ceil(resolution.x / 8.0f);
			uint dispatchY = (uint)std::ceil(resolution.y / 8.0f);

			context->Dispatch(dispatchX, dispatchY, 1);
		}

		ID3D11Resource* swapChainResource;
		swapChain.SRV->GetResource(&swapChainResource);

		context->CopyResource(swapChainResource, hdrTexture->resource.get());
	}

	FLOAT clearColor[4] = { 0, 0, 0, 0 };
	context->ClearRenderTargetView(uiTexture->rtv.get(), clearColor);
}