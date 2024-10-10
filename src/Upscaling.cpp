#include "Upscaling.h"

#include "Hooks.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Upscaling::Settings,
	upscaleMethod,
	upscaleMethodNoDLSS,
	sharpness,
	dlssPreset);

void Upscaling::DrawSettings()
{
	// Skyrim settings control whether any upscaling is possible
	auto state = State::GetSingleton();
	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	auto streamline = Streamline::GetSingleton();
	GET_INSTANCE_MEMBER(BSImagespaceShaderISTemporalAA, imageSpaceManager);
	auto& bTAA = BSImagespaceShaderISTemporalAA->taaEnabled;  // Setting used by shaders

	// Update upscale mode based on TAA setting
	settings.upscaleMethod = bTAA ? (settings.upscaleMethod == (uint)UpscaleMethod::kNONE ? (uint)UpscaleMethod::kTAA : settings.upscaleMethod) : (uint)UpscaleMethod::kNONE;

	// Display upscaling options in the UI
	const char* upscaleModes[] = { "Disabled", "Temporal Anti-Aliasing", "AMD FSR 3.1", "NVIDIA DLAA" };

	// Determine available modes
	bool featureDLSS = streamline->featureDLSS;
	uint* currentUpscaleMode = featureDLSS ? &settings.upscaleMethod : &settings.upscaleMethodNoDLSS;
	uint availableModes = (state->isVR && state->upscalerLoaded) ? (featureDLSS ? 2 : 1) : (featureDLSS ? 3 : 2);

	// Slider for method selection
	ImGui::SliderInt("Method", (int*)currentUpscaleMode, 0, availableModes, std::format("{}", upscaleModes[(uint)*currentUpscaleMode]).c_str());

	*currentUpscaleMode = std::min(availableModes, (uint)*currentUpscaleMode);
	bTAA = *currentUpscaleMode != (uint)UpscaleMethod::kNONE;

	// settings for scaleform/ini
	if (auto iniSettingCollection = RE::INIPrefSettingCollection::GetSingleton()) {
		if (auto setting = iniSettingCollection->GetSetting("bUseTAA:Display")) {
			setting->data.b = bTAA;
		}
	}

	// Check the current upscale method
	auto upscaleMethod = GetUpscaleMethod();

	// Display sharpness slider if applicable
	if (upscaleMethod != UpscaleMethod::kTAA && upscaleMethod != UpscaleMethod::kNONE) {
		ImGui::SliderFloat("Sharpness", &settings.sharpness, 0.0f, 1.0f, "%.1f");
		settings.sharpness = std::clamp(settings.sharpness, 0.0f, 1.0f);
	}

	// Display DLSS preset slider if using DLSS
	if (upscaleMethod == UpscaleMethod::kDLSS) {
		const char* dlssPresets[] = { "Default", "Preset A", "Preset B", "Preset C", "Preset D", "Preset E", "Preset F" };
		ImGui::SliderInt("DLSS Preset", (int*)&settings.dlssPreset, 0, 6, std::format("{}", dlssPresets[(uint)settings.dlssPreset]).c_str());
		settings.dlssPreset = std::min(6u, (uint)settings.dlssPreset);
	}
}

void Upscaling::SaveSettings(json& o_json)
{
	std::lock_guard<std::mutex> lock(settingsMutex);

	o_json = settings;
	auto iniSettingCollection = RE::INIPrefSettingCollection::GetSingleton();
	if (iniSettingCollection) {
		auto setting = iniSettingCollection->GetSetting("bUseTAA:Display");
		if (setting) {
			iniSettingCollection->WriteSetting(setting);
		}
	}
}

void Upscaling::LoadSettings(json& o_json)
{
	std::lock_guard<std::mutex> lock(settingsMutex);
	settings = o_json;
	auto iniSettingCollection = RE::INIPrefSettingCollection::GetSingleton();
	if (iniSettingCollection) {
		auto setting = iniSettingCollection->GetSetting("bUseTAA:Display");
		if (setting) {
			iniSettingCollection->ReadSetting(setting);
		}
	}
}

Upscaling::UpscaleMethod Upscaling::GetUpscaleMethod()
{
	auto streamline = Streamline::GetSingleton();
	return streamline->featureDLSS ? (Upscaling::UpscaleMethod)settings.upscaleMethod : (Upscaling::UpscaleMethod)settings.upscaleMethodNoDLSS;
}

