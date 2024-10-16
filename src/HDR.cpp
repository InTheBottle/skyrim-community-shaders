#include "HDR.h"

#include "State.h"

bool HDR::QueryHDRSupport()
{
	return true;
}

// https://github.com/EndlesslyFlowering/Starfield-Luma/blob/master/Plugin/src/Utils.cpp#L240
// Only works if HDR is enaged on the monitor that contains the swapchain
bool GetHDRMaxLuminance(IDXGISwapChain3* a_swapChainInterface, float& a_outMaxLuminance)
{
	IDXGIOutput* output = nullptr;
	if (FAILED(a_swapChainInterface->GetContainingOutput(&output))) {
		return false;
	}

	IDXGIOutput6* output6 = nullptr;
	if (FAILED(output->QueryInterface(&output6))) {
		return false;
	}

	DXGI_OUTPUT_DESC1 desc1;
	if (FAILED(output6->GetDesc1(&desc1))) {
		return false;
	}

	// Note: this might end up being outdated if a new display is added/removed,
	// or if HDR is toggled on them after swapchain creation (though it seems to be consistent between SDR and HDR).
	a_outMaxLuminance = desc1.MaxLuminance;

	// HDR is not supported (this only works if HDR is enaged on the monitor that currently contains the swapchain)
	if (desc1.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 && desc1.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) {
		return false;
	}

	return true;
}

void HDR::QueryDisplayPeakBrightness(IDXGISwapChain3* a_swapChainInterface)
{
	if (GetHDRMaxLuminance(a_swapChainInterface, reportedDisplayPeakBrightness)) {
		displayPeakBrightness = (int)reportedDisplayPeakBrightness;
	}
}

void HDR::DrawSettings()
{
	if (ImGui::Button("Reset HDR Settings", { -1, 0 })) {
		reportedDisplayPeakBrightness = 400;
		displayPeakBrightness = 400;
		gameBrightness = 200;
		uiBrightness = 200;

		IDXGISwapChain3* swapChain3 = nullptr;
		DX::ThrowIfFailed(State::GetSingleton()->swapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&swapChain3));
		swapChain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
		QueryDisplayPeakBrightness(swapChain3);
	}

	ImGui::SliderInt("Display Peak Brightness (nits)", &displayPeakBrightness, 400, 10000);
	ImGui::SliderInt("Game Brightness (nits)", &gameBrightness, 100, 400);
	ImGui::SliderInt("UI Brightness (nits)", &uiBrightness, 100, 400);

	gameBrightness = std::min(gameBrightness, displayPeakBrightness);
	uiBrightness = std::min(uiBrightness, displayPeakBrightness);
}

float4 HDR::GetHDRData()
{
	float4 data;
	data.x = (float)enabled;
	data.y = (float)displayPeakBrightness;
	data.z = (float)gameBrightness;
	data.w = (float)uiBrightness;
	return data;
}

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

	texDesc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
	srvDesc.Format = texDesc.Format;
	rtvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	hdrTexture = new Texture2D(texDesc);
	hdrTexture->CreateSRV(srvDesc);
	hdrTexture->CreateRTV(rtvDesc);
	hdrTexture->CreateUAV(uavDesc);

	texDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
	srvDesc.Format = texDesc.Format;
	rtvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	outputTexture = new Texture2D(texDesc);
	outputTexture->CreateSRV(srvDesc);
	outputTexture->CreateRTV(rtvDesc);
	outputTexture->CreateUAV(uavDesc);

	hdrDataCB = new ConstantBuffer(ConstantBufferDesc<HDRDataCB>());
}

void HDR::CheckSwapchain()
{
	if (!swapChainResource) {
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto& swapChain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
		if (swapChain.SRV) {
			swapChain.SRV->GetResource(&swapChainResource);
		}
	}
}

void HDR::ClearShaderCache()
{
	if (hdrOutputCS) {
		hdrOutputCS->Release();
		hdrOutputCS = nullptr;
	}
}

ID3D11ComputeShader* HDR::GetHDROutputCS()
{
	if (!hdrOutputCS) {
		logger::debug("Compiling HDROutputCS.hlsl");
		hdrOutputCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\HDROutputCS.hlsl", {}, "cs_5_0"));
	}
	return hdrOutputCS;
}

void HDR::HDROutput()
{
	static auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	static auto& context = State::GetSingleton()->context;
	static auto& swapChain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];

	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	{
		HDRDataCB data = { GetHDRData() };
		hdrDataCB->Update(data);
	}

	{
		ID3D11ShaderResourceView* srvs[2]{ hdrTexture->srv.get(), uiTexture->srv.get() };
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11UnorderedAccessView* uavs[1]{ outputTexture->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		ID3D11Buffer* cbs[1]{ hdrDataCB->CB() };
		context->CSSetConstantBuffers(0, ARRAYSIZE(cbs), cbs);

		context->CSSetShader(GetHDROutputCS(), nullptr, 0);

		auto dispatchCount = Util::GetScreenDispatchCount(false);
		context->Dispatch(dispatchCount.x, dispatchCount.y, 1);

		srvs[0] = nullptr;
		srvs[1] = nullptr;
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		uavs[0] = nullptr;
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		cbs[0] = nullptr;
		context->CSSetConstantBuffers(0, ARRAYSIZE(cbs), cbs);
	}

	// Copy fake swapchain into real one
	context->CopyResource(swapChainResource, outputTexture->resource.get());

	// Reset UI buffer
	float clearColor[4] = { 0, 0, 0, 0 };
	context->ClearRenderTargetView(uiTexture->rtv.get(), clearColor);

	// Set render target for compatibility
	context->OMSetRenderTargets(1, &swapChain.RTV, nullptr);
}
