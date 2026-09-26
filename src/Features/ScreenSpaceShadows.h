#pragma once

#include "Buffer.h"
#include "ScreenSpaceShadows/DistantShadowMap.h"

struct ScreenSpaceShadows : Feature
{
public:
	virtual inline std::string GetName() override { return "Screen Space Shadows"; }
	virtual std::string GetDisplayName() override { return T("feature.screen_space_shadows.name", "Screen Space Shadows"); }
	virtual inline std::string GetShortName() override { return "ScreenSpaceShadows"; }
	virtual inline std::string_view GetShaderDefineName() override { return "SCREEN_SPACE_SHADOWS"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kShadows; }

	/** @brief Returns a localized description and list of key features for the UI summary panel. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.screen_space_shadows.description", "Screen Space Shadows enhances shadow quality by adding detailed contact shadows and improving shadow accuracy.\nThis technique adds fine-detail shadows that traditional shadow mapping might miss."),
			{ T("feature.screen_space_shadows.key_feature_1", "Enhanced contact shadows"),
				T("feature.screen_space_shadows.key_feature_2", "Improved shadow detail"),
				T("feature.screen_space_shadows.key_feature_3", "Better shadow accuracy"),
				T("feature.screen_space_shadows.key_feature_4", "Fine-scale shadow effects"),
				T("feature.screen_space_shadows.key_feature_5", "Configurable shadow contrast") } };
	}

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	struct BendSettings
	{
		float SurfaceThickness = 0.02f;
		float BilinearThreshold = 0.02f;
		float ShadowContrast = 1.0f;
		uint Enable = 1;
		uint SampleCount = 1;
		uint pad0[3];
	};

	BendSettings bendSettings;

	enum class DistantMethod : uint
	{
		ShadowMap = 0,
		ScreenSpace = 1
	};

	struct DistantSettings
	{
		bool Enable = true;
		uint SampleCount = 12;
		float MaxRayLength = 16384.0f;
		float Intensity = 1.0f;
		float Thickness = 1.0f;
		uint Method = static_cast<uint>(DistantMethod::ShadowMap);
		uint MapResolution = 2048;
		float MapRange = 40960.0f;
		float MinCasterSize = 128.0f;
		float UpdateInterval = 2000.0f;
		float FilterRadius = 1.5f;
		float Bias = 1.5f;
		bool TreeLOD = true;
	};

	DistantSettings distantSettings;

	static constexpr uint DistantMinSampleCount = 4;
	static constexpr uint DistantMaxSampleCount = 32;
	static constexpr float DistantMinRayLength = 2048.0f;
	static constexpr float DistantMaxRayLength = 65536.0f;
	static constexpr float DistantMinThickness = 0.1f;
	static constexpr float DistantMaxThickness = 2.0f;
	static constexpr float DistantFadeLength = 1024.0f;
	static constexpr uint DistantMapResolutions[3] = { 1024, 2048, 4096 };
	static constexpr float DistantMinMapRange = 16384.0f;
	static constexpr float DistantMaxMapRange = 131072.0f;
	static constexpr float DistantMaxCasterSize = 2048.0f;
	static constexpr float DistantMaxUpdateInterval = 5000.0f;
	static constexpr float DistantMaxFilterRadius = 4.0f;
	static constexpr float DistantMaxBias = 8.0f;
	static constexpr float DistantMapBlendBand = 0.05f;

	struct alignas(16) DistantShadowsCB
	{
		float2 RenderSize;
		float2 InvRenderSize;
		float StartDistance;
		float FadeLength;
		float MaxRayLength;
		float Intensity;
		float ThicknessScale;
		uint SampleCount;
		uint UseContactShadows;
		float pad0;
		uint HalfSize[2];
		float pad1[2];
		DistantShadowMap::CascadeData MapCascades[DistantShadowMap::CascadeCount];
		float MapFilterRadius;
		float MapBiasTexels;
		float MapInvResolution;
		float MapBlendBand;
	};
	STATIC_ASSERT_ALIGNAS_16(DistantShadowsCB);

	struct alignas(16) RaymarchCB
	{
		// Runtime data returned from BuildDispatchList():
		float LightCoordinate[4];  // Values stored in DispatchList::LightCoordinate_Shader by BuildDispatchList()
		int WaveOffset[2];         // Values stored in DispatchData::WaveOffset_Shader by BuildDispatchList()

		// Renderer Specific Values:
		float FarDepthValue;   // Set to the Depth Buffer Value for the far clip plane, as determined by renderer projection matrix setup (typically 0).
		float NearDepthValue;  // Set to the Depth Buffer Value for the near clip plane, as determined by renderer projection matrix setup (typically 1).

		// Sampling data:
		float InvDepthTextureSize[2];  // Inverse of the texture dimensions for 'DepthTexture' (used to convert from pixel coordinates to UVs)
									   // If 'PointBorderSampler' is an Unnormalized sampler, then this value can be hard-coded to 1.
									   // The 'USE_HALF_PIXEL_OFFSET' macro might need to be defined if sampling at exact pixel coordinates isn't precise (e.g., if odd patterns appear in the shadow).

		float2 DynamicRes;

		BendSettings settings;
	};
	STATIC_ASSERT_ALIGNAS_16(RaymarchCB);

	ID3D11SamplerState* pointBorderSampler = nullptr;

	ConstantBuffer* raymarchCB = nullptr;
	ID3D11ComputeShader* raymarchCS = nullptr;

	Texture2D* screenSpaceShadowsTexture = nullptr;

	ConstantBuffer* distantShadowsCB = nullptr;
	ID3D11ComputeShader* distantTraceCS = nullptr;
	ID3D11ComputeShader* distantResolveCS = nullptr;
	ID3D11ComputeShader* distantShadowMapCS = nullptr;
	DistantShadowMap distantShadowMap;
	Texture2D* contactShadowsCopyTexture = nullptr;
	Texture2D* distantHalfTexture = nullptr;

	/** @brief Creates the raymarch constant buffer, point border sampler, and shadow output texture. */
	virtual void SetupResources() override;

	/** @brief Draws the ImGui settings UI for screen-space shadow configuration. */
	virtual void DrawSettings() override;

	/** @brief Releases the compiled raymarch compute shader for recompilation. */
	virtual void ClearShaderCache() override;
	/** @brief Releases the raymarch compute shader so it is recompiled on next use. */
	void InvalidateRaymarchShaders();
	/** @brief Calculates the resolution-scaled and quantized sample count for the raymarch shader. */
	uint GetScaledSampleCount();
	uint lastCompiledSampleCount = 0;
	/**
	 * @brief Returns the compiled raymarch compute shader, recompiling if the sample count changed.
	 * @return The compiled ID3D11ComputeShader, or nullptr on failure.
	 */
	ID3D11ComputeShader* GetComputeRaymarch();

	/** @brief Clears the shadow texture and dispatches shadow ray marching if conditions are met. */
	virtual void Prepass() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	/** @brief Dispatches the Bend SSS compute shader to generate screen-space contact shadows. */
	void DrawShadows();

	bool CompileDistantShadows();
	float GetShadowCascadeEndDistance();
	void DrawDistantShadows(bool a_hasContactShadows);
	bool DrawDistantShadowMap(DistantShadowsCB& a_data);

	virtual void RestoreDefaultSettings() override;
};
