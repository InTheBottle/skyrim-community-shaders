#include "Bindings.h"

#define DEFINE_VALUE(a_value) \
	auto& a_value = !REL::Module::IsVR() ? state->GetRuntimeData().a_value : state->GetVRRuntimeData().a_value;

void Bindings::DepthStencilStateSetDepthMode(RE::BSGraphics::DepthStencilDepthMode a_mode)
{
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
	DEFINE_VALUE(depthStencilDepthMode)
	DEFINE_VALUE(depthStencilDepthModePrevious)
	DEFINE_VALUE(stateUpdateFlags)

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
	DEFINE_VALUE(alphaBlendMode)
	DEFINE_VALUE(stateUpdateFlags)

	if (alphaBlendMode != a_mode) {
		alphaBlendMode = a_mode;
		stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
	}
}

void Bindings::AlphaBlendStateSetAlphaToCoverage(uint32_t a_value)
{
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
	DEFINE_VALUE(alphaBlendAlphaToCoverage)
	DEFINE_VALUE(stateUpdateFlags)

	if (alphaBlendAlphaToCoverage != a_value) {
		alphaBlendAlphaToCoverage = a_value;
		stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
	}
}

void Bindings::AlphaBlendStateSetWriteMode(uint32_t a_value)
{
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
	DEFINE_VALUE(alphaBlendWriteMode)
	DEFINE_VALUE(stateUpdateFlags)

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

void Bindings::SetupResources()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto device = renderer->GetRuntimeData().forwarder;

	{
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		auto& indirect = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kINDIRECT];

		D3D11_TEXTURE2D_DESC texDesc{};
		main.texture->GetDesc(&texDesc);
		DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, &indirect.texture));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		main.SRV->GetDesc(&srvDesc);
		DX::ThrowIfFailed(device->CreateShaderResourceView(indirect.texture, &srvDesc, &indirect.SRV));

		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		main.RTV->GetDesc(&rtvDesc);
		DX::ThrowIfFailed(device->CreateRenderTargetView(indirect.texture, &rtvDesc, &indirect.RTV));

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		main.UAV->GetDesc(&uavDesc);
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(indirect.texture, &uavDesc, &indirect.UAV));
	}


	//{
	//	static DepthStates* depthStates = (DepthStates*)REL::RelocationID(524747, 411362).address();
	//	static BlendStates* blendStates = (BlendStates*)REL::RelocationID(524749, 411364).address();

	//	auto mainBlendState = blendStates->a[0][0][0][1]; // alphaBlendMode, alphaToCoverage, alphaBlendWriteMode, alphaBlendModeExtra
	//	D3D11_BLEND_DESC blendDesc;
	//	mainBlendState->GetDesc(&blendDesc);

	//	for (int i = 3; i < 8; i++)
	//	{
	//		blendDesc.RenderTarget[i] = blendDesc.RenderTarget[0];
	//	}

	//	ID3D11BlendState* newBlendState;
	//	DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &newBlendState));

	//	mainBlendState->Release();
	//	blendStates->a[0][0][0][1] = newBlendState;
	//}
}

void Bindings::Reset()
{
}

void Bindings::StartDeferred()
{
	// Also requires modifying blend states, this one assumes the snow shader in the 4th slot so it's fine
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();

	state->GetRuntimeData().renderTargets[3] = RE::RENDER_TARGETS::kINDIRECT; // We must use unused targets to be indexable
	state->GetRuntimeData().setRenderTargetMode[3] = RE::BSGraphics::SetRenderTargetMode::SRTM_CLEAR; // Dirty from last frame, this calls ClearRenderTargetView once
	state->GetRuntimeData().stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET); // Run OMSetRenderTargets again
}

void Bindings::EndDeferred()
{
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
	// Change from uint32_t in clib to RE::RENDER_TARGETS::RENDER_TARGET 
	state->GetRuntimeData().renderTargets[3] = RE::RENDER_TARGETS::kNONE;  // Do not render to our target past this point
	state->GetRuntimeData().stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again
}
