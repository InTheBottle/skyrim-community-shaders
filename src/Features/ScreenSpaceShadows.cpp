#include "ScreenSpaceShadows.h"

#include "State.h"
#include "Util.h"

#pragma warning(push)
#pragma warning(disable: 4838 4244)
#include "ScreenSpaceShadows/bend_sss_cpu.h"
#pragma warning(pop)

using RE::RENDER_TARGETS;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceShadows::BendSettings,
	SurfaceThickness,
	BilinearThreshold,
	ShadowContrast,
	IgnoreEdgePixels,
	UsePrecisionOffset,
	BilinearSamplingOffsetMode,
	DebugOutputEdgeMask,
	DebugOutputThreadIndex,
	DebugOutputWaveIndex)

void ScreenSpaceShadows::DrawSettings()
{
	if (ImGui::TreeNodeEx("General", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("SurfaceThickness", &bendSettings.SurfaceThickness, 0.005f, 0.05f);
		ImGui::SliderFloat("BilinearThreshold", &bendSettings.BilinearThreshold, 0.02f, 1.0f);
		ImGui::SliderFloat("ShadowContrast", &bendSettings.ShadowContrast, 0.0f, 4.0f);

		ImGui::Checkbox("IgnoreEdgePixels", (bool*)&bendSettings.IgnoreEdgePixels);
		ImGui::Checkbox("UsePrecisionOffset", (bool*)&bendSettings.UsePrecisionOffset);
		ImGui::Checkbox("BilinearSamplingOffsetMode", (bool*)&bendSettings.BilinearSamplingOffsetMode);
		ImGui::Checkbox("DebugOutputEdgeMask", (bool*)&bendSettings.DebugOutputEdgeMask);
		ImGui::Checkbox("DebugOutputThreadIndex", (bool*)&bendSettings.DebugOutputThreadIndex);
		ImGui::Checkbox("DebugOutputWaveIndex", (bool*)&bendSettings.DebugOutputWaveIndex);

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}
}

void ScreenSpaceShadows::ClearShaderCache()
{
	if (raymarch) {
		raymarch->Release();
		raymarch = nullptr;
	}
	if (blurH) {
		blurH->Release();
		blurH = nullptr;
	}
	if (blurV) {
		blurV->Release();
		blurV = nullptr;
	}
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeShader()
{
	if (!raymarch) {
		logger::debug("Compiling RaymarchCS");
		raymarch = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\RaymarchCS.hlsl", {}, "cs_5_0");
	}
	return raymarch;
}


ID3D11ComputeShader* ScreenSpaceShadows::GetComputeShaderBlurH()
{
	if (!blurH) {
		logger::debug("Compiling BlurCS HORIZONTAL");
		blurH = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\BlurCS.hlsl", { { "HORIZONTAL", nullptr } }, "cs_5_0");
	}
	return blurH;
}


ID3D11ComputeShader* ScreenSpaceShadows::GetComputeShaderBlurV()
{
	if (!blurV) {
		logger::debug("Compiling BlurCS VERTICAL");
		blurV = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\BlurCS.hlsl", { { "VERTICAL", nullptr } }, "cs_5_0");
	}
	return blurV;
}


void ScreenSpaceShadows::DrawShadows()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
	auto viewport = RE::BSGraphics::State::GetSingleton();

	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());

	auto state = State::GetSingleton();

	auto& directionNi = dirLight->GetWorldDirection();
	float3 light = { directionNi.x, directionNi.y, directionNi.z };
	light.Normalize();
	float4 lightProjection = float4(-light.x, -light.y, -light.z, 0.0f);
	lightProjection = DirectX::SimpleMath::Vector4::Transform(lightProjection, shadowState->GetRuntimeData().cameraData.getEye().viewProjMat);	
	float lightProjectionF[4] = { lightProjection.x, lightProjection.y, lightProjection.z, lightProjection.w};
				
	int viewportSize[2] = { (int)state->screenWidth, (int)state->screenHeight };
			
	int minRenderBounds[2] = { 0, 0 };
	int maxRenderBounds[2] = { 
		(int)((float)viewportSize[0] * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale), 
		(int)((float)viewportSize[1] * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale) 
	};

	auto dispatchList = Bend::BuildDispatchList(lightProjectionF, viewportSize, minRenderBounds, maxRenderBounds);
				
	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	context->CSSetShaderResources(0, 1, &depth.depthSRV);

	auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
	context->CSSetUnorderedAccessViews(0, 1, &shadowMask.UAV, nullptr);

	context->CSSetSamplers(0, 1, &pointBorderSampler);

	context->CSSetShader(GetComputeShader(), nullptr, 0);

	auto buffer = raymarchCB->CB();
	context->CSSetConstantBuffers(1, 1, &buffer);

	for (int i = 0; i < dispatchList.DispatchCount; i++)
	{
		auto dispatchData = dispatchList.Dispatch[i];
					
		RaymarchCB data{};
		data.LightCoordinate[0] = dispatchList.LightCoordinate_Shader[0];
		data.LightCoordinate[1] = dispatchList.LightCoordinate_Shader[1];
		data.LightCoordinate[2] = dispatchList.LightCoordinate_Shader[2];
		data.LightCoordinate[3] = dispatchList.LightCoordinate_Shader[3];

		data.WaveOffset[0] = dispatchData.WaveOffset_Shader[0];
		data.WaveOffset[1] = dispatchData.WaveOffset_Shader[1];

		data.FarDepthValue = 1.0f;
		data.NearDepthValue = 0.0f;

		data.InvDepthTextureSize[0] = 1.0f / (float)viewportSize[0];
		data.InvDepthTextureSize[1] = 1.0f / (float)viewportSize[1];
			
		data.settings = bendSettings;

		raymarchCB->Update(data);			

		context->Dispatch(dispatchData.WaveCount[0], dispatchData.WaveCount[1], dispatchData.WaveCount[2]);
	}

	ID3D11ShaderResourceView* views[1]{ nullptr };
	context->CSSetShaderResources(0, 1, views);

	ID3D11UnorderedAccessView* uavs[1]{ nullptr };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);

	buffer = nullptr;
	context->CSSetConstantBuffers(1, 1, &buffer);

	BlurShadowMask();
}

void ScreenSpaceShadows::BlurShadowMask()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];

	ID3D11ShaderResourceView* view = shadowMask.SRV;
	context->CSSetShaderResources(0, 1, &view);

	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

	view = depth.depthSRV;
	context->CSSetShaderResources(1, 1, &view);

	ID3D11UnorderedAccessView* uav = shadowMaskTemp->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	context->CSSetSamplers(0, 1, &linearSampler);

	context->CSSetShader(GetComputeShaderBlurH(), nullptr, 0);

	uint32_t dispatchX = (uint32_t)std::ceil(shadowMaskTemp->desc.Width / 32.0f);
	uint32_t dispatchY = (uint32_t)std::ceil(shadowMaskTemp->desc.Height / 32.0f);

	context->Dispatch(dispatchX, dispatchY, 1);

	view = nullptr;
	context->CSSetShaderResources(0, 1, &view);

	uav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	view = shadowMaskTemp->srv.get();
	context->CSSetShaderResources(0, 1, &view);

	uav = shadowMask.UAV;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	context->CSSetShader(GetComputeShaderBlurV(), nullptr, 0);

	context->Dispatch(dispatchX, dispatchY, 1);

	view = nullptr;
	context->CSSetShaderResources(0, 1, &view);

	uav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);
}

void ScreenSpaceShadows::Draw(const RE::BSShader*, const uint32_t)
{
}

void ScreenSpaceShadows::Load(json& o_json)
{
	if (o_json[GetName()].is_object())
		bendSettings = o_json[GetName()];

	Feature::Load(o_json);
}

void ScreenSpaceShadows::Save(json& o_json)
{
	o_json[GetName()] = bendSettings;
}

void ScreenSpaceShadows::RestoreDefaultSettings()
{
	bendSettings = {};
}

void ScreenSpaceShadows::SetupResources()
{
	raymarchCB = new ConstantBuffer(ConstantBufferDesc<RaymarchCB>());

	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

	{
		auto device = renderer->GetRuntimeData().forwarder;

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		samplerDesc.BorderColor[0] = 1.0f;
		samplerDesc.BorderColor[1] = 1.0f;
		samplerDesc.BorderColor[2] = 1.0f;
		samplerDesc.BorderColor[3] = 1.0f;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &pointBorderSampler));
	}

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
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &linearSampler));
	}

	{
		auto& shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSHADOW_MASK];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

		shadowMask.texture->GetDesc(&texDesc);
		shadowMask.SRV->GetDesc(&srvDesc);
		shadowMask.RTV->GetDesc(&rtvDesc);
		shadowMask.UAV->GetDesc(&uavDesc);

		{
			texDesc.Format = DXGI_FORMAT_R8_UNORM;
			srvDesc.Format = texDesc.Format;
			rtvDesc.Format = texDesc.Format;
			uavDesc.Format = texDesc.Format;

			shadowMaskTemp = new Texture2D(texDesc);
			shadowMaskTemp->CreateSRV(srvDesc);
			shadowMaskTemp->CreateUAV(uavDesc);
		}
	}
}

void ScreenSpaceShadows::Reset()
{
}

