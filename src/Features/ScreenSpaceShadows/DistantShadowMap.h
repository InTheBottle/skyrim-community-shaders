#pragma once

#include "Buffer.h"

class DistantShadowMap
{
public:
	static constexpr uint CascadeCount = 2;

	struct Config
	{
		uint Resolution = 2048;
		float Range = 40960.0f;
		float MinCasterRadius = 128.0f;
		float UpdateInterval = 2000.0f;
		float StartDistance = 8192.0f;
		bool IncludeTreeLOD = true;
		bool IncludeTerrain = false;

		bool operator==(const Config&) const = default;
	};

	struct alignas(16) CascadeData
	{
		float4 CameraToMap[3];
		float4 Params;
	};
	STATIC_ASSERT_ALIGNAS_16(CascadeData);

	bool Update(const Config& a_config);

	void GetCascadeData(CascadeData (&a_out)[CascadeCount]) const;

	void Invalidate();

	ID3D11ShaderResourceView* GetSRV() const { return texture ? texture->srv.get() : nullptr; }
	ID3D11SamplerState* GetComparisonSampler() const { return comparisonSampler.get(); }

	void ClearShaderCache();

private:
	struct Cascade
	{
		bool valid = false;
		float3 lightDirection;
		float3 right;
		float3 up;
		float3 forward;
		float3 center;
		float halfExtent = 0.0f;
		float depthNear = 0.0f;
		float depthRange = 1.0f;
		std::chrono::steady_clock::time_point lastRender;
	};

	struct alignas(16) StaticInstance
	{
		float4 ObjectToClip[3];
		float4 TexTransform;
		float4 Params;
	};
	STATIC_ASSERT_ALIGNAS_16(StaticInstance);

	struct TreeDrawCB
	{
		float4 ObjectToClip[3];
		float4 Params;
	};
	static_assert(sizeof(TreeDrawCB) % 16 == 0);

	struct StaticDraw
	{
		ID3D11Buffer* vertexBuffer;
		ID3D11Buffer* indexBuffer;
		ID3D11ShaderResourceView* diffuse;
		uint stride;
		uint indexCount;
		uint layoutKey;
		uint instance;
	};

	struct TreeDraw
	{
		ID3D11Buffer* vertexBuffer;
		ID3D11Buffer* indexBuffer;
		ID3D11Buffer* instanceBuffer;
		ID3D11ShaderResourceView* diffuse;
		uint stride;
		uint indexCount;
		uint layoutKey;
		uint instanceStride;
		uint instanceCount;
		TreeDrawCB constants;
	};

	struct CullContext
	{
		float3 center;
		float3 right;
		float3 up;
		float3 forward;
		float halfExtent;
		float depthFar;
		float3 cameraPosition;
		float nearSkipDistance;
		float shadowStretch;
		float minCasterRadius;
		float2 billboardRotation;
		bool includeTrees;
		bool includeTerrain;
	};

	static constexpr uint MaxStaticDraws = 65536;
	static constexpr float MinSunElevationSin = 0.02f;
	static constexpr float RedrawSunAngleCos = 0.99999f;
	static constexpr float RecenterFraction = 0.1f;
	static constexpr float DepthMargin = 32768.0f;

	bool EnsureResources(uint a_resolution);
	bool EnsureShaders();
	ID3D11InputLayout* GetStaticLayout(uint a_key);
	ID3D11InputLayout* GetTreeLayout(uint a_key);

	void RenderCascade(uint a_index, const float3& a_lightDirection, const float3& a_cameraPosition, float a_halfExtent);
	void Collect(RE::NiAVObject* a_root, const CullContext& a_cull);
	void CollectGeometry(RE::BSGeometry* a_geometry, const CullContext& a_cull);
	void CollectTreeLOD(RE::BSGeometry* a_geometry, const CullContext& a_cull);
	void DrawCollected(uint a_index);

	static bool IsVertexFormatSupported(uint64_t a_desc);
	static uint GetLayoutKey(uint64_t a_desc);
	static void BuildObjectToClip(const RE::NiTransform& a_world, const CullContext& a_cull, float a_depthNear, float a_depthRange, float4 (&a_rows)[3]);

	Config lastConfig{};
	Cascade cascades[CascadeCount];
	uint lastRenderedCascade = CascadeCount - 1;
	uint resolution = 0;

	std::unique_ptr<Texture2D> texture;
	winrt::com_ptr<ID3D11DepthStencilView> sliceDSVs[CascadeCount];
	winrt::com_ptr<ID3D11SamplerState> comparisonSampler;
	winrt::com_ptr<ID3D11SamplerState> diffuseSampler;
	winrt::com_ptr<ID3D11RasterizerState> rasterizerState;
	winrt::com_ptr<ID3D11DepthStencilState> depthStencilState;
	winrt::com_ptr<ID3D11Buffer> instanceBuffer;
	std::unique_ptr<ConstantBuffer> treeDrawCB;

	winrt::com_ptr<ID3D11VertexShader> staticVS;
	winrt::com_ptr<ID3D11VertexShader> treeVS;
	winrt::com_ptr<ID3D11PixelShader> alphaTestPS;
	winrt::com_ptr<ID3DBlob> staticVSBlob;
	winrt::com_ptr<ID3DBlob> treeVSBlob;
	bool shaderCompileFailed = false;
	std::unordered_map<uint, winrt::com_ptr<ID3D11InputLayout>> staticLayouts;
	std::unordered_map<uint, winrt::com_ptr<ID3D11InputLayout>> treeLayouts;

	std::vector<StaticDraw> staticDraws;
	std::vector<StaticInstance> staticInstances;
	std::vector<TreeDraw> treeDraws;
	std::vector<RE::NiAVObject*> traversalStack;
	bool loggedDrawCap = false;
};
