#include "ScreenSpaceShadows.h"

#include "State.h"
#include "Util.h"

using RE::RENDER_TARGETS;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceShadows::Settings,
	MaxSamples,
	FarDistanceScale,
	FarThicknessScale,
	FarHardness,
	NearDistance,
	NearThickness,
	NearHardness,
	BlurRadius,
	BlurDropoff,
	Enabled)

void ScreenSpaceShadows::DrawSettings()
{
	if (ImGui::TreeNodeEx("General", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Screen-Space Shadows", &settings.Enabled);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Enables screen-space shadows.");
		}

		ImGui::SliderInt("Max Samples", (int*)&settings.MaxSamples, 1, 512);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Controls the accuracy of traced shadows.");
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Blur Filter", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Blur Radius", &settings.BlurRadius, 0, 1);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Blur radius.");
		}

		ImGui::SliderFloat("Blur Depth Dropoff", &settings.BlurDropoff, 0.001f, 0.1f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Blur depth dropoff.");
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Near Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Near Distance", &settings.NearDistance, 0.25f, 128);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Near Shadow Distance.");
		}

		ImGui::SliderFloat("Near Thickness", &settings.NearThickness, 0, 128);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Near Shadow Thickness.");
		}
		ImGui::SliderFloat("Near Hardness", &settings.NearHardness, 0, 64);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Near Shadow Hardness.");
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Far Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Far Distance Scale", &settings.FarDistanceScale, 0, 1);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Far Shadow Distance Scale.");
		}
		ImGui::SliderFloat("Far Thickness Scale", &settings.FarThicknessScale, 0, 1);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Far Shadow Thickness Scale.");
		}
		ImGui::SliderFloat("Far Hardness", &settings.FarHardness, 0, 64);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Far Shadow Hardness.");
		}

		ImGui::TreePop();
	}
}

void ScreenSpaceShadows::ClearShaderCache()
{
	if (raymarchProgram) {
		raymarchProgram->Release();
		raymarchProgram = nullptr;
	}
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeShader()
{
	if (!raymarchProgram) {
		logger::debug("Compiling raymarchProgram");
		raymarchProgram = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\RaymarchCS.hlsl", {}, "cs_5_0");
	}
	return raymarchProgram;
}

void ScreenSpaceShadows::DrawShadows()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());

	auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();

	auto viewport = RE::BSGraphics::State::GetSingleton();

	float resolutionX = State::GetSingleton()->screenWidth * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
	float resolutionY = State::GetSingleton()->screenHeight * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

	{
		RaymarchCB data{};

		data.BufferDim.x = (float)State::GetSingleton()->screenWidth;
		data.BufferDim.y = (float)State::GetSingleton()->screenHeight;

		data.RcpBufferDim.x = 1.0f / data.BufferDim.x;
		data.RcpBufferDim.y = 1.0f / data.BufferDim.y;

		data.DynamicRes.x = viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
		data.DynamicRes.y = viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

		data.DynamicRes.z = 1.0f / data.DynamicRes.x;
		data.DynamicRes.w = 1.0f / data.DynamicRes.y;

		for (int eyeIndex = 0; eyeIndex < (!REL::Module::IsVR() ? 1 : 2); eyeIndex++) {
			if (REL::Module::IsVR())
				data.ProjMatrix[eyeIndex] = shadowState->GetVRRuntimeData().cameraData.getEye(eyeIndex).projMat;
			else
				data.ProjMatrix[eyeIndex] = shadowState->GetRuntimeData().cameraData.getEye().projMat;

			data.InvProjMatrix[eyeIndex] = data.ProjMatrix[eyeIndex].Invert();
		}

		data.CameraData = Util::GetCameraData();

		auto& direction = dirLight->GetWorldDirection();
		float4 position{ -direction.x, -direction.y, -direction.z, 0.0f };

		auto viewMatrix = !REL::Module::IsVR() ? shadowState->GetRuntimeData().cameraData.getEye().viewMat : shadowState->GetVRRuntimeData().cameraData.getEye().viewMat;

		data.InvDirLightDirectionVS = float4::Transform(position, viewMatrix);

		data.ShadowDistance = 10000.0f;

		data.Settings = settings;

		raymarchCB->Update(data);
	}

	ID3D11Buffer* buffer[1] = { raymarchCB->CB() };
	context->CSSetConstantBuffers(0, 1, buffer);

	context->CSSetSamplers(0, 1, &computeSampler);

	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

	ID3D11ShaderResourceView* view = depth.depthSRV;
	context->CSSetShaderResources(0, 1, &view);

	ID3D11ShaderResourceView* stencilView = nullptr;
	if (REL::Module::IsVR()) {
		stencilView = depth.stencilSRV;
		context->CSSetShaderResources(89, 1, &stencilView);
	}

	auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
	context->CSSetUnorderedAccessViews(0, 1, &shadowMask.UAV, nullptr);

	auto shader = GetComputeShader();
	context->CSSetShader(shader, nullptr, 0);

	context->Dispatch((uint32_t)std::ceil(resolutionX / 32.0f), (uint32_t)std::ceil(resolutionY / 32.0f), 1);

	if (REL::Module::IsVR()) {
		stencilView = nullptr;
		context->CSSetShaderResources(89, 1, &stencilView);
	}
}

void ScreenSpaceShadows::Draw(const RE::BSShader*, const uint32_t)
{
}

void ScreenSpaceShadows::Load(json& o_json)
{
	if (o_json[GetName()].is_object())
		settings = o_json[GetName()];

	Feature::Load(o_json);
}

void ScreenSpaceShadows::Save(json& o_json)
{
	o_json[GetName()] = settings;
}

void ScreenSpaceShadows::RestoreDefaultSettings()
{
	settings = {};
}

void ScreenSpaceShadows::SetupResources()
{
	raymarchCB = new ConstantBuffer(ConstantBufferDesc<RaymarchCB>());

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	{
		logger::debug("Creating screenSpaceShadowsTexture");

		auto device = renderer->GetRuntimeData().forwarder;

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &computeSampler));
	}
}

void ScreenSpaceShadows::Reset()
{
}