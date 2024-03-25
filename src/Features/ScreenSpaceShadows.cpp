#include "ScreenSpaceShadows.h"

#include "State.h"
#include "Util.h"

#pragma warning(push)
#pragma warning(disable: 4838 4244)
#include "ScreenSpaceShadows/bend_sss_cpu.h"
#pragma warning(pop)

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
		ImGui::SliderFloat("SurfaceThickness", &bendSettings.SurfaceThickness, 0.005f, 0.05f);
		ImGui::SliderFloat("BilinearThreshold", &bendSettings.BilinearThreshold, 0.02f, 1.0f);
		ImGui::SliderFloat("ShadowContrast", &bendSettings.ShadowContrast, 1.0f, 4.0f);
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

enum class GrassShaderTechniques
{
	RenderDepth = 8,
};

void ScreenSpaceShadows::ModifyGrass(const RE::BSShader*, const uint32_t descriptor)
{
	const auto technique = descriptor & 0b1111;
	if (technique != static_cast<uint32_t>(GrassShaderTechniques::RenderDepth)) {
		ModifyLighting(nullptr, 0);
	}
}

enum class DistantTreeShaderTechniques
{
	DistantTreeBlock = 0,
	Depth = 1,
};

void ScreenSpaceShadows::ModifyDistantTree(const RE::BSShader*, const uint32_t descriptor)
{
	const auto technique = descriptor & 1;
	if (technique != static_cast<uint32_t>(DistantTreeShaderTechniques::Depth)) {
		ModifyLighting(nullptr, 0);
	}
}

enum class LightingShaderTechniques
{
	None = 0,
	Envmap = 1,
	Glowmap = 2,
	Parallax = 3,
	Facegen = 4,
	FacegenRGBTint = 5,
	Hair = 6,
	ParallaxOcc = 7,
	MTLand = 8,
	LODLand = 9,
	Snow = 10,  // unused
	MultilayerParallax = 11,
	TreeAnim = 12,
	LODObjects = 13,
	MultiIndexSparkle = 14,
	LODObjectHD = 15,
	Eye = 16,
	Cloud = 17,  // unused
	LODLandNoise = 18,
	MTLandLODBlend = 19,
	Outline = 20,
};

