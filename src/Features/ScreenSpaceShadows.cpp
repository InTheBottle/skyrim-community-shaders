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

	auto dispatchList = Bend::BuildDispatchList(lightProjectionF, viewportSize, minRenderBounds, maxRenderBounds, false, 64);
				
	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	context->CSSetShaderResources(0, 1, &depth.depthSRV);

	auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
	context->CSSetUnorderedAccessViews(0, 1, &shadowMask.UAV, nullptr);

	context->CSSetSamplers(0, 1, &pointBorderSampler);

	context->CSSetShader(GetComputeShader(), nullptr, 0);

	auto buffer = raymarchCB->CB();
	context->CSSetConstantBuffers(0, 1, &buffer);

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
		logger::debug("Creating screenSpaceShadowsTexture");

		auto device = renderer->GetRuntimeData().forwarder;

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		samplerDesc.BorderColor[0] = 0.0f;
		samplerDesc.BorderColor[1] = 0.0f;
		samplerDesc.BorderColor[2] = 0.0f;
		samplerDesc.BorderColor[3] = 0.0f;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &pointBorderSampler));
	}
}

void ScreenSpaceShadows::Reset()
{
}