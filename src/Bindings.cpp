#include "Bindings.h"
#include "State.h"
#include "Util.h"

void Bindings::DepthStencilStateSetDepthMode(RE::BSGraphics::DepthStencilDepthMode a_mode)
{
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
	GET_INSTANCE_MEMBER(depthStencilDepthMode, state)
	GET_INSTANCE_MEMBER(depthStencilDepthModePrevious, state)
	GET_INSTANCE_MEMBER(stateUpdateFlags, state)

	if (depthStencilDepthMode != a_mode) {
		depthStencilDepthMode = a_mode;
		if (depthStencilDepthModePrevious != a_mode)
			stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
		else
			stateUpdateFlags.reset(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
	}
}

void Bindings::AlphaBlendStateSetMode(uint32_t a_mode)
{
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
	GET_INSTANCE_MEMBER(alphaBlendMode, state)
	GET_INSTANCE_MEMBER(stateUpdateFlags, state)

	if (alphaBlendMode != a_mode) {
		alphaBlendMode = a_mode;
		stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
	}
}

void Bindings::AlphaBlendStateSetAlphaToCoverage(uint32_t a_value)
{
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
	GET_INSTANCE_MEMBER(alphaBlendAlphaToCoverage, state)
	GET_INSTANCE_MEMBER(stateUpdateFlags, state)

	if (alphaBlendAlphaToCoverage != a_value) {
		alphaBlendAlphaToCoverage = a_value;
		stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
	}
}

void Bindings::AlphaBlendStateSetWriteMode(uint32_t a_value)
{
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
	GET_INSTANCE_MEMBER(alphaBlendWriteMode, state)
	GET_INSTANCE_MEMBER(stateUpdateFlags, state)

	if (alphaBlendWriteMode != a_value) {
		alphaBlendWriteMode = a_value;
		stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
	}
}

struct DepthStates
{
	ID3D11DepthStencilState* a[6][40];
};

struct BlendStates
{
	ID3D11BlendState* a[7][2][13][2];
};

void SetupRenderTarget(RE::RENDER_TARGET target, D3D11_TEXTURE2D_DESC texDesc, D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc, D3D11_RENDER_TARGET_VIEW_DESC rtvDesc, D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc, DXGI_FORMAT format)
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto device = renderer->GetRuntimeData().forwarder;

	texDesc.Format = format;
	srvDesc.Format = format;
	rtvDesc.Format = format;
	uavDesc.Format = format;

	auto& data = renderer->GetRuntimeData().renderTargets[target];
	DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, &data.texture));
	DX::ThrowIfFailed(device->CreateShaderResourceView(data.texture, &srvDesc, &data.SRV));
	DX::ThrowIfFailed(device->CreateRenderTargetView(data.texture, &rtvDesc, &data.RTV));
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(data.texture, &uavDesc, &data.UAV));
}

void Bindings::SetupResources()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();

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

		// Available targets:
		// MAIN ONLY ALPHA
		// WATER REFLECTIONS
		// BLURFULL_BUFFER
		// LENSFLAREVIS
		// SAO DOWNSCALED
		// SAO CAMERAZ+MIP_LEVEL_0_ESRAM
		// SAO_RAWAO_DOWNSCALED
		// SAO_RAWAO_PREVIOUS_DOWNSCALDE
		// SAO_TEMP_BLUR_DOWNSCALED
		// INDIRECT
		// INDIRECT_DOWNSCALED
		// RAWINDIRECT
		// RAWINDIRECT_DOWNSCALED
		// RAWINDIRECT_PREVIOUS
		// RAWINDIRECT_PREVIOUS_DOWNSCALED
		// RAWINDIRECT_SWAP
		// VOLUMETRIC_LIGHTING_HALF_RES
		// VOLUMETRIC_LIGHTING_BLUR_HALF_RES
		// VOLUMETRIC_LIGHTING_QUARTER_RES
		// VOLUMETRIC_LIGHTING_BLUR_QUARTER_RES
		// TEMPORAL_AA_WATER_1
		// TEMPORAL_AA_WATER_2

#define ALBEDO RE::RENDER_TARGETS::kINDIRECT
#define SPECULAR RE::RENDER_TARGETS::kINDIRECT_DOWNSCALED
#define REFLECTANCE RE::RENDER_TARGETS::kRAWINDIRECT
#define NORMALROUGHNESS RE::RENDER_TARGETS::kRAWINDIRECT_DOWNSCALED

		// Albedo
		SetupRenderTarget(ALBEDO, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R8G8B8A8_UNORM);
		// Specular
		SetupRenderTarget(SPECULAR, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R11G11B10_FLOAT);
		// Reflectance
		SetupRenderTarget(REFLECTANCE, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R8G8B8A8_UNORM);
		// Normal + Roughness
		SetupRenderTarget(NORMALROUGHNESS, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R8G8B8A8_UNORM);


		{
			texDesc.Format = DXGI_FORMAT_R8_UNORM;
			srvDesc.Format = texDesc.Format;
			rtvDesc.Format = texDesc.Format;
			uavDesc.Format = texDesc.Format;

			filteredShadowTexture = new Texture2D(texDesc);
			filteredShadowTexture->CreateSRV(srvDesc);
			filteredShadowTexture->CreateUAV(uavDesc);
		}
	}

	{
		deferredCB = new ConstantBuffer(ConstantBufferDesc<DeferredCB>());
	}
}

void Bindings::Reset()
{
}

void Bindings::UpdateConstantBuffer()
{
	auto state = State::GetSingleton();
	auto viewport = RE::BSGraphics::State::GetSingleton();

	DeferredCB data{};

	auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();

	if (REL::Module::IsVR()) {
		data.ViewMatrix[0] = shadowState->GetVRRuntimeData().cameraData.getEye(0).viewMat;
		data.ViewMatrix[1] = shadowState->GetVRRuntimeData().cameraData.getEye(1).viewMat;
		data.ProjMatrix[0] = shadowState->GetVRRuntimeData().cameraData.getEye(0).projMat;
		data.ProjMatrix[1] = shadowState->GetVRRuntimeData().cameraData.getEye(1).projMat;
		data.ViewProjMatrix[0] = shadowState->GetVRRuntimeData().cameraData.getEye(0).viewProjMat;
		data.ViewProjMatrix[1] = shadowState->GetVRRuntimeData().cameraData.getEye(1).viewProjMat;
		data.InvViewMatrix[0] = shadowState->GetVRRuntimeData().cameraData.getEye(0).viewMat.Invert();
		data.InvViewMatrix[1] = shadowState->GetVRRuntimeData().cameraData.getEye(1).viewMat.Invert();
		data.InvProjMatrix[0] = shadowState->GetVRRuntimeData().cameraData.getEye(0).projMat.Invert();
		data.InvProjMatrix[0] = shadowState->GetVRRuntimeData().cameraData.getEye(1).projMat.Invert();
	} else {
		data.ViewMatrix[0] = shadowState->GetRuntimeData().cameraData.getEye(0).viewMat;
		data.ProjMatrix[0] = shadowState->GetRuntimeData().cameraData.getEye(0).projMat;
		data.ViewProjMatrix[0] = shadowState->GetRuntimeData().cameraData.getEye(0).viewProjMat;
		data.InvViewMatrix[0] = shadowState->GetRuntimeData().cameraData.getEye(0).viewMat.Invert();
		data.InvProjMatrix[0] = shadowState->GetRuntimeData().cameraData.getEye(0).projMat.Invert();
	}

	auto accumulator = RE::BSGraphics::BSShaderAccumulator::GetCurrentAccumulator();
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());

	data.DirLightColor = { dirLight->GetLightRuntimeData().diffuse.red, dirLight->GetLightRuntimeData().diffuse.green, dirLight->GetLightRuntimeData().diffuse.blue, 1.0f };

	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	data.DirLightColor *= !REL::Module::IsVR() ? imageSpaceManager->GetRuntimeData().data.baseData.hdr.sunlightScale : imageSpaceManager->GetVRRuntimeData().data.baseData.hdr.sunlightScale;

	auto& direction = dirLight->GetWorldDirection();
	float4 position{ -direction.x, -direction.y, -direction.z, 0.0f };

	data.DirLightDirectionVS[0] = float4::Transform(position, data.ViewMatrix[0]);
	data.DirLightDirectionVS[0].Normalize();

	data.DirLightDirectionVS[1] = float4::Transform(position, data.ViewMatrix[1]);
	data.DirLightDirectionVS[1].Normalize();

	auto& shaderManager = RE::BSShaderManager::State::GetSingleton();
	RE::NiTransform& dalcTransform = shaderManager.directionalAmbientTransform;
	Util::StoreTransform3x4NoScale(data.DirectionalAmbient, dalcTransform);

	data.BufferDim.x = state->screenWidth;
	data.BufferDim.y = state->screenHeight;

	data.RcpBufferDim.x = 1.0f / State::GetSingleton()->screenWidth;
	data.RcpBufferDim.y = 1.0f / State::GetSingleton()->screenHeight;

	auto useTAA = !REL::Module::IsVR() ? imageSpaceManager->GetRuntimeData().BSImagespaceShaderISTemporalAA->taaEnabled : imageSpaceManager->GetVRRuntimeData().BSImagespaceShaderISTemporalAA->taaEnabled;
	data.FrameCount = viewport->uiFrameCount * (useTAA || state->upscalerLoaded);

	data.CameraData = Util::GetCameraData();

	deferredCB->Update(data);
}

void Bindings::StartDeferred()
{
	if (!inWorld)
		return;

	static bool setup = false;
	if (!setup) {
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto device = renderer->GetRuntimeData().forwarder;

		static BlendStates* blendStates = (BlendStates*)REL::RelocationID(524749, 411364).address();

		{
			forwardBlendStates[0] = blendStates->a[0][0][1][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[0]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = true;
			for (uint i = 1; i < 8; i++) {
				blendDesc.RenderTarget[i] = blendDesc.RenderTarget[0];
			}

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[0]));
		}

		{
			forwardBlendStates[1] = blendStates->a[0][0][10][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[1]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = true;
			for (uint i = 1; i < 8; i++) {
				blendDesc.RenderTarget[i] = blendDesc.RenderTarget[0];
			}

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[1]));
		}

		{
			forwardBlendStates[2] = blendStates->a[1][0][1][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[2]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = true;
			for (uint i = 1; i < 8; i++) {
				blendDesc.RenderTarget[i] = blendDesc.RenderTarget[0];
			}

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[2]));
		}

		{
			forwardBlendStates[3] = blendStates->a[1][0][11][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[3]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = true;
			for (uint i = 1; i < 8; i++) {
				blendDesc.RenderTarget[i] = blendDesc.RenderTarget[0];
			}

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[3]));
		}
		setup = true;
	}

	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();

	// Backup original render targets
	for (uint i = 0; i < 4; i++) {
		forwardRenderTargets[i] = state->GetRuntimeData().renderTargets[i];
	}

	RE::RENDER_TARGET targets[6]{
		RE::RENDER_TARGET::kMAIN,
		RE::RENDER_TARGET::kMOTION_VECTOR,
		NORMALROUGHNESS,
		ALBEDO,
		SPECULAR,
		REFLECTANCE,
	};

	for (uint i = 0; i < 6; i++) {
		state->GetRuntimeData().renderTargets[i] = targets[i];                                             // We must use unused targets to be indexable
		state->GetRuntimeData().setRenderTargetMode[i] = RE::BSGraphics::SetRenderTargetMode::SRTM_CLEAR;  // Dirty from last frame, this calls ClearRenderTargetView once
	}

	// Improved snow shader
	if (forwardRenderTargets[3] != RE::RENDER_TARGET::kNONE) {
		state->GetRuntimeData().renderTargets[6] = forwardRenderTargets[3];                                // We must use unused targets to be indexable
		state->GetRuntimeData().setRenderTargetMode[6] = RE::BSGraphics::SetRenderTargetMode::SRTM_CLEAR;  // Dirty from last frame, this calls ClearRenderTargetView once
	}

	state->GetRuntimeData().stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again

	static BlendStates* blendStates = (BlendStates*)REL::RelocationID(524749, 411364).address();

	// Set modified blend states
	blendStates->a[0][0][1][0] = deferredBlendStates[0];
	blendStates->a[0][0][10][0] = deferredBlendStates[1];
	blendStates->a[1][0][1][0] = deferredBlendStates[2];
	blendStates->a[1][0][11][0] = deferredBlendStates[3];

	state->GetRuntimeData().stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

	deferredPass = true;
}

void Bindings::NormalMappingShadows()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	{
		auto normalRoughness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
		auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
		auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

		ID3D11ShaderResourceView* srvs[3]{
			normalRoughness.SRV,
			shadowMask.SRV,
			depth.depthSRV
		};

		context->CSSetShaderResources(0, 3, srvs);

		auto main = renderer->GetRuntimeData().renderTargets[forwardRenderTargets[0]];
		auto normals = renderer->GetRuntimeData().renderTargets[forwardRenderTargets[2]];

		ID3D11UnorderedAccessView* uavs[1]{ filteredShadowTexture->uav.get() };
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

		auto buffer = deferredCB->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);

		auto shader = GetComputeDeferredNormalMappingShadows();
		context->CSSetShader(shader, nullptr, 0);

		auto state = State::GetSingleton();
		auto viewport = RE::BSGraphics::State::GetSingleton();

		float resolutionX = state->screenWidth * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
		float resolutionY = state->screenHeight * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

		uint32_t dispatchX = (uint32_t)std::ceil(resolutionX / 32.0f);
		uint32_t dispatchY = (uint32_t)std::ceil(resolutionY / 32.0f);

		context->Dispatch(dispatchX, dispatchY, 1);
	}

	ID3D11ShaderResourceView* views[3]{ nullptr, nullptr, nullptr };
	context->CSSetShaderResources(0, 3, views);

	ID3D11UnorderedAccessView* uavs[1]{ nullptr };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	ID3D11Buffer* buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &buffer);

	context->CSSetShader(nullptr, nullptr, 0);
}

void Bindings::DeferredPasses()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	UpdateConstantBuffer();

	NormalMappingShadows();

	{
		auto specular = renderer->GetRuntimeData().renderTargets[SPECULAR];
		auto albedo = renderer->GetRuntimeData().renderTargets[ALBEDO];
		auto reflectance = renderer->GetRuntimeData().renderTargets[REFLECTANCE];
		auto normalRoughness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
		auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

		ID3D11ShaderResourceView* srvs[6]{
			specular.SRV,
			albedo.SRV,
			reflectance.SRV,
			normalRoughness.SRV,
			filteredShadowTexture->srv.get(),
			depth.depthSRV
		};

		context->CSSetShaderResources(0, 6, srvs);

		auto main = renderer->GetRuntimeData().renderTargets[forwardRenderTargets[0]];
		auto normals = renderer->GetRuntimeData().renderTargets[forwardRenderTargets[2]];

		ID3D11UnorderedAccessView* uavs[2]{ main.UAV, normals.UAV };
		context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

		auto buffer = deferredCB->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);

		auto shader = GetComputeDeferredComposite();
		context->CSSetShader(shader, nullptr, 0);

		auto state = State::GetSingleton();
		auto viewport = RE::BSGraphics::State::GetSingleton();

		float resolutionX = state->screenWidth * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
		float resolutionY = state->screenHeight * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

		uint32_t dispatchX = (uint32_t)std::ceil(resolutionX / 32.0f);
		uint32_t dispatchY = (uint32_t)std::ceil(resolutionY / 32.0f);

		context->Dispatch(dispatchX, dispatchY, 1);

		shader = GetComputeDeferredComposite();
		context->CSSetShader(shader, nullptr, 0);
	}

	ID3D11ShaderResourceView* views[6]{ nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	context->CSSetShaderResources(0, 6, views);

	ID3D11UnorderedAccessView* uavs[2]{ nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);

	ID3D11Buffer* buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &buffer);

	context->CSSetShader(nullptr, nullptr, 0);
}

void Bindings::EndDeferred()
{
	if (!inWorld)
		return;

	inWorld = false;

	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();

	// Do not render to our targets past this point
	for (uint i = 0; i < 4; i++) {
		state->GetRuntimeData().renderTargets[i] = forwardRenderTargets[i];
	}

	for (uint i = 4; i < 8; i++) {
		state->GetRuntimeData().renderTargets[i] = RE::RENDER_TARGET::kNONE;
	}

	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	DeferredPasses();  // Perform deferred passes and composite forward buffers

	state->GetRuntimeData().stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again

	static BlendStates* blendStates = (BlendStates*)REL::RelocationID(524749, 411364).address();

	// Restore modified blend states
	blendStates->a[0][0][1][0] = forwardBlendStates[0];
	blendStates->a[0][0][10][0] = forwardBlendStates[1];
	blendStates->a[1][0][1][0] = forwardBlendStates[2];
	blendStates->a[1][0][11][0] = forwardBlendStates[3];

	state->GetRuntimeData().stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);

	deferredPass = false;
}

void Bindings::ClearShaderCache()
{
	if (deferredNormalMappingShadowsCS) {
		deferredNormalMappingShadowsCS->Release();
		deferredNormalMappingShadowsCS = nullptr;
	}
	if (deferredCompositeCS) {
		deferredCompositeCS->Release();
		deferredCompositeCS = nullptr;
	}
}

ID3D11ComputeShader* Bindings::GetComputeDeferredNormalMappingShadows()
{
	if (!deferredNormalMappingShadowsCS) {
		logger::debug("Compiling DeferredNormalMappingShadowsCS");
		deferredNormalMappingShadowsCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\DeferredNormalMappingShadowsCS.hlsl", {}, "cs_5_0");
	}
	return deferredNormalMappingShadowsCS;
}

ID3D11ComputeShader* Bindings::GetComputeDeferredComposite()
{
	if (!deferredCompositeCS) {
		logger::debug("Compiling DeferredCompositeCS");
		deferredCompositeCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\DeferredCompositeCS.hlsl", {}, "cs_5_0");
	}
	return deferredCompositeCS;
}