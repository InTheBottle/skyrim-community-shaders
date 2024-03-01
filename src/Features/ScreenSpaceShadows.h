#pragma once

#include "Buffer.h"
#include "Feature.h"

struct ScreenSpaceShadows : Feature
{
	static ScreenSpaceShadows* GetSingleton()
	{
		static ScreenSpaceShadows singleton;
		return &singleton;
	}

	virtual inline std::string GetName() { return "Screen-Space Shadows"; }
	virtual inline std::string GetShortName() { return "ScreenSpaceShadows"; }
	inline std::string_view GetShaderDefineName() override { return "SCREEN_SPACE_SHADOWS"; }

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	struct Settings
	{
		uint32_t MaxSamples = 24;
		float FarDistanceScale = 0.025f;
		float FarThicknessScale = 0.025f;
		float FarHardness = 8.0f;
		float NearDistance = 16.0f;
		float NearThickness = 2.0f;
		float NearHardness = 32.0f;
		float BlurRadius = 0.5f;
		float BlurDropoff = 0.005f;
		bool Enabled = true;
	};

	struct alignas(16) PerPass
	{
		uint32_t EnableSSS;
		uint32_t FrameCount;
		uint32_t pad[2];
	};

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
		uint pad[2];
	};

	Settings settings;

	ConstantBuffer* perPass = nullptr;

	ID3D11SamplerState* pointBorderSampler = nullptr;

	Texture2D* screenSpaceShadowsTexture = nullptr;

	ConstantBuffer* raymarchCB = nullptr;
	ID3D11ComputeShader* raymarchProgram = nullptr;

	bool renderedScreenCamera = false;

	virtual void SetupResources();
	virtual void Reset();

	virtual void DrawSettings();
	void ModifyGrass(const RE::BSShader* shader, const uint32_t descriptor);
	void ModifyDistantTree(const RE::BSShader*, const uint32_t descriptor);

	virtual void ClearShaderCache() override;
	ID3D11ComputeShader* GetComputeShader();

	void ModifyLighting(const RE::BSShader* shader, const uint32_t descriptor);
	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual void RestoreDefaultSettings();
};
