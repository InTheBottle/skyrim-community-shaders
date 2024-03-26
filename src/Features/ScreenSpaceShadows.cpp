#include "ScreenSpaceShadows.h"

#include "State.h"
#include "Util.h"
#include <Bindings.h>

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
	if (downscaleDepthCS) {
		downscaleDepthCS->Release();
		downscaleDepthCS = nullptr;
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

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeShaderDownscale()
{
	if (!downscaleDepthCS) {
		logger::debug("Compiling DownscaleDepthCS");
		downscaleDepthCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\DownscaleDepthCS.hlsl", {}, "cs_5_0");
	}
	return downscaleDepthCS;
}

void ScreenSpaceShadows::DownscaleDepth()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

	ID3D11ShaderResourceView* view = depth.depthSRV;
	context->CSSetShaderResources(0, 1, &view);

	auto uav = downscaledDepth->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	auto shader = GetComputeShaderDownscale();
	context->CSSetShader(shader, nullptr, 0);

	context->Dispatch((uint32_t)std::ceil(downscaledDepth->desc.Width / 32.0f), (uint32_t)std::ceil(downscaledDepth->desc.Height / 32.0f), 1);

	shader = nullptr;
	context->CSSetShader(shader, nullptr, 0);

	view = nullptr;
	context->CSSetShaderResources(0, 1, &view);

	uav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

}

void ScreenSpaceShadows::DrawShadows()
{
	DownscaleDepth();

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());

	auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();

	//auto viewport = RE::BSGraphics::State::GetSingleton();

	//float resolutionX = State::GetSingleton()->screenWidth * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
	//float resolutionY = State::GetSingleton()->screenHeight * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

	{
		ID3D11ShaderResourceView* view = nullptr;
		context->PSSetShaderResources(14, 1, &view);
	}

	{
		RaymarchCB data{};

		data.BufferDim.x = (float)downscaledDepth->desc.Width;
		data.BufferDim.y = (float)downscaledDepth->desc.Height;

		data.RcpBufferDim.x = 1.0f / data.BufferDim.x;
		data.RcpBufferDim.y = 1.0f / data.BufferDim.y;

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

		ID3D11Buffer* buffer[1] = { raymarchCB->CB() };
		context->CSSetConstantBuffers(1, 1, buffer);

		context->CSSetSamplers(0, 1, &computeSampler);

		auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		auto normalRoughness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];

		ID3D11ShaderResourceView* views[2] = { downscaledDepth->srv.get(), normalRoughness.SRV };
		context->CSSetShaderResources(0, 2, views);

		ID3D11ShaderResourceView* stencilView = nullptr;
		if (REL::Module::IsVR()) {
			stencilView = depth.stencilSRV;
			context->CSSetShaderResources(89, 1, &stencilView);
		}

	//	auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
		auto uav = lqShadows->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

		auto shader = GetComputeShader();
		context->CSSetShader(shader, nullptr, 0);

		context->Dispatch((uint32_t)std::ceil(downscaledDepth->desc.Width / 32.0f), (uint32_t)std::ceil(downscaledDepth->desc.Height / 32.0f), 1);

		if (REL::Module::IsVR()) {
			stencilView = nullptr;
			context->CSSetShaderResources(89, 1, &stencilView);
		}
	}

	ID3D11ShaderResourceView* views[2]{ nullptr, nullptr };
	context->CSSetShaderResources(0, 2, views);

	ID3D11UnorderedAccessView* uavs[1]{ nullptr };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	ID3D11Buffer* buffer = nullptr;
	context->CSSetConstantBuffers(1, 1, &buffer);

	context->CSSetShader(nullptr, nullptr, 0);
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

	{
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

		main.texture->GetDesc(&texDesc);
		main.SRV->GetDesc(&srvDesc);
		main.RTV->GetDesc(&rtvDesc);
		main.UAV->GetDesc(&uavDesc);

		texDesc.Format = DXGI_FORMAT_R8_UNORM;
		srvDesc.Format = texDesc.Format;
		rtvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		texDesc.Width /= 2;
		texDesc.Height /= 2;

		lqShadows = new Texture2D(texDesc);
		lqShadows->CreateSRV(srvDesc);
		lqShadows->CreateUAV(uavDesc);
		
		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		srvDesc.Format = texDesc.Format;
		rtvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		downscaledDepth = new Texture2D(texDesc);
		downscaledDepth->CreateSRV(srvDesc);
		downscaledDepth->CreateUAV(uavDesc);
	}
}

void ScreenSpaceShadows::Reset()
{
}