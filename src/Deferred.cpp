#include "Deferred.h"

#include "ShaderCache.h"
#include "State.h"
#include "Util.h"

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
	auto& device = State::GetSingleton()->device;

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

void Deferred::SetupResources()
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

		// Albedo
		SetupRenderTarget(ALBEDO, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R8G8B8A8_UNORM);
		// Specular
		SetupRenderTarget(SPECULAR, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R11G11B10_FLOAT);
		// Reflectance
		SetupRenderTarget(REFLECTANCE, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R8G8B8A8_UNORM);
		// Normal + Roughness
		SetupRenderTarget(NORMALROUGHNESS, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R8G8B8A8_UNORM);
		// Masks
		SetupRenderTarget(MASKS, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R8G8B8A8_UNORM);
		// Additional Masks
		SetupRenderTarget(MASKS2, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R8G8B8A8_UNORM);
	}

	{
		deferredCB = new ConstantBuffer(ConstantBufferDesc<DeferredCB>());
	}

	{
		auto& device = State::GetSingleton()->device;

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
}

void Deferred::UpdateConstantBuffer()
{
	DeferredCB data{};

	auto state = State::GetSingleton();

	data.BufferDim.x = state->screenWidth;
	data.BufferDim.y = state->screenHeight;
	data.BufferDim.z = 1.0f / data.BufferDim.x;
	data.BufferDim.w = 1.0f / data.BufferDim.y;

	data.CameraData = Util::GetCameraData();

	auto& shaderManager = RE::BSShaderManager::State::GetSingleton();
	RE::NiTransform& dalcTransform = shaderManager.directionalAmbientTransform;
	Util::StoreTransform3x4NoScale(data.DirectionalAmbient, dalcTransform);

	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();

	auto useTAA = !REL::Module::IsVR() ? imageSpaceManager->GetRuntimeData().BSImagespaceShaderISTemporalAA->taaEnabled : imageSpaceManager->GetVRRuntimeData().BSImagespaceShaderISTemporalAA->taaEnabled;
	data.FrameCount = useTAA ? RE::BSGraphics::State::GetSingleton()->uiFrameCount : 0;

	deferredCB->Update(data);
}

void Deferred::ZPrepassPasses()
{
	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
	GET_INSTANCE_MEMBER(stateUpdateFlags, shadowState)

	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again

	for (auto* feature : Feature::GetFeatureList()) {
		if (feature->loaded) {
			feature->ZPrepass();
		}
	}
}

void Deferred::StartDeferred()
{
	if (!inWorld)
		return;

	auto& shaderCache = SIE::ShaderCache::Instance();

	if (!shaderCache.IsEnabled())
		return;

	State::GetSingleton()->UpdateSharedData();

	static bool setup = false;
	if (!setup) {
		auto& device = State::GetSingleton()->device;

		static BlendStates* blendStates = (BlendStates*)REL::RelocationID(524749, 411364).address();

		{
			forwardBlendStates[0] = blendStates->a[0][0][1][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[0]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[0]));
		}

		{
			forwardBlendStates[1] = blendStates->a[0][0][10][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[1]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[1]));
		}

		{
			forwardBlendStates[2] = blendStates->a[1][0][1][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[2]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[2]));
		}

		{
			forwardBlendStates[3] = blendStates->a[1][0][11][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[3]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[3]));
		}

		{
			forwardBlendStates[4] = blendStates->a[2][0][1][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[4]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[4]));
		}

		{
			forwardBlendStates[5] = blendStates->a[2][0][11][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[5]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[5]));
		}

		{
			forwardBlendStates[6] = blendStates->a[3][0][11][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[6]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[6]));
		}
		setup = true;
	}

	auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
	GET_INSTANCE_MEMBER(renderTargets, shadowState)
	GET_INSTANCE_MEMBER(setRenderTargetMode, shadowState)
	GET_INSTANCE_MEMBER(stateUpdateFlags, shadowState)

	// Backup original render targets
	for (uint i = 0; i < 4; i++) {
		forwardRenderTargets[i] = renderTargets[i];
	}

	RE::RENDER_TARGET targets[8]{
		RE::RENDER_TARGET::kMAIN,
		RE::RENDER_TARGET::kMOTION_VECTOR,
		NORMALROUGHNESS,
		ALBEDO,
		SPECULAR,
		REFLECTANCE,
		MASKS,
		MASKS2
	};

	for (uint i = 2; i < 8; i++) {
		renderTargets[i] = targets[i];                                             // We must use unused targets to be indexable
		setRenderTargetMode[i] = RE::BSGraphics::SetRenderTargetMode::SRTM_CLEAR;  // Dirty from last frame, this calls ClearRenderTargetView once
	}

	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again

	deferredPass = true;

	ZPrepassPasses();
}

void Deferred::DeferredPasses()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto& context = State::GetSingleton()->context;
	auto state = State::GetSingleton();
	auto viewport = RE::BSGraphics::State::GetSingleton();

	UpdateConstantBuffer();

	{
		auto buffer = deferredCB->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);

		REL::Relocation<ID3D11Buffer**> perFrame{ REL::RelocationID(524768, 411384) };
		context->CSSetConstantBuffers(12, 1, perFrame.get());
	}

	Skylighting::GetSingleton()->Bind();

	auto specular = renderer->GetRuntimeData().renderTargets[SPECULAR];
	auto albedo = renderer->GetRuntimeData().renderTargets[ALBEDO];
	auto normalRoughness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
	auto masks2 = renderer->GetRuntimeData().renderTargets[MASKS2];

	auto main = renderer->GetRuntimeData().renderTargets[forwardRenderTargets[0]];
	auto normals = renderer->GetRuntimeData().renderTargets[forwardRenderTargets[2]];
	auto snow = renderer->GetRuntimeData().renderTargets[forwardRenderTargets[3]];

	// Ambient Composite
	{
		ID3D11ShaderResourceView* srvs[2]{
			albedo.SRV,
			normalRoughness.SRV
		};

		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11UnorderedAccessView* uavs[1]{ main.UAV };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		auto shader = GetComputeAmbientComposite();
		context->CSSetShader(shader, nullptr, 0);

		float resolutionX = state->screenWidth * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
		float resolutionY = state->screenHeight * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

		uint32_t dispatchX = (uint32_t)std::ceil(resolutionX / 8.0f);
		uint32_t dispatchY = (uint32_t)std::ceil(resolutionY / 8.0f);

		context->Dispatch(dispatchX, dispatchY, 1);
	}

	// Deferred Composite
	{
		ID3D11ShaderResourceView* srvs[4]{
			specular.SRV,
			albedo.SRV,
			normalRoughness.SRV,
			masks2.SRV
		};

		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11UnorderedAccessView* uavs[3]{ main.UAV, normals.UAV, snow.UAV };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		auto shader = GetComputeMainComposite();
		context->CSSetShader(shader, nullptr, 0);

		float resolutionX = state->screenWidth * viewport->GetRuntimeData().dynamicResolutionCurrentWidthScale;
		float resolutionY = state->screenHeight * viewport->GetRuntimeData().dynamicResolutionCurrentHeightScale;

		uint32_t dispatchX = (uint32_t)std::ceil(resolutionX / 8.0f);
		uint32_t dispatchY = (uint32_t)std::ceil(resolutionY / 8.0f);

		context->Dispatch(dispatchX, dispatchY, 1);
	}

	// Clear
	{
		ID3D11ShaderResourceView* views[4]{ nullptr, nullptr, nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[3]{ nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		ID3D11Buffer* buffer = nullptr;
		context->CSSetConstantBuffers(0, 1, &buffer);

		context->CSSetShader(nullptr, nullptr, 0);
	}
}

void Deferred::EndDeferred()
{
	if (!inWorld)
		return;

	inWorld = false;

	auto& shaderCache = SIE::ShaderCache::Instance();

	if (!shaderCache.IsEnabled())
		return;

	auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
	GET_INSTANCE_MEMBER(renderTargets, shadowState)
	GET_INSTANCE_MEMBER(stateUpdateFlags, shadowState)

	// Do not render to our targets past this point
	for (uint i = 0; i < 4; i++) {
		renderTargets[i] = forwardRenderTargets[i];
	}

	for (uint i = 4; i < 8; i++) {
		shadowState->GetRuntimeData().renderTargets[i] = RE::RENDER_TARGET::kNONE;
	}

	auto context = RE::BSGraphics::Renderer::GetSingleton()->GetRuntimeData().context;
	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	DeferredPasses();  // Perform deferred passes and composite forward buffers

	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again

	deferredPass = false;
}

void Deferred::OverrideBlendStates()
{
	static bool setup = false;
	if (!setup) {
		auto& device = State::GetSingleton()->device;

		static BlendStates* blendStates = (BlendStates*)REL::RelocationID(524749, 411364).address();

		{
			forwardBlendStates[0] = blendStates->a[0][0][1][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[0]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[0]));
		}

		{
			forwardBlendStates[1] = blendStates->a[0][0][10][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[1]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[1]));
		}

		{
			forwardBlendStates[2] = blendStates->a[1][0][1][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[2]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[2]));
		}

		{
			forwardBlendStates[3] = blendStates->a[1][0][11][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[3]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[3]));
		}

		{
			forwardBlendStates[4] = blendStates->a[2][0][1][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[4]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[4]));
		}

		{
			forwardBlendStates[5] = blendStates->a[2][0][11][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[5]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[5]));
		}

		{
			forwardBlendStates[6] = blendStates->a[3][0][11][0];

			D3D11_BLEND_DESC blendDesc;
			forwardBlendStates[6]->GetDesc(&blendDesc);

			blendDesc.IndependentBlendEnable = false;

			DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[6]));
		}
		setup = true;
	}

	static BlendStates* blendStates = (BlendStates*)REL::RelocationID(524749, 411364).address();

	// Set modified blend states
	blendStates->a[0][0][1][0] = deferredBlendStates[0];
	blendStates->a[0][0][10][0] = deferredBlendStates[1];
	blendStates->a[1][0][1][0] = deferredBlendStates[2];
	blendStates->a[1][0][11][0] = deferredBlendStates[3];
	blendStates->a[2][0][1][0] = deferredBlendStates[4];
	blendStates->a[2][0][11][0] = deferredBlendStates[5];
	blendStates->a[3][0][11][0] = deferredBlendStates[6];

	auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
	GET_INSTANCE_MEMBER(stateUpdateFlags, shadowState)

	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
}

void Deferred::ResetBlendStates()
{
	static BlendStates* blendStates = (BlendStates*)REL::RelocationID(524749, 411364).address();

	// Restore modified blend states
	blendStates->a[0][0][1][0] = forwardBlendStates[0];
	blendStates->a[0][0][10][0] = forwardBlendStates[1];
	blendStates->a[1][0][1][0] = forwardBlendStates[2];
	blendStates->a[1][0][11][0] = forwardBlendStates[3];
	blendStates->a[2][0][1][0] = forwardBlendStates[4];
	blendStates->a[2][0][11][0] = forwardBlendStates[5];
	blendStates->a[3][0][11][0] = forwardBlendStates[6];

	auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
	GET_INSTANCE_MEMBER(stateUpdateFlags, shadowState)

	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
}

void Deferred::ClearShaderCache()
{
	if (ambientCompositeCS) {
		ambientCompositeCS->Release();
		ambientCompositeCS = nullptr;
	}
	if (mainCompositeCS) {
		mainCompositeCS->Release();
		mainCompositeCS = nullptr;
	}
}

ID3D11ComputeShader* Deferred::GetComputeAmbientComposite()
{
	if (!ambientCompositeCS) {
		logger::debug("Compiling AmbientCompositeCS");
		ambientCompositeCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\AmbientCompositeCS.hlsl", {}, "cs_5_0");
	}
	return ambientCompositeCS;
}

ID3D11ComputeShader* Deferred::GetComputeMainComposite()
{
	if (!mainCompositeCS) {
		logger::debug("Compiling DeferredCompositeCS");
		mainCompositeCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\DeferredCompositeCS.hlsl", {}, "cs_5_0");
	}
	return mainCompositeCS;
}