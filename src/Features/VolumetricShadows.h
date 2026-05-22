#pragma once

#include "Buffer.h"

struct VolumetricShadows : Feature
{
public:
	virtual inline std::string GetName() override { return "Volumetric Shadows"; }
	virtual inline std::string GetShortName() override { return "VolumetricShadows"; }
	virtual inline std::string_view GetShaderDefineName() override { return "VOLUMETRIC_SHADOWS"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	virtual bool IsCore() const override { return true; }
	virtual bool IsInMenu() const override { return true; }

	static constexpr uint32_t kSharedShadowMapShaderSlot = 18;

	// Froxel grid dimensions. The width doubles in VR to cover side-by-side stereo.
	static constexpr uint32_t kFroxelGridWidth = 160;
	static constexpr uint32_t kFroxelGridHeight = 96;
	static constexpr uint32_t kFroxelGridDepth = 64;

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Volumetric Shadows pre-filters the directional shadow cascades into a view-space froxel grid.\n"
			"Consumers (particles, decals, effects, transparent geometry) sample the grid directly,\n"
			"which is cheaper than re-projecting into shadow space per pixel and gives smoother results than VSM.",
			{ "View-space froxel grid",
				"PCF pre-filtering",
				"Multi-cascade blending",
				"Optimized for effects rendering" }
		};
	}

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	// Pre-filtered shadow froxel grid (R8 visibility in view space).
	ID3D11Texture3D* shadowFroxelTexture = nullptr;
	ID3D11ShaderResourceView* shadowFroxelSRV = nullptr;
	ID3D11UnorderedAccessView* shadowFroxelUAV = nullptr;
	uint32_t froxelWidth = 0;
	uint32_t froxelHeight = 0;
	uint32_t froxelDepth = 0;

	ID3D11ComputeShader* buildShadowFroxelCS = nullptr;

	// Samplers
	ID3D11SamplerState* linearSampler = nullptr;
	ID3D11SamplerState* comparisonSampler = nullptr;

	// Build-time configuration uploaded each dispatch.
	struct alignas(16) VolumetricShadowsCB
	{
		uint32_t GridSize[3];
		uint32_t HasShadows;

		float NearZ;
		float FarZ;
		float ShadowBias;
		float pad0;
	};
	static_assert(sizeof(VolumetricShadowsCB) % 16 == 0, "VolumetricShadowsCB must be 16-byte aligned");

	ConstantBuffer* configCB = nullptr;

	// Transient pointer captured from the shadow rendering pass.
	ID3D11ShaderResourceView* shadowView = nullptr;

	virtual void DrawSettings() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;

	// Captured during Skyrim's shadow rendering pass (slot t4 holds the cascade SRV here).
	// Triggered from State::Draw on the RenderShadowmask utility shader.
	void BuildShadowFroxel();

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	virtual bool SupportsVR() override { return true; }

	virtual void PostPostLoad() override;

private:
	void CreateFroxelResources();
	void CompileShaders();

	static void SetSharedShadowMapSRV(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_srv);
};
