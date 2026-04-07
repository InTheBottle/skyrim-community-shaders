#pragma once

struct ENBPostProcessing : Feature
{
public:
	virtual inline std::string GetName() override { return "ENB Post Processing"; }
	virtual inline std::string GetShortName() override { return "ENBPostProcessing"; }
	virtual std::string_view GetCategory() const override { return "Post-Processing"; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"ENB Post Processing provides a framework for loading and executing ENBSeries-compatible FX effect files.\n"
			"This allows for advanced post-processing effects and visual enhancements using DirectX 11 Effect (.fx) files.",
			{ "ENBSeries-compatible FX support",
				"DirectX 11 Effect file loading",
				"Advanced post-processing pipeline",
				"Custom technique execution",
				"Dynamic UI variable system" }
		};
	}

	struct alignas(16) PerFrame
	{
		uint32_t Enable;
		uint32_t EnableSky;
		float SkyBoostIntensity;
		float GradientIntensity;

		float GradientDesaturation;
		float GradientTopIntensity;
		float GradientTopCurve;
		float pad0;

		float3 GradientTopColorFilter;
		float GradientMiddleIntensity;

		float GradientMiddleCurve;
		float3 GradientMiddleColorFilter;

		float GradientHorizonIntensity;
		float GradientHorizonCurve;
		float2 pad1;

		float3 GradientHorizonColorFilter;
		float CloudsIntensity;

		float CloudsCurve;
		float CloudsDesaturation;
		float CloudsOpacity;
		float pad2;

		float3 CloudsColorFilter;
		float CloudsEdgeIntensity;

		float CloudsEdgeMoonMultiplier;
		float ColorPow;
		float VolumetricRaysRangeFactor;
		float VolumetricRaysDesaturation;

		float3 VolumetricRaysColorFilter;
		float pad3;
	};

	bool enableEffect = false;

	PerFrame GetCommonBufferData();

	virtual void DrawSettings() override;
	virtual void SetupResources() override;
	virtual void Reset() override;
	void OverrideWeather(RE::Sky* a_sky);
	void CheckCommonData();
	void OverridePointLightColor(float3& a_color);
	struct DirectionalAmbientColors
	{
		RE::NiColor directionalAmbientColors[3][2];
	};
	void OverrideAmbientLighting(DirectionalAmbientColors& DirectionalAmbientColors);
	void ModifySky(RE::BSRenderPass* Pass);
	virtual void PostPostLoad() override;
};