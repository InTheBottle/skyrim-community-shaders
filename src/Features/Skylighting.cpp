#include "Skylighting.h"
#include <Util.h>

#include "State.h"
#include <ShaderCache.h>


void Skylighting::DrawSettings()
{
	
	ImGui::SliderFloat("Distance Mult", &distanceMult, 1.0f, 100.0f);
	ImGui::SliderFloat("Far Mult", &farMult, 1.0f, 100.0f);
}

void Skylighting::SetupResources()
{
	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(PerPass);
		sbDesc.ByteWidth = sizeof(PerPass);
		perPass = std::make_unique<Buffer>(sbDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = 1;
		perPass->CreateSRV(srvDesc);
	}

	{
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();

		auto precipation = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];
		D3D11_TEXTURE2D_DESC texDesc{};
		precipation.texture->GetDesc(&texDesc);
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R32_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.SampleDesc.Quality = 0;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.CPUAccessFlags = 0;
		texDesc.MiscFlags = 0;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		precipation.depthSRV->GetDesc(&srvDesc);
		srvDesc.Format = texDesc.Format;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;

		precipOcclusionTex = std::make_unique<Texture2D>(texDesc);
		precipOcclusionTex->CreateSRV(srvDesc);
		precipOcclusionTex->CreateUAV(uavDesc);

		precipOcclusionTempTex = std::make_unique<Texture2D>(texDesc);
		precipOcclusionTempTex->CreateSRV(srvDesc);
		precipOcclusionTempTex->CreateUAV(uavDesc);
	}
}

void Skylighting::Reset()
{

}

void Skylighting::RestoreDefaultSettings()
{
}

void Skylighting::Load(json& o_json)
{
	Feature::Load(o_json);
}

void Skylighting::Save(json&)
{
}

void Skylighting::Draw(const RE::BSShader*, const uint32_t)
{
}

void Skylighting::ClearShaderCache()
{
	if (horizontalSSBlur) {
		horizontalSSBlur->Release();
		horizontalSSBlur = nullptr;
	}
	if (verticalSSBlur) {
		verticalSSBlur->Release();
		verticalSSBlur = nullptr;
	}
}

ID3D11ComputeShader* Skylighting::GetComputeShaderHorizontalBlur()
{
	if (!horizontalSSBlur) {
		logger::debug("Compiling horizontalBlur");
		horizontalSSBlur = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\Skylighting\\Blur.hlsl", { { "HORIZONTAL", "" } }, "cs_5_0");
	}
	return horizontalSSBlur;
}

ID3D11ComputeShader* Skylighting::GetComputeShaderVerticalBlur()
{
	if (!verticalSSBlur) {
		logger::debug("Compiling verticalBlur");
		verticalSSBlur = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\Skylighting\\Blur.hlsl", {}, "cs_5_0");
	}
	return verticalSSBlur;
}

void Skylighting::Blur()
{
	auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	auto context = renderer->GetRuntimeData().context;

	{
		auto precipation = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];

		{
			ID3D11ShaderResourceView* view = precipation.depthSRV;
			context->CSSetShaderResources(0, 1, &view);

			ID3D11UnorderedAccessView* uav = precipOcclusionTempTex->uav.get();
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

			UINT numGroupsX = (UINT)ceilf((float)precipOcclusionTex->desc.Width / 256.0f);

			auto shader = GetComputeShaderHorizontalBlur();
			context->CSSetShader(shader, nullptr, 0);

			context->Dispatch(numGroupsX, precipOcclusionTex->desc.Height, 1);
		}

		// Unbind the input texture from the CS for good housekeeping.
		ID3D11ShaderResourceView* nullSRV[1] = { 0 };
		context->CSSetShaderResources(0, 1, nullSRV);
		context->PSSetShaderResources(70, 1, nullSRV);

		// Unbind output from compute shader (we are going to use this output as an input in the next pass,
		// and a resource cannot be both an output and input at the same time.
		ID3D11UnorderedAccessView* nullUAV[1] = { 0 };
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, 0);
	
		{
			ID3D11ShaderResourceView* view = precipOcclusionTempTex->srv.get();
			context->CSSetShaderResources(0, 1, &view);

			ID3D11UnorderedAccessView* uav = precipOcclusionTex->uav.get();
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

			auto shader = GetComputeShaderVerticalBlur();
			context->CSSetShader(shader, nullptr, 0);

			UINT numGroupsY = (UINT)ceilf((float)precipOcclusionTex->desc.Height / 256.0f);

			context->Dispatch(precipOcclusionTex->desc.Width, numGroupsY, 1);
		}

		context->CSSetShaderResources(0, 1, nullSRV);
		context->CSSetUnorderedAccessViews(0, 1, nullUAV, 0);

		// Disable compute shader.
		context->CSSetShader(0, 0, 0);
	}


	for (int i = 0; i < 10; i++) {
		{
			{
				{
					ID3D11ShaderResourceView* view = precipOcclusionTex->srv.get();
					context->CSSetShaderResources(0, 1, &view);

					ID3D11UnorderedAccessView* uav = precipOcclusionTempTex->uav.get();
					context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

					UINT numGroupsX = (UINT)ceilf((float)precipOcclusionTex->desc.Width / 256.0f);

					auto shader = GetComputeShaderHorizontalBlur();
					context->CSSetShader(shader, nullptr, 0);

					context->Dispatch(numGroupsX, precipOcclusionTex->desc.Height, 1);
				}

				// Unbind the input texture from the CS for good housekeeping.
				ID3D11ShaderResourceView* nullSRV[1] = { 0 };
				context->CSSetShaderResources(0, 1, nullSRV);
				context->PSSetShaderResources(70, 1, nullSRV);

				// Unbind output from compute shader (we are going to use this output as an input in the next pass,
				// and a resource cannot be both an output and input at the same time.
				ID3D11UnorderedAccessView* nullUAV[1] = { 0 };
				context->CSSetUnorderedAccessViews(0, 1, nullUAV, 0);

				{
					ID3D11ShaderResourceView* view = precipOcclusionTempTex->srv.get();
					context->CSSetShaderResources(0, 1, &view);

					ID3D11UnorderedAccessView* uav = precipOcclusionTex->uav.get();
					context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

					auto shader = GetComputeShaderVerticalBlur();
					context->CSSetShader(shader, nullptr, 0);

					UINT numGroupsY = (UINT)ceilf((float)precipOcclusionTex->desc.Height / 256.0f);

					context->Dispatch(precipOcclusionTex->desc.Width, numGroupsY, 1);
				}

				context->CSSetShaderResources(0, 1, nullSRV);
				context->CSSetUnorderedAccessViews(0, 1, nullUAV, 0);

				// Disable compute shader.
				context->CSSetShader(0, 0, 0);
			}
		}
	}
}

void Skylighting::BlurAndBind()
{
	 auto renderer = RE::BSGraphics::Renderer::GetSingleton();
	 auto context = renderer->GetRuntimeData().context;

	 {
		PerPass data{};
		data.PrecipProj = precipProj;
		data.Params = { 250 * distanceMult, 0, 0, 0 };

		D3D11_MAPPED_SUBRESOURCE mapped;
		DX::ThrowIfFailed(context->Map(perPass->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
		size_t bytes = sizeof(PerPass);
		memcpy_s(mapped.pData, bytes, &data, bytes);
		context->Unmap(perPass->resource.get(), 0);
	 }

	 ID3D11ShaderResourceView* views[2]{};
	 views[0] = perPass->srv.get();
	 views[1] = precipOcclusionTex->srv.get();
	 context->PSSetShaderResources(69, 2, views);
}