void Upscaling::CheckResources()
{
	static auto previousUpscaleMode = UpscaleMethod::kTAA;
	auto currentUpscaleMode = GetUpscaleMethod();

	auto streamline = Streamline::GetSingleton();
	auto fidelityFX = FidelityFX::GetSingleton();

	if (previousUpscaleMode != currentUpscaleMode) {
		if (previousUpscaleMode == UpscaleMethod::kTAA)
			CreateUpscalingResources();
		else if (previousUpscaleMode == UpscaleMethod::kDLSS)
			streamline->DestroyDLSSResources();
		else if (previousUpscaleMode == UpscaleMethod::kFSR)
			fidelityFX->DestroyFSRResources();

		if (currentUpscaleMode == UpscaleMethod::kTAA)
			DestroyUpscalingResources();
		else if (currentUpscaleMode == UpscaleMethod::kFSR)
			fidelityFX->DestroyFSRResources();
		else if (currentUpscaleMode == UpscaleMethod::kFSR)
			fidelityFX->CreateFSRResources();

		previousUpscaleMode = currentUpscaleMode;
	}
}

ID3D11ComputeShader* Upscaling::GetRCASComputeShader()
{
	static auto previousSharpness = settings.sharpness;
	auto currentSharpness = settings.sharpness;

	if (previousSharpness != currentSharpness) {
		previousSharpness = currentSharpness;

		if (rcasCS) {
			rcasCS->Release();
			rcasCS = nullptr;
		}
	}

	if (!rcasCS) {
		logger::debug("Compiling Utility.hlsl");
		rcasCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\RCAS\\RCAS.hlsl", { { "SHARPNESS", std::format("{}", currentSharpness).c_str() } }, "cs_5_0");
	}

	return rcasCS;
}

void Upscaling::Upscale()
{
	std::lock_guard<std::mutex> lock(settingsMutex);  // Lock for the duration of this function

	auto upscaleMode = GetUpscaleMethod();

	if (upscaleMode == UpscaleMethod::kNONE || upscaleMode == UpscaleMethod::kTAA)
		return;

	CheckResources();

	Hooks::BSGraphics_SetDirtyStates::func(false);

	auto state = State::GetSingleton();
	state->BeginPerfEvent("Upscaling");

	auto& context = state->context;

	ID3D11ShaderResourceView* inputTextureSRV;
	context->PSGetShaderResources(0, 1, &inputTextureSRV);

	inputTextureSRV->Release();

	ID3D11RenderTargetView* outputTextureRTV;
	ID3D11DepthStencilView* dsv;
	context->OMGetRenderTargets(1, &outputTextureRTV, &dsv);
	context->OMSetRenderTargets(0, nullptr, nullptr);

	outputTextureRTV->Release();

	if (dsv)
		dsv->Release();

	ID3D11Resource* inputTextureResource;
	inputTextureSRV->GetResource(&inputTextureResource);

	ID3D11Resource* outputTextureResource;
	outputTextureRTV->GetResource(&outputTextureResource);

	context->CopyResource(upscalingTempTexture->resource.get(), inputTextureResource);

	if (upscaleMode == UpscaleMethod::kDLSS)
		Streamline::GetSingleton()->Upscale(upscalingTempTexture);
	else if (upscaleMode == UpscaleMethod::kFSR)
		FidelityFX::GetSingleton()->Upscale(upscalingTempTexture);

	state->EndPerfEvent();

	if (GetUpscaleMethod() == UpscaleMethod::kDLSS) {
		state->BeginPerfEvent("Sharpening");

		context->CopyResource(inputTextureResource, upscalingTempTexture->resource.get());

		{
			auto dispatchCount = Util::GetScreenDispatchCount(false);

			{
				ID3D11ShaderResourceView* views[1] = { inputTextureSRV };
				context->CSSetShaderResources(0, ARRAYSIZE(views), views);

				ID3D11UnorderedAccessView* uavs[1] = { upscalingTempTexture->uav.get() };
				context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

				context->CSSetShader(GetRCASComputeShader(), nullptr, 0);

				context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
			}

			ID3D11ShaderResourceView* views[1] = { nullptr };
			context->CSSetShaderResources(0, ARRAYSIZE(views), views);

			ID3D11UnorderedAccessView* uavs[1] = { nullptr };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			ID3D11ComputeShader* shader = nullptr;
			context->CSSetShader(shader, nullptr, 0);
		}

		state->EndPerfEvent();
	}

	context->CopyResource(outputTextureResource, upscalingTempTexture->resource.get());
}

void Upscaling::CreateUpscalingResources()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

	main.texture->GetDesc(&texDesc);
	main.SRV->GetDesc(&srvDesc);
	main.UAV->GetDesc(&uavDesc);

	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	upscalingTempTexture = new Texture2D(texDesc);
	upscalingTempTexture->CreateSRV(srvDesc);
	upscalingTempTexture->CreateUAV(uavDesc);
}

void Upscaling::DestroyUpscalingResources()
{
	upscalingTempTexture->srv = nullptr;
	upscalingTempTexture->uav = nullptr;
	upscalingTempTexture->resource = nullptr;
	delete upscalingTempTexture;
}