uint32_t GetTechnique(uint32_t descriptor)
{
	return 0x3F & (descriptor >> 24);
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

void ScreenSpaceShadows::ModifyLighting(const RE::BSShader*, const uint32_t)
{
	if (!loaded)
		return;

	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;

	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());

	bool skyLight = true;
	if (auto sky = RE::Sky::GetSingleton())
		skyLight = sky->mode.get() == RE::Sky::Mode::kFull;

	if (dirLight && skyLight) {
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();

		auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();

		bool enableSSS = true;

		if (shadowState->GetRuntimeData().cubeMapRenderTarget == RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS) {
			enableSSS = false;

		} else if (!renderedScreenCamera && settings.Enabled) {
			renderedScreenCamera = true;

			// Backup the game state
			struct OldState
			{
				ID3D11ShaderResourceView* srvs[2];
				ID3D11SamplerState* sampler;
				ID3D11ComputeShader* shader;
				ID3D11Buffer* buffer;
				ID3D11UnorderedAccessView* uav;
				ID3D11ClassInstance* instance;
				UINT numInstances;
			};

			OldState old{};
			context->CSGetShaderResources(0, 2, old.srvs);
			context->CSGetSamplers(0, 1, &old.sampler);
			context->CSGetShader(&old.shader, &old.instance, &old.numInstances);
			context->CSGetConstantBuffers(0, 1, &old.buffer);
			context->CSGetUnorderedAccessViews(0, 1, &old.uav);

			{
				auto viewport = RE::BSGraphics::State::GetSingleton();

				auto& directionNi = dirLight->GetWorldDirection();
				float3 light = { directionNi.x, directionNi.y, directionNi.z };
				light.Normalize();
				float4 lightProjection = float4(-light.x, -light.y, -light.z, 0.0f);
				lightProjection = DirectX::SimpleMath::Vector4::Transform(lightProjection, shadowState->GetRuntimeData().cameraData.getEye().viewProjMat);	
				float lightProjectionF[4] = { lightProjection.x, lightProjection.y, lightProjection.z, lightProjection.w};
				
				int viewportSize[2] = { (int)screenSpaceShadowsTexture->desc.Width, (int)screenSpaceShadowsTexture->desc.Height };
			
				int minRenderBounds[2] = { 0, 0 };
				int maxRenderBounds[2] = { 
					(int)((float)viewportSize[0] * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale), 
					(int)((float)viewportSize[1] * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale) 
				};

				auto dispatchList = Bend::BuildDispatchList(lightProjectionF, viewportSize, minRenderBounds, maxRenderBounds, false, 64);
				
				auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
				context->CSSetShaderResources(0, 1, &depth.depthSRV);

				ID3D11UnorderedAccessView* uav = screenSpaceShadowsTexture->uav.get();
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

				context->CSSetSamplers(0, 1, &pointBorderSampler);

				context->CSSetShader(GetComputeShader(), nullptr, 0);

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
					ID3D11Buffer* buffer = nullptr;
					context->CSSetConstantBuffers(0, 1, &buffer);

					raymarchCB->Update(data);
					
					buffer = raymarchCB->CB();
					context->CSSetConstantBuffers(0, 1, &buffer);

					context->Dispatch(dispatchData.WaveCount[0], dispatchData.WaveCount[1], dispatchData.WaveCount[2]);
				}
			}

			// Restore the game state
			context->CSSetShaderResources(0, 2, old.srvs);
			for (uint8_t i = 0; i < 2; i++)
				if (old.srvs[i])
					old.srvs[i]->Release();

			context->CSSetSamplers(0, 1, &old.sampler);
			if (old.sampler)
				old.sampler->Release();

			context->CSSetShader(old.shader, &old.instance, old.numInstances);
			if (old.shader)
				old.shader->Release();

			context->CSSetConstantBuffers(0, 1, &old.buffer);
			if (old.buffer)
				old.buffer->Release();

			context->CSSetUnorderedAccessViews(0, 1, &old.uav, nullptr);
			if (old.uav)
				old.uav->Release();
		}

		PerPass data{};
		data.EnableSSS = enableSSS && settings.Enabled;
		perPass->Update(data);

		if (renderedScreenCamera) {
			auto shadowMask = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
			ID3D11ShaderResourceView* views[2]{};
			views[0] = shadowMask.depthSRV;
			views[1] = screenSpaceShadowsTexture->srv.get();
			context->PSSetShaderResources(20, ARRAYSIZE(views), views);
		}
	} else {
		PerPass data{};
		data.EnableSSS = false;
		perPass->Update(data);
	}

	ID3D11Buffer* buffers[1]{};
	buffers[0] = perPass->CB();
	context->PSSetConstantBuffers(5, ARRAYSIZE(buffers), buffers);

	context->PSSetSamplers(14, 1, &pointBorderSampler);
}

void ScreenSpaceShadows::Draw(const RE::BSShader* shader, const uint32_t descriptor)
{
	switch (shader->shaderType.get()) {
	case RE::BSShader::Type::Grass:
		ModifyGrass(shader, descriptor);
		break;
	case RE::BSShader::Type::DistantTree:
		ModifyDistantTree(shader, descriptor);
		break;
	case RE::BSShader::Type::Lighting:
		ModifyLighting(shader, descriptor);
		break;
	}
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
	perPass = new ConstantBuffer(ConstantBufferDesc<PerPass>());
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

	{
		auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		main.texture->GetDesc(&texDesc);
		texDesc.Format = DXGI_FORMAT_R16_FLOAT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
		screenSpaceShadowsTexture = new Texture2D(texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		main.SRV->GetDesc(&srvDesc);
		srvDesc.Format = texDesc.Format;
		screenSpaceShadowsTexture->CreateSRV(srvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;
		screenSpaceShadowsTexture->CreateUAV(uavDesc);
	}
}

void ScreenSpaceShadows::Reset()
{
	renderedScreenCamera = false;
}

bool ScreenSpaceShadows::HasShaderDefine(RE::BSShader::Type shaderType)
{
	switch (shaderType) {
	case RE::BSShader::Type::Lighting:
	case RE::BSShader::Type::Grass:
	case RE::BSShader::Type::DistantTree:
		return true;
	default:
		return false;
	}
}