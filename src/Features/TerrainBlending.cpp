#include "TerrainBlending.h"

#include "Bindings.h"
#include "State.h"
#include <Util.h>

void TerrainBlending::DrawSettings()
{
}

void TerrainBlending::Draw(const RE::BSShader*, const uint32_t)
{
}

ID3D11VertexShader* TerrainBlending::GetTerrainVertexShader()
{
	if (!terrainVertexShader) {
		logger::debug("Compiling Utility.hlsl");
		terrainVertexShader = (ID3D11VertexShader*)Util::CompileShader(L"Data\\Shaders\\Utility.hlsl", { { "RENDER_DEPTH", "" } }, "vs_5_0");
	}
	return terrainVertexShader;
}

ID3D11VertexShader* TerrainBlending::GetTerrainOffsetVertexShader()
{
	if (!terrainOffsetVertexShader) {
		logger::debug("Compiling Utility.hlsl");
		terrainOffsetVertexShader = (ID3D11VertexShader*)Util::CompileShader(L"Data\\Shaders\\Utility.hlsl", { { "RENDER_DEPTH", "" }, { "OFFSET_DEPTH", "" } }, "vs_5_0");
	}
	return terrainOffsetVertexShader;
}

ID3D11ComputeShader* TerrainBlending::GetDepthBlendShader()
{
	if (!depthBlendShader) {
		logger::debug("Compiling DepthBlend.hlsl");
		depthBlendShader = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\TerrainBlending\\DepthBlend.hlsl", {}, "cs_5_0");
	}
	return depthBlendShader;
}

void TerrainBlending::SetupResources()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto device = renderer->GetRuntimeData().forwarder;

	{
		auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		
		D3D11_TEXTURE2D_DESC texDesc;
		mainDepth.texture->GetDesc(&texDesc);
		DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, NULL, &terrainDepth.texture));

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		mainDepth.depthSRV->GetDesc(&srvDesc);
		DX::ThrowIfFailed(device->CreateShaderResourceView(terrainDepth.texture, &srvDesc, &terrainDepth.depthSRV));

		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc;
		mainDepth.views[0]->GetDesc(&dsvDesc);
		DX::ThrowIfFailed(device->CreateDepthStencilView(terrainDepth.texture, &dsvDesc, &terrainDepth.views[0]));
	}

	{
		auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		main.texture->GetDesc(&texDesc);
		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		terrainOffsetTexture = new Texture2D(texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		main.SRV->GetDesc(&srvDesc);
		srvDesc.Format = texDesc.Format;
		terrainOffsetTexture->CreateSRV(srvDesc);

		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		main.RTV->GetDesc(&rtvDesc);
		rtvDesc.Format = texDesc.Format;
		terrainOffsetTexture->CreateRTV(rtvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		main.UAV->GetDesc(&uavDesc);
		uavDesc.Format = texDesc.Format;
		terrainOffsetTexture->CreateUAV(uavDesc);

		auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		depthSRVBackup = mainDepth.depthSRV;	
		mainDepth.depthSRV = terrainOffsetTexture->srv.get();

		auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		prepassSRVBackup = zPrepassCopy.depthSRV;
		zPrepassCopy.depthSRV = terrainOffsetTexture->srv.get();
	}

	{
		D3D11_DEPTH_STENCIL_DESC depthStencilDesc{};
		depthStencilDesc.DepthEnable = true;
		depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
		depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		depthStencilDesc.StencilEnable = false;
		DX::ThrowIfFailed(device->CreateDepthStencilState(&depthStencilDesc, &terrainDepthStencilState));
	}
}

void TerrainBlending::Reset()
{
}

void TerrainBlending::Load(json& o_json)
{
	Feature::Load(o_json);
}

void TerrainBlending::Save(json&)
{
}

void TerrainBlending::RestoreDefaultSettings()
{
}

void TerrainBlending::PostPostLoad()
{
	Hooks::Install();
}

struct DepthStates
{
	ID3D11DepthStencilState* a[6][40];
};

struct BlendStates
{
	ID3D11BlendState* a[7][2][13][2];
};

#define GET_INSTANCE_MEMBER(a_value, a_source) \
	auto& a_value = !REL::Module::IsVR() ? a_source->GetRuntimeData().a_value : a_source->GetVRRuntimeData().a_value;

void TerrainBlending::TerrainShaderHacks()
{
	if (renderTerrainDepth) {
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto context = renderer->GetRuntimeData().context;
		if (renderAltTerrain)
		{
			auto dsv = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].views[0];
			context->OMSetRenderTargets(0, nullptr, dsv);
			context->VSSetShader(GetTerrainOffsetVertexShader(), NULL, NULL);
		} else {
			auto dsv = terrainDepth.views[0];
			context->OMSetRenderTargets(0, nullptr, dsv);
			context->VSSetShader(GetTerrainVertexShader(), NULL, NULL);
		}
		renderAltTerrain = !renderAltTerrain;
	} else if (renderTerrainWorld) {
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		auto context = renderer->GetRuntimeData().context;

		auto state = RE::BSGraphics::RendererShadowState::GetSingleton();

		static BlendStates* blendStates = (BlendStates*)REL::RelocationID(524749, 411364).address();

		GET_INSTANCE_MEMBER(alphaBlendAlphaToCoverage, state)
		GET_INSTANCE_MEMBER(alphaBlendModeExtra, state)
		GET_INSTANCE_MEMBER(alphaBlendWriteMode, state)

		// Enable alpha testing
		context->OMSetBlendState(blendStates->a[1][alphaBlendAlphaToCoverage][alphaBlendWriteMode][alphaBlendModeExtra], nullptr, 0xFFFFFFFF);
		
		// Enable rendering for depth below the surface
		context->OMSetDepthStencilState(terrainDepthStencilState, 0xFF);

		// Used to get the distance of the surface to the lowest depth
		auto view = prepassSRVBackup;
		context->PSSetShaderResources(35, 1, &view);
	}
}

void TerrainBlending::OverrideTerrainDepth()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	auto rtv = terrainOffsetTexture->rtv.get();
	FLOAT clearColor[4]{ 1, 0, 0, 0 };
	context->ClearRenderTargetView(rtv, clearColor);
	
	auto dsv = terrainDepth.views[0];
	context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0u);
}

void TerrainBlending::ResetTerrainDepth()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	auto dsv = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].views[0];
	context->OMSetRenderTargets(0, NULL, dsv);
	context->PSSetShader(NULL, NULL, NULL);;
}

void TerrainBlending::BlendPrepassDepths()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	ID3D11RenderTargetView* nullViews[8] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
	ID3D11DepthStencilView* nullDsv = nullptr;
	context->OMSetRenderTargets(8, nullViews, nullDsv);

	{
		ID3D11ShaderResourceView* views[2] = { depthSRVBackup, terrainDepth.depthSRV };
		context->CSSetShaderResources(0, 2, views);

		ID3D11UnorderedAccessView* uav = terrainOffsetTexture->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

		context->CSSetShader(GetDepthBlendShader(), nullptr, 0);

		context->Dispatch((uint32_t)std::ceil((float)terrainOffsetTexture->desc.Width / 32.0f), (uint32_t)std::ceil((float)terrainOffsetTexture->desc.Height / 32.0f), 1);
	}

	ID3D11ShaderResourceView* views[2] = { nullptr, nullptr };
	context->CSSetShaderResources(0, 2, views);

	ID3D11UnorderedAccessView* uav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	ID3D11ComputeShader* shader = nullptr;
	context->CSSetShader(shader, nullptr, 0);

	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
	state->GetRuntimeData().stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

void TerrainBlending::ResetTerrainWorld()
{
	auto state = RE::BSGraphics::RendererShadowState::GetSingleton();
	state->GetRuntimeData().stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
	state->GetRuntimeData().stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
}

void TerrainBlending::ClearShaderCache()
{
	if (terrainVertexShader) {
		terrainVertexShader->Release();
		terrainVertexShader = nullptr;
	}
	if (terrainOffsetVertexShader) {
		terrainOffsetVertexShader->Release();
		terrainOffsetVertexShader = nullptr;
	}
	if (depthBlendShader) {
		depthBlendShader->Release();
		depthBlendShader = nullptr;
	}
}
