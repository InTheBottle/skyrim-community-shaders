#include "DistantShadowMap.h"

#include "Features/Effects11/D3D11StateBackup.h"
#include "State.h"
#include "Utils/D3D.h"

#include <d3dcompiler.h>

namespace
{
	constexpr const wchar_t* DepthShaderPath = L"Data\\Shaders\\ScreenSpaceShadows\\DistantShadowMapDepth.hlsl";

	uint64_t ReadVertexDesc(const RE::BSGraphics::VertexDesc& a_desc)
	{
		return *reinterpret_cast<const uint64_t*>(&a_desc);
	}

	uint GetVertexFlags(uint64_t a_desc)
	{
		return static_cast<uint>((a_desc >> 44) & 0xFFFF);
	}

	uint GetVertexStride(uint64_t a_desc)
	{
		return static_cast<uint>(a_desc & 0xF) * 4;
	}

	uint GetTexCoordOffset(uint64_t a_desc)
	{
		return static_cast<uint>((a_desc >> (4 * RE::BSGraphics::Vertex::VA_TEXCOORD0 + 2)) & 0x3C);
	}

	ID3D11ShaderResourceView* GetTextureSRV(RE::NiSourceTexture* a_texture)
	{
		if (!a_texture || !a_texture->rendererTexture)
			return nullptr;
		return reinterpret_cast<ID3D11ShaderResourceView*>(a_texture->rendererTexture->resourceView);
	}

	float GetAlphaTestRef(RE::BSGeometry* a_geometry, float a_fallback)
	{
		auto* alphaProperty = static_cast<RE::NiAlphaProperty*>(a_geometry->GetGeometryRuntimeData().alphaProperty.get());
		if (!alphaProperty || !alphaProperty->GetAlphaTesting())
			return a_fallback;
		return std::max<float>(alphaProperty->alphaThreshold, 1.0f) / 255.0f;
	}

	winrt::com_ptr<ID3DBlob> CompileDepthShader(const char* a_entry, const char* a_target)
	{
		winrt::com_ptr<ID3DBlob> blob;
		winrt::com_ptr<ID3DBlob> errors;
		if (FAILED(D3DCompileFromFile(DepthShaderPath, nullptr, nullptr, a_entry, a_target, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.put(), errors.put()))) {
			logger::error("[SSS] Distant shadow map shader {} failed to compile: {}", a_entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "file not found");
			return nullptr;
		}
		return blob;
	}

	bool IsMovableReference(RE::BSGeometry* a_geometry)
	{
		for (auto* parent = a_geometry->parent; parent; parent = parent->parent) {
			auto* fadeNode = parent->AsFadeNode();
			if (!fadeNode)
				continue;

			static const RE::BSFixedString bsxKey{ "BSX" };
			auto* extraData = fadeNode->GetExtraData(bsxKey);
			if (!extraData)
				return false;

			using enum RE::BSXFlags::Flag;
			const auto value = static_cast<int32_t>(static_cast<RE::BSXFlags*>(extraData)->value);
			constexpr int32_t movable = static_cast<int32_t>(kRagdoll) | static_cast<int32_t>(kEditorMarker) | static_cast<int32_t>(kDynamic) | static_cast<int32_t>(kNeedsTransformUpdate);
			return (value & movable) != 0;
		}
		return false;
	}

	bool Overlaps(const RE::NiBound& a_bound, const float3& a_center, const float3& a_right, const float3& a_up, const float3& a_forward, float a_halfExtent, float a_depthFar)
	{
		const float3 offset{ a_bound.center.x - a_center.x, a_bound.center.y - a_center.y, a_bound.center.z - a_center.z };
		const float reach = a_halfExtent + a_bound.radius;
		return std::abs(offset.Dot(a_right)) <= reach && std::abs(offset.Dot(a_up)) <= reach && offset.Dot(a_forward) - a_bound.radius <= a_depthFar;
	}

	bool InsideNearZone(const RE::NiBound& a_bound, const float3& a_camera, float a_nearSkipDistance, float a_shadowStretch)
	{
		const float dx = a_bound.center.x - a_camera.x;
		const float dy = a_bound.center.y - a_camera.y;
		return std::sqrt(dx * dx + dy * dy) + a_bound.radius * (1.0f + a_shadowStretch) < a_nearSkipDistance;
	}
}

bool DistantShadowMap::IsVertexFormatSupported(uint64_t a_desc)
{
	using namespace RE::BSGraphics;
	const uint flags = GetVertexFlags(a_desc);
	if (!(flags & Vertex::VF_VERTEX) || (flags & Vertex::VF_SKINNED))
		return false;
	const uint stride = GetVertexStride(a_desc);
	return stride >= ((flags & Vertex::VF_FULLPREC) ? 12u : 8u);
}

uint DistantShadowMap::GetLayoutKey(uint64_t a_desc)
{
	using namespace RE::BSGraphics;
	const uint flags = GetVertexFlags(a_desc);
	const bool fullPrecision = flags & Vertex::VF_FULLPREC;
	const bool hasTexCoord = flags & Vertex::VF_UV;
	return (fullPrecision ? 1u : 0u) | (hasTexCoord ? (GetTexCoordOffset(a_desc) << 8) : 0u);
}

bool DistantShadowMap::EnsureResources(uint a_resolution)
{
	if (texture && resolution == a_resolution)
		return true;

	auto device = globals::d3d::device;

	try {
		texture.reset();
		for (auto& dsv : sliceDSVs)
			dsv = nullptr;

		D3D11_TEXTURE2D_DESC texDesc{
			.Width = a_resolution,
			.Height = a_resolution,
			.MipLevels = 1,
			.ArraySize = CascadeCount,
			.Format = DXGI_FORMAT_R32_TYPELESS,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		texture = std::make_unique<Texture2D>(texDesc, "SSS::DistantShadowMap");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray = { .MostDetailedMip = 0, .MipLevels = 1, .FirstArraySlice = 0, .ArraySize = CascadeCount };
		texture->CreateSRV(srvDesc);

		for (uint i = 0; i < CascadeCount; i++) {
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
			dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
			dsvDesc.Texture2DArray = { .MipSlice = 0, .FirstArraySlice = i, .ArraySize = 1 };
			DX::ThrowIfFailed(device->CreateDepthStencilView(texture->resource.get(), &dsvDesc, sliceDSVs[i].put()));
			Util::SetResourceName(sliceDSVs[i].get(), "SSS::DistantShadowMap DSV %u", i);
		}

		if (!comparisonSampler) {
			D3D11_SAMPLER_DESC samplerDesc{};
			samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
			samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
			samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
			DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, comparisonSampler.put()));
			Util::SetResourceName(comparisonSampler.get(), "SSS::DistantShadowMap ComparisonSampler");

			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
			samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
			samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
			samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
			DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, diffuseSampler.put()));
			Util::SetResourceName(diffuseSampler.get(), "SSS::DistantShadowMap DiffuseSampler");
		}

		if (!rasterizerState) {
			D3D11_RASTERIZER_DESC rasterizerDesc{};
			rasterizerDesc.FillMode = D3D11_FILL_SOLID;
			rasterizerDesc.CullMode = D3D11_CULL_NONE;
			rasterizerDesc.SlopeScaledDepthBias = 3.0f;
			rasterizerDesc.DepthClipEnable = FALSE;
			DX::ThrowIfFailed(device->CreateRasterizerState(&rasterizerDesc, rasterizerState.put()));
			Util::SetResourceName(rasterizerState.get(), "SSS::DistantShadowMap RasterizerState");

			D3D11_DEPTH_STENCIL_DESC depthDesc{};
			depthDesc.DepthEnable = TRUE;
			depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
			depthDesc.DepthFunc = D3D11_COMPARISON_LESS;
			DX::ThrowIfFailed(device->CreateDepthStencilState(&depthDesc, depthStencilState.put()));
			Util::SetResourceName(depthStencilState.get(), "SSS::DistantShadowMap DepthStencilState");
		}

		if (!instanceBuffer) {
			D3D11_BUFFER_DESC bufferDesc{};
			bufferDesc.ByteWidth = MaxStaticDraws * sizeof(StaticInstance);
			bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
			bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
			bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			DX::ThrowIfFailed(device->CreateBuffer(&bufferDesc, nullptr, instanceBuffer.put()));
			Util::SetResourceName(instanceBuffer.get(), "SSS::DistantShadowMap InstanceBuffer");

			treeDrawCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<TreeDrawCB>(), "SSS::DistantShadowMap TreeDrawCB");
		}
	} catch (const std::exception& e) {
		logger::error("[SSS] Failed to create distant shadow map resources at {}x{}: {}", a_resolution, a_resolution, e.what());
		texture.reset();
		resolution = 0;
		return false;
	}

	resolution = a_resolution;
	Invalidate();
	return true;
}

bool DistantShadowMap::EnsureShaders()
{
	if (staticVS && treeVS && alphaTestPS)
		return true;
	if (shaderCompileFailed)
		return false;

	auto device = globals::d3d::device;

	staticVSBlob = CompileDepthShader("StaticVS", "vs_5_0");
	treeVSBlob = CompileDepthShader("TreeVS", "vs_5_0");
	auto psBlob = CompileDepthShader("AlphaTestPS", "ps_5_0");
	if (!staticVSBlob || !treeVSBlob || !psBlob) {
		shaderCompileFailed = true;
		return false;
	}

	if (FAILED(device->CreateVertexShader(staticVSBlob->GetBufferPointer(), staticVSBlob->GetBufferSize(), nullptr, staticVS.put())) ||
		FAILED(device->CreateVertexShader(treeVSBlob->GetBufferPointer(), treeVSBlob->GetBufferSize(), nullptr, treeVS.put())) ||
		FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, alphaTestPS.put()))) {
		logger::error("[SSS] Failed to create distant shadow map shaders");
		staticVS = nullptr;
		treeVS = nullptr;
		alphaTestPS = nullptr;
		shaderCompileFailed = true;
		return false;
	}
	Util::SetResourceName(staticVS.get(), "SSS::DistantShadowMap StaticVS");
	Util::SetResourceName(treeVS.get(), "SSS::DistantShadowMap TreeVS");
	Util::SetResourceName(alphaTestPS.get(), "SSS::DistantShadowMap AlphaTestPS");
	return true;
}

void DistantShadowMap::ClearShaderCache()
{
	staticVS = nullptr;
	treeVS = nullptr;
	alphaTestPS = nullptr;
	staticVSBlob = nullptr;
	treeVSBlob = nullptr;
	staticLayouts.clear();
	treeLayouts.clear();
	shaderCompileFailed = false;
	Invalidate();
}

ID3D11InputLayout* DistantShadowMap::GetStaticLayout(uint a_key)
{
	if (auto it = staticLayouts.find(a_key); it != staticLayouts.end())
		return it->second.get();

	const bool fullPrecision = a_key & 1;
	const uint texCoordOffset = a_key >> 8;
	const D3D11_INPUT_ELEMENT_DESC elements[] = {
		{ "POSITION", 0, fullPrecision ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, texCoordOffset, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
		{ "TEXCOORD", 2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
		{ "TEXCOORD", 3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
		{ "TEXCOORD", 4, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
		{ "TEXCOORD", 5, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 64, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
	};

	winrt::com_ptr<ID3D11InputLayout> layout;
	if (FAILED(globals::d3d::device->CreateInputLayout(elements, ARRAYSIZE(elements), staticVSBlob->GetBufferPointer(), staticVSBlob->GetBufferSize(), layout.put())))
		logger::warn("[SSS] Distant shadow map input layout {:#x} failed", a_key);
	else
		Util::SetResourceName(layout.get(), "SSS::DistantShadowMap StaticLayout %x", a_key);
	return staticLayouts.emplace(a_key, layout).first->second.get();
}

ID3D11InputLayout* DistantShadowMap::GetTreeLayout(uint a_key)
{
	if (auto it = treeLayouts.find(a_key); it != treeLayouts.end())
		return it->second.get();

	const bool fullPrecision = a_key & 1;
	const uint texCoordOffset = a_key >> 8;
	const D3D11_INPUT_ELEMENT_DESC elements[] = {
		{ "POSITION", 0, fullPrecision ? DXGI_FORMAT_R32G32B32_FLOAT : DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R16G16_FLOAT, 0, texCoordOffset, D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 4, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
	};

	winrt::com_ptr<ID3D11InputLayout> layout;
	if (FAILED(globals::d3d::device->CreateInputLayout(elements, ARRAYSIZE(elements), treeVSBlob->GetBufferPointer(), treeVSBlob->GetBufferSize(), layout.put())))
		logger::warn("[SSS] Distant shadow map tree input layout {:#x} failed", a_key);
	else
		Util::SetResourceName(layout.get(), "SSS::DistantShadowMap TreeLayout %x", a_key);
	return treeLayouts.emplace(a_key, layout).first->second.get();
}

void DistantShadowMap::Invalidate()
{
	for (auto& cascade : cascades)
		cascade.valid = false;
}

void DistantShadowMap::BuildObjectToClip(const RE::NiTransform& a_world, const CullContext& a_cull, float a_depthNear, float a_depthRange, float4 (&a_rows)[3])
{
	const auto& m = a_world.rotate.entry;
	const float3 translation{ a_world.translate.x - a_cull.center.x, a_world.translate.y - a_cull.center.y, a_world.translate.z - a_cull.center.z };

	auto row = [&](const float3& a_axis, float a_scale, float a_offset) {
		const float s = a_world.scale * a_scale;
		return float4{
			(a_axis.x * m[0][0] + a_axis.y * m[1][0] + a_axis.z * m[2][0]) * s,
			(a_axis.x * m[0][1] + a_axis.y * m[1][1] + a_axis.z * m[2][1]) * s,
			(a_axis.x * m[0][2] + a_axis.y * m[1][2] + a_axis.z * m[2][2]) * s,
			translation.Dot(a_axis) * a_scale + a_offset
		};
	};

	const float xyScale = 1.0f / a_cull.halfExtent;
	a_rows[0] = row(a_cull.right, xyScale, 0.0f);
	a_rows[1] = row(a_cull.up, xyScale, 0.0f);
	a_rows[2] = row(a_cull.forward, 1.0f / a_depthRange, a_depthNear / a_depthRange);
}

void DistantShadowMap::Collect(RE::NiAVObject* a_root, const CullContext& a_cull)
{
	traversalStack.clear();
	traversalStack.push_back(a_root);

	while (!traversalStack.empty()) {
		auto* object = traversalStack.back();
		traversalStack.pop_back();

		if (!object || object->GetFlags().any(RE::NiAVObject::Flag::kHidden))
			continue;

		const auto& bound = object->worldBound;
		if (bound.radius > 0.0f) {
			if (!Overlaps(bound, a_cull.center, a_cull.right, a_cull.up, a_cull.forward, a_cull.halfExtent, a_cull.depthFar))
				continue;
			if (InsideNearZone(bound, a_cull.cameraPosition, a_cull.nearSkipDistance, a_cull.shadowStretch))
				continue;
		}

		if (auto* node = object->AsNode()) {
			auto& children = node->GetChildren();
			if (auto* switchNode = netimmerse_cast<RE::NiSwitchNode*>(node)) {
				if (switchNode->index >= 0 && static_cast<uint32_t>(switchNode->index) < children.size())
					traversalStack.push_back(children[static_cast<std::uint16_t>(switchNode->index)].get());
				continue;
			}
			for (auto& child : children)
				if (child)
					traversalStack.push_back(child.get());
			continue;
		}

		if (auto* geometry = object->AsGeometry())
			CollectGeometry(geometry, a_cull);
	}
}

void DistantShadowMap::CollectGeometry(RE::BSGeometry* a_geometry, const CullContext& a_cull)
{
	auto& runtimeData = a_geometry->GetGeometryRuntimeData();
	auto* property = runtimeData.shaderProperty.get();
	if (!property)
		return;

	const auto* rtti = property->GetRTTI();
	if (rtti == globals::rtti::BSDistantTreeShaderPropertyRTTI.get()) {
		if (a_cull.includeTrees && a_geometry->GetType().get() == RE::BSGeometry::Type::kMultiStreamInstanceTriShape)
			CollectTreeLOD(a_geometry, a_cull);
		return;
	}
	if (rtti != globals::rtti::BSLightingShaderPropertyRTTI.get())
		return;

	using Type = RE::BSGeometry::Type;
	const auto type = a_geometry->GetType().get();
	if (type != Type::kTriShape && type != Type::kSubIndexTriShape && type != Type::kSubIndexLandTriShape && type != Type::kMeshLODTriShape)
		return;

	using enum RE::BSShaderProperty::EShaderPropertyFlag;
	const auto& flags = property->flags;
	if (flags.none(kZBufferWrite) || flags.any(kSkinned, kDecal, kDynamicDecal, kRefraction, kTempRefraction, kEyeReflect))
		return;

	const bool landscape = flags.any(kMultiTextureLandscape, kLODLandscape);
	if (landscape && !a_cull.includeTerrain)
		return;

	const bool lod = flags.any(kLODObjects, kHDLODObjects, kLODLandscape);
	if (!lod && !landscape) {
		if (a_geometry->worldBound.radius < a_cull.minCasterRadius || IsMovableReference(a_geometry))
			return;
	}

	auto* rendererData = runtimeData.rendererData;
	if (!rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
		return;

	const uint64_t desc = ReadVertexDesc(runtimeData.vertexDesc);
	if (!IsVertexFormatSupported(desc))
		return;

	const uint triangleCount = static_cast<RE::BSTriShape*>(a_geometry)->GetTrishapeRuntimeData().triangleCount;
	if (!triangleCount)
		return;

	if (staticInstances.size() >= MaxStaticDraws) {
		if (!loggedDrawCap) {
			logger::warn("[SSS] Distant shadow map reached {} casters; the rest are skipped", MaxStaticDraws);
			loggedDrawCap = true;
		}
		return;
	}

	float alphaRef = GetAlphaTestRef(a_geometry, 0.0f);
	ID3D11ShaderResourceView* diffuse = nullptr;
	if (alphaRef > 0.0f && (GetVertexFlags(desc) & RE::BSGraphics::Vertex::VF_UV))
		diffuse = GetTextureSRV(property->GetBaseTexture());
	if (!diffuse)
		alphaRef = 0.0f;

	StaticInstance instance{};
	BuildObjectToClip(a_geometry->world, a_cull, a_cull.depthFar, 2.0f * a_cull.depthFar, instance.ObjectToClip);
	instance.TexTransform = { 0.0f, 0.0f, 1.0f, 1.0f };
	if (auto* material = property->material) {
		instance.TexTransform = { material->texCoordOffset[0].x, material->texCoordOffset[0].y, material->texCoordScale[0].x, material->texCoordScale[0].y };
	}
	instance.Params = { alphaRef, 0.0f, 0.0f, 0.0f };

	staticDraws.push_back({ reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer),
		reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer),
		diffuse,
		GetVertexStride(desc),
		triangleCount * 3,
		GetLayoutKey(desc),
		static_cast<uint>(staticInstances.size()) });
	staticInstances.push_back(instance);
}

void DistantShadowMap::CollectTreeLOD(RE::BSGeometry* a_geometry, const CullContext& a_cull)
{
	auto* shape = static_cast<RE::BSMultiStreamInstanceTriShape*>(a_geometry);
	auto& runtimeData = a_geometry->GetGeometryRuntimeData();
	auto* rendererData = runtimeData.rendererData;
	if (!rendererData || !rendererData->vertexBuffer || !rendererData->indexBuffer)
		return;

	const uint64_t desc = ReadVertexDesc(runtimeData.vertexDesc);
	if (!IsVertexFormatSupported(desc) || !(GetVertexFlags(desc) & RE::BSGraphics::Vertex::VF_UV))
		return;

	auto& multiStream = shape->GetMultiStreamTrishapeRuntimeData();
	const uint instanceStride = 2u * multiStream.instanceSize;
	const uint triangleCount = shape->GetTrishapeRuntimeData().triangleCount;
	if (instanceStride < 8u || !triangleCount)
		return;

	auto* diffuse = GetTextureSRV(runtimeData.shaderProperty->GetBaseTexture());
	if (!diffuse)
		return;

	TreeDraw draw{};
	draw.vertexBuffer = reinterpret_cast<ID3D11Buffer*>(rendererData->vertexBuffer);
	draw.indexBuffer = reinterpret_cast<ID3D11Buffer*>(rendererData->indexBuffer);
	draw.diffuse = diffuse;
	draw.stride = GetVertexStride(desc);
	draw.indexCount = triangleCount * 3;
	draw.layoutKey = GetLayoutKey(desc);
	draw.instanceStride = instanceStride;
	BuildObjectToClip(a_geometry->world, a_cull, a_cull.depthFar, 2.0f * a_cull.depthFar, draw.constants.ObjectToClip);
	draw.constants.Params = { a_cull.billboardRotation.x, a_cull.billboardRotation.y, GetAlphaTestRef(a_geometry, 0.5f), 0.0f };

	for (auto* group : multiStream.instanceGroups) {
		if (!group || !group->vertexBuffer || !group->vertexBuffer->buffer || !group->instanceCount)
			continue;
		draw.instanceBuffer = reinterpret_cast<ID3D11Buffer*>(group->vertexBuffer->buffer);
		draw.instanceCount = group->instanceCount;
		treeDraws.push_back(draw);
	}
}

void DistantShadowMap::DrawCollected(uint a_index)
{
	auto context = globals::d3d::context;

	if (!staticInstances.empty()) {
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(instanceBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
			return;
		std::memcpy(mapped.pData, staticInstances.data(), staticInstances.size() * sizeof(StaticInstance));
		context->Unmap(instanceBuffer.get(), 0);
	}

	std::sort(staticDraws.begin(), staticDraws.end(), [](const StaticDraw& a, const StaticDraw& b) {
		if ((a.diffuse != nullptr) != (b.diffuse != nullptr))
			return a.diffuse == nullptr;
		if (a.layoutKey != b.layoutKey)
			return a.layoutKey < b.layoutKey;
		return a.diffuse < b.diffuse;
	});

	Effects11Util::D3D11FullStateBackup backup;
	backup.Save(context);
	ID3D11GeometryShader* geometryShader = nullptr;
	ID3D11HullShader* hullShader = nullptr;
	ID3D11DomainShader* domainShader = nullptr;
	context->GSGetShader(&geometryShader, nullptr, nullptr);
	context->HSGetShader(&hullShader, nullptr, nullptr);
	context->DSGetShader(&domainShader, nullptr, nullptr);

	context->GSSetShader(nullptr, nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);

	auto* dsv = sliceDSVs[a_index].get();
	context->OMSetRenderTargets(0, nullptr, dsv);
	context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
	context->OMSetDepthStencilState(depthStencilState.get(), 0);
	context->OMSetBlendState(nullptr, nullptr, 0xffffffff);

	const D3D11_VIEWPORT viewport{ 0.0f, 0.0f, static_cast<float>(resolution), static_cast<float>(resolution), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	context->RSSetState(rasterizerState.get());
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

	auto* sampler = diffuseSampler.get();
	context->PSSetSamplers(0, 1, &sampler);

	context->VSSetShader(staticVS.get(), nullptr, 0);
	context->PSSetShader(nullptr, nullptr, 0);

	uint currentLayout = UINT_MAX;
	bool alphaBound = false;
	ID3D11InputLayout* layout = nullptr;
	for (const auto& draw : staticDraws) {
		if (draw.layoutKey != currentLayout) {
			currentLayout = draw.layoutKey;
			layout = GetStaticLayout(draw.layoutKey);
			context->IASetInputLayout(layout);
		}
		if (!layout)
			continue;

		if (draw.diffuse) {
			if (!alphaBound) {
				context->PSSetShader(alphaTestPS.get(), nullptr, 0);
				alphaBound = true;
			}
			context->PSSetShaderResources(0, 1, &draw.diffuse);
		}

		ID3D11Buffer* buffers[2] = { draw.vertexBuffer, instanceBuffer.get() };
		const UINT strides[2] = { draw.stride, sizeof(StaticInstance) };
		const UINT offsets[2] = { 0, 0 };
		context->IASetVertexBuffers(0, 2, buffers, strides, offsets);
		context->IASetIndexBuffer(draw.indexBuffer, DXGI_FORMAT_R16_UINT, 0);
		context->DrawIndexedInstanced(draw.indexCount, 1, 0, 0, draw.instance);
	}

	if (!treeDraws.empty()) {
		context->VSSetShader(treeVS.get(), nullptr, 0);
		context->PSSetShader(alphaTestPS.get(), nullptr, 0);
		auto* cb = treeDrawCB->CB();
		context->VSSetConstantBuffers(0, 1, &cb);

		currentLayout = UINT_MAX;
		for (auto& draw : treeDraws) {
			if (draw.layoutKey != currentLayout) {
				currentLayout = draw.layoutKey;
				layout = GetTreeLayout(draw.layoutKey);
				context->IASetInputLayout(layout);
			}
			if (!layout)
				continue;

			context->PSSetShaderResources(0, 1, &draw.diffuse);
			ID3D11Buffer* buffers[2] = { draw.vertexBuffer, draw.instanceBuffer };
			const UINT strides[2] = { draw.stride, draw.instanceStride };
			const UINT offsets[2] = { 0, 0 };
			context->IASetVertexBuffers(0, 2, buffers, strides, offsets);
			context->IASetIndexBuffer(draw.indexBuffer, DXGI_FORMAT_R16_UINT, 0);

			const float2 facing{ draw.constants.Params.x, draw.constants.Params.y };
			const float2 rotations[2] = { facing, { -facing.y, facing.x } };
			for (const auto& rotation : rotations) {
				draw.constants.Params.x = rotation.x;
				draw.constants.Params.y = rotation.y;
				treeDrawCB->Update(draw.constants);
				context->DrawIndexedInstanced(draw.indexCount, draw.instanceCount, 0, 0, 0);
			}
		}
	}

	backup.Restore(context);
	backup.Release();
	context->GSSetShader(geometryShader, nullptr, 0);
	context->HSSetShader(hullShader, nullptr, 0);
	context->DSSetShader(domainShader, nullptr, 0);
	if (geometryShader)
		geometryShader->Release();
	if (hullShader)
		hullShader->Release();
	if (domainShader)
		domainShader->Release();
}

void DistantShadowMap::RenderCascade(uint a_index, const float3& a_lightDirection, const float3& a_cameraPosition, float a_halfExtent)
{
	ZoneScoped;
	auto* smState = globals::game::smState;
	auto* root = smState ? smState->shadowSceneNode[0] : nullptr;
	if (!root)
		return;

	auto& cascade = cascades[a_index];

	const float3 forward = -a_lightDirection;
	float3 right = forward.Cross(float3{ 0.0f, 0.0f, 1.0f });
	if (right.LengthSquared() < 1e-6f)
		right = { 1.0f, 0.0f, 0.0f };
	right.Normalize();
	float3 up = right.Cross(forward);
	up.Normalize();

	const float texelSize = 2.0f * a_halfExtent / static_cast<float>(resolution);
	const float snappedX = std::floor(a_cameraPosition.Dot(right) / texelSize) * texelSize;
	const float snappedY = std::floor(a_cameraPosition.Dot(up) / texelSize) * texelSize;
	const float depthCenter = a_cameraPosition.Dot(forward);

	const float sinElevation = std::clamp(a_lightDirection.z, 1e-3f, 1.0f);
	const float cosElevation = std::sqrt(std::max(0.0f, 1.0f - sinElevation * sinElevation));

	float2 facing{ 1.0f, 0.0f };
	const float horizontalLength = std::sqrt(a_lightDirection.x * a_lightDirection.x + a_lightDirection.y * a_lightDirection.y);
	if (horizontalLength > 1e-3f)
		facing = { -a_lightDirection.y / horizontalLength, a_lightDirection.x / horizontalLength };

	CullContext cull{};
	cull.center = right * snappedX + up * snappedY + forward * depthCenter;
	cull.right = right;
	cull.up = up;
	cull.forward = forward;
	cull.halfExtent = a_halfExtent;
	cull.depthFar = 1.5f * a_halfExtent + DepthMargin;
	cull.cameraPosition = a_cameraPosition;
	cull.nearSkipDistance = lastConfig.StartDistance;
	cull.shadowStretch = std::min(cosElevation / sinElevation, 8.0f);
	cull.minCasterRadius = lastConfig.MinCasterRadius;
	cull.billboardRotation = facing;
	cull.includeTrees = lastConfig.IncludeTreeLOD;
	cull.includeTerrain = lastConfig.IncludeTerrain;

	staticDraws.clear();
	staticInstances.clear();
	treeDraws.clear();

	{
		ZoneScopedN("SSS - Distant Map Collect");
		Collect(root, cull);
	}

	globals::profiler->BeginPass("ScreenSpaceShadows::DistantMap");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("SSS - Distant Shadow Map");

	DrawCollected(a_index);

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
	globals::profiler->EndPass();

	cascade.valid = true;
	cascade.lightDirection = a_lightDirection;
	cascade.right = right;
	cascade.up = up;
	cascade.forward = forward;
	cascade.center = cull.center;
	cascade.halfExtent = a_halfExtent;
	cascade.depthNear = cull.depthFar;
	cascade.depthRange = 2.0f * cull.depthFar;
	cascade.lastRender = std::chrono::steady_clock::now();
}

bool DistantShadowMap::Update(const Config& a_config)
{
	if (!(a_config == lastConfig)) {
		lastConfig = a_config;
		Invalidate();
	}

	if (!EnsureResources(a_config.Resolution) || !EnsureShaders())
		return false;

	auto* smState = globals::game::smState;
	auto* shadowSceneNode = smState ? smState->shadowSceneNode[0] : nullptr;
	if (!shadowSceneNode)
		return false;

	auto* sunLight = shadowSceneNode->GetRuntimeData().sunLight;
	auto* dirLight = sunLight ? skyrim_cast<RE::NiDirectionalLight*>(sunLight->light.get()) : nullptr;
	if (!dirLight)
		return false;

	const auto& worldDirection = dirLight->GetWorldDirection();
	float3 lightDirection{ -worldDirection.x, -worldDirection.y, -worldDirection.z };
	if (lightDirection.LengthSquared() < 1e-8f)
		return false;
	lightDirection.Normalize();
	if (lightDirection.z < MinSunElevationSin) {
		Invalidate();
		return false;
	}

	const auto& cameraPosAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();
	const float3 cameraPosition{ cameraPosAdjust.x, cameraPosAdjust.y, cameraPosAdjust.z };

	const float halfExtents[CascadeCount] = {
		std::clamp(a_config.StartDistance * 1.75f, 4096.0f, a_config.Range * 0.5f),
		a_config.Range
	};

	const auto now = std::chrono::steady_clock::now();
	int priorities[CascadeCount]{};
	for (uint i = 0; i < CascadeCount; i++) {
		auto& cascade = cascades[i];
		if (cascade.valid) {
			const float3 offset = cameraPosition - cascade.center;
			const float drift = std::max(std::abs(offset.Dot(cascade.right)), std::abs(offset.Dot(cascade.up)));
			if (drift > cascade.halfExtent * 0.5f)
				cascade.valid = false;
			else if (cascade.halfExtent != halfExtents[i])
				priorities[i] = 3;
			else if (cascade.lightDirection.Dot(lightDirection) < RedrawSunAngleCos || drift > cascade.halfExtent * RecenterFraction)
				priorities[i] = 2;
			else if (a_config.UpdateInterval > 0.0f && std::chrono::duration<float, std::milli>(now - cascade.lastRender).count() >= a_config.UpdateInterval)
				priorities[i] = 1;
		}
		if (!cascade.valid)
			priorities[i] = 4;
	}

	uint chosen = CascadeCount;
	int best = 0;
	for (uint offset = 1; offset <= CascadeCount; offset++) {
		const uint i = (lastRenderedCascade + offset) % CascadeCount;
		if (priorities[i] > best) {
			best = priorities[i];
			chosen = i;
		}
	}

	if (chosen < CascadeCount) {
		RenderCascade(chosen, lightDirection, cameraPosition, halfExtents[chosen]);
		lastRenderedCascade = chosen;
	}

	for (const auto& cascade : cascades)
		if (cascade.valid)
			return true;
	return false;
}

void DistantShadowMap::GetCascadeData(CascadeData (&a_out)[CascadeCount]) const
{
	const auto& cameraPosAdjust = globals::game::frameBufferCached.GetCameraPosAdjust();
	const float3 cameraPosition{ cameraPosAdjust.x, cameraPosAdjust.y, cameraPosAdjust.z };

	for (uint i = 0; i < CascadeCount; i++) {
		const auto& cascade = cascades[i];
		a_out[i] = {};
		if (!cascade.valid || !resolution)
			continue;

		const float xyScale = 0.5f / cascade.halfExtent;
		const float depthScale = 1.0f / cascade.depthRange;
		const float3 offset = cameraPosition - cascade.center;

		a_out[i].CameraToMap[0] = { cascade.right.x * xyScale, cascade.right.y * xyScale, cascade.right.z * xyScale, 0.5f + offset.Dot(cascade.right) * xyScale };
		a_out[i].CameraToMap[1] = { -cascade.up.x * xyScale, -cascade.up.y * xyScale, -cascade.up.z * xyScale, 0.5f - offset.Dot(cascade.up) * xyScale };
		a_out[i].CameraToMap[2] = { cascade.forward.x * depthScale, cascade.forward.y * depthScale, cascade.forward.z * depthScale, (offset.Dot(cascade.forward) + cascade.depthNear) * depthScale };
		a_out[i].Params = { 2.0f * cascade.halfExtent / static_cast<float>(resolution), depthScale, 1.0f, 0.0f };
	}
}
