#include "VolumetricShadows.h"

#include "Deferred.h"
#include "State.h"
#include "Utils/D3D.h"

void VolumetricShadows::SetupResources()
{
	auto device = globals::d3d::device;

	{
		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &linearSampler));
		Util::SetResourceName(linearSampler, "VolumetricShadows::LinearSampler");
	}

	{
		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &comparisonSampler));
		Util::SetResourceName(comparisonSampler, "VolumetricShadows::ComparisonSampler");
	}

	configCB = new ConstantBuffer(ConstantBufferDesc<VolumetricShadowsCB>(), "VolumetricShadows::ConfigCB");

	CompileShaders();
}

void VolumetricShadows::CompileShaders()
{
	std::vector<std::pair<const char*, const char*>> defines;
	if (REL::Module::IsVR())
		defines.push_back({ "VR", nullptr });
	buildShadowFroxelCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\VolumetricShadows\\BuildShadowFroxelCS.hlsl", defines, "cs_5_0"));
}

void VolumetricShadows::ClearShaderCache()
{
	if (buildShadowFroxelCS) {
		buildShadowFroxelCS->Release();
		buildShadowFroxelCS = nullptr;
	}
	CompileShaders();
}

void VolumetricShadows::CreateFroxelResources()
{
	auto device = globals::d3d::device;

	froxelWidth = kFroxelGridWidth * (REL::Module::IsVR() ? 2u : 1u);
	froxelHeight = kFroxelGridHeight;
	froxelDepth = kFroxelGridDepth;

	D3D11_TEXTURE3D_DESC texDesc{};
	texDesc.Width = froxelWidth;
	texDesc.Height = froxelHeight;
	texDesc.Depth = froxelDepth;
	texDesc.MipLevels = 1;
	texDesc.Format = DXGI_FORMAT_R8_UNORM;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	DX::ThrowIfFailed(device->CreateTexture3D(&texDesc, nullptr, &shadowFroxelTexture));
	Util::SetResourceName(shadowFroxelTexture, "VolumetricShadows::ShadowFroxel");

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = texDesc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
	srvDesc.Texture3D.MipLevels = 1;
	DX::ThrowIfFailed(device->CreateShaderResourceView(shadowFroxelTexture, &srvDesc, &shadowFroxelSRV));
	Util::SetResourceName(shadowFroxelSRV, "VolumetricShadows::ShadowFroxel SRV");

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = texDesc.Format;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
	uavDesc.Texture3D.WSize = froxelDepth;
	DX::ThrowIfFailed(device->CreateUnorderedAccessView(shadowFroxelTexture, &uavDesc, &shadowFroxelUAV));
	Util::SetResourceName(shadowFroxelUAV, "VolumetricShadows::ShadowFroxel UAV");
}

void VolumetricShadows::BuildShadowFroxel()
{
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "VolumetricShadows::BuildShadowFroxel");

	auto context = globals::d3d::context;

	if (!globals::state->HasDirectionalShadows()) {
		SetSharedShadowMapSRV(context, nullptr);
		return;
	}

	// Capture Skyrim's bound directional shadow cascade array (Texture2DArray<float>).
	context->PSGetShaderResources(4, 1, &shadowView);
	if (!shadowView) {
		SetSharedShadowMapSRV(context, nullptr);
		return;
	}

	if (!shadowFroxelTexture)
		CreateFroxelResources();

	if (!buildShadowFroxelCS) {
		shadowView->Release();
		shadowView = nullptr;
		SetSharedShadowMapSRV(context, nullptr);
		return;
	}

	// Mirror Deferred::CopyShadowLightData here so the t98 SBO contains current-frame cascade
	// matrices before our build dispatch reads from it. Deferred will refresh it again during
	// EarlyPrepasses; calling twice is cheap and avoids ordering coupling.
	globals::deferred->CopyShadowLightData();

	// Configure the build pass.
	VolumetricShadowsCB cbData{};
	cbData.GridSize[0] = froxelWidth;
	cbData.GridSize[1] = froxelHeight;
	cbData.GridSize[2] = froxelDepth;
	cbData.HasShadows = 1;
	cbData.NearZ = 16.0f;
	cbData.FarZ = 1.0f;  // The shader reads the actual far split distance from the SBO; this is a fallback.
	cbData.ShadowBias = 0.0001f;
	configCB->Update(cbData);

	ID3D11Buffer* perFrameCB = *globals::game::perFrame.get();
	ID3D11Buffer* vrPerFrameCB = nullptr;
	if (REL::Module::IsVR()) {
		static REL::Relocation<ID3D11Buffer**> VRValues{ REL::Offset(0x3180688) };
		vrPerFrameCB = *VRValues.get();
	}

	ID3D11Buffer* sharedDataBuf = globals::state->sharedDataCB->CB();
	ID3D11Buffer* configBuf = configCB->CB();

	context->CSSetConstantBuffers(1, 1, &configBuf);
	context->CSSetConstantBuffers(5, 1, &sharedDataBuf);
	context->CSSetConstantBuffers(12, 1, &perFrameCB);
	if (vrPerFrameCB)
		context->CSSetConstantBuffers(13, 1, &vrPerFrameCB);

	ID3D11ShaderResourceView* directionalShadowLightsSRV = globals::deferred->directionalShadowLights->srv.get();
	ID3D11ShaderResourceView* csSrvs[2]{ shadowView, directionalShadowLightsSRV };
	context->CSSetShaderResources(0, 1, &csSrvs[0]);
	context->CSSetShaderResources(98, 1, &csSrvs[1]);

	ID3D11SamplerState* samplers[2]{ comparisonSampler, linearSampler };
	context->CSSetSamplers(0, 2, samplers);

	ID3D11UnorderedAccessView* uavs[1]{ shadowFroxelUAV };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	context->CSSetShader(buildShadowFroxelCS, nullptr, 0);

	const uint32_t groupX = (froxelWidth + 7u) / 8u;
	const uint32_t groupY = (froxelHeight + 7u) / 8u;
	const uint32_t groupZ = (froxelDepth + 3u) / 4u;
	context->Dispatch(groupX, groupY, groupZ);

	// Cleanup CS bindings.
	ID3D11Buffer* nullCBs[1] = { nullptr };
	ID3D11ShaderResourceView* nullSRVs[1] = { nullptr };
	ID3D11SamplerState* nullSamplers[2] = { nullptr, nullptr };
	ID3D11UnorderedAccessView* nullUAVs[1] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 1, nullUAVs, nullptr);
	context->CSSetShaderResources(0, 1, nullSRVs);
	context->CSSetShaderResources(98, 1, nullSRVs);
	context->CSSetSamplers(0, 2, nullSamplers);
	context->CSSetConstantBuffers(1, 1, nullCBs);
	context->CSSetShader(nullptr, nullptr, 0);

	SetSharedShadowMapSRV(context, shadowFroxelSRV);

	shadowView->Release();
	shadowView = nullptr;
}

void VolumetricShadows::SetSharedShadowMapSRV(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_srv)
{
	a_context->PSSetShaderResources(kSharedShadowMapShaderSlot, 1, &a_srv);
}

void VolumetricShadows::DrawSettings()
{
	ImGui::TextWrapped(
		"Builds a %ux%ux%u view-space froxel grid (per eye) of PCF-filtered directional shadow visibility.\n"
		"Consumers sample the grid via the shared shadow map texture at slot t%u.",
		kFroxelGridWidth,
		kFroxelGridHeight,
		kFroxelGridDepth,
		kSharedShadowMapShaderSlot);

	if (shadowFroxelTexture) {
		ImGui::SeparatorText("Resource");
		ImGui::Text("Shadow Froxel: %ux%ux%u (R8_UNORM)", froxelWidth, froxelHeight, froxelDepth);
	}
}

void VolumetricShadows::LoadSettings(json&)
{
	// No settings currently
}

void VolumetricShadows::SaveSettings(json&)
{
	// No settings currently
}

void VolumetricShadows::RestoreDefaultSettings()
{
	// No settings currently
}

struct CreateDepthStencil_VolumetricLighting
{
	static void thunk(RE::BSGraphics::Renderer* This, uint32_t a_target, RE::BSGraphics::DepthStencilTargetProperties* a_properties)
	{
		RE::BSGraphics::DepthStencilTargetProperties properties = *a_properties;
		a_properties->height = 1024;
		a_properties->width = 1024;
		func(This, a_target, &properties);
	}
	static inline REL::Relocation<decltype(thunk)> func;
};

void VolumetricShadows::PostPostLoad()
{
	stl::write_thunk_call<CreateDepthStencil_VolumetricLighting>(REL::RelocationID(100458, 107175).address() + REL::Relocate(0x9DC, 0x9DC, 0xC60));
}

bool VolumetricShadows::HasShaderDefine(RE::BSShader::Type)
{
	return true;
}
