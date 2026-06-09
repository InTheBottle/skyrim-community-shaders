#pragma once

#include "Buffer.h"

struct VolumetricShadows : Feature
{
public:
	virtual inline std::string GetName() override { return "Volumetric Shadows"; }
	virtual std::string GetDisplayName() override { return T("feature.volumetric_shadows.name", "Volumetric Shadows"); }
	virtual inline std::string GetShortName() override { return "VolumetricShadows"; }
	virtual inline std::string_view GetShaderDefineName() override { return "VOLUMETRIC_SHADOWS"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLighting; }
	virtual bool IsCore() const override { return true; }
	virtual bool IsInMenu() const override { return true; }

	static constexpr uint32_t kSharedShadowMapShaderSlot = 18;

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.volumetric_shadows.description", "Volumetric Shadows provides downsampled MSM shadow maps for use by effects like particles and decals.\nThis improves shadow quality on transparent objects with minimal performance impact."),
			{ T("feature.volumetric_shadows.key_feature_1", "Downsampled MSM shadows"),
				T("feature.volumetric_shadows.key_feature_2", "Gaussian blur filtering"),
				T("feature.volumetric_shadows.key_feature_3", "Multi-cascade support"),
				T("feature.volumetric_shadows.key_feature_4", "Optimized for effects rendering") } };
	}

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	struct Settings
	{
		float BlurRadius = 100.0f;
	};
	Settings settings;

	struct alignas(16) VSMLinearizeCB
	{
		float CascadeNear;
		float CascadeFar;
		uint32_t _pad[2];
	};

	struct alignas(16) BlurCB
	{
		uint32_t BlurRadius;
		uint32_t _pad[3];
	};

	float4 GetCascadeDepthParams();

	// Compute shaders
	ID3D11ComputeShader* downsampleShadowMip0CS = nullptr;
	ID3D11ComputeShader* downsampleShadowMip1CS = nullptr;
	ID3D11ComputeShader* blurShadowHorizontalCS = nullptr;
	ID3D11ComputeShader* blurShadowVerticalCS = nullptr;

	ID3D11ShaderResourceView* shadowView = nullptr;

	// Downsampled shadow texture with 2 mip levels
	ID3D11Texture2D* shadowCopyTexture = nullptr;
	ID3D11ShaderResourceView* shadowCopySRV = nullptr;
	ID3D11ShaderResourceView* shadowCopyMip0SRV = nullptr;
	ID3D11ShaderResourceView* shadowCopyMip1SRV = nullptr;
	ID3D11UnorderedAccessView* shadowCopyMip0UAV = nullptr;
	ID3D11UnorderedAccessView* shadowCopyMip1UAV = nullptr;
	uint32_t shadowCopyWidth = 0;
	uint32_t shadowCopyHeight = 0;

	// Temporary texture for blur intermediate result
	ID3D11Texture2D* shadowBlurTempTexture = nullptr;
	ID3D11ShaderResourceView* shadowBlurTempMip0SRV = nullptr;
	ID3D11ShaderResourceView* shadowBlurTempMip1SRV = nullptr;
	ID3D11UnorderedAccessView* shadowBlurTempMip0UAV = nullptr;
	ID3D11UnorderedAccessView* shadowBlurTempMip1UAV = nullptr;

	// Cbuffers
	ID3D11Buffer* linearizeCB = nullptr;
	ID3D11Buffer* blurCB = nullptr;

	// Cached cascade near/far values and projection scale
	float cascadeNear[2] = { 0.f, 0.f };
	float cascadeFar[2] = { 1.f, 1.f };
	float cascadeScale[2] = { 1.f, 1.f };

	// Samplers
	ID3D11SamplerState* linearSampler = nullptr;

	virtual void DrawSettings() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;

	void CopyShadowLightData();

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;


	virtual void PostPostLoad() override;

private:
	static void SetSharedShadowMapSRV(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_srv);
	void ExtractCascadeNearFar();
};
