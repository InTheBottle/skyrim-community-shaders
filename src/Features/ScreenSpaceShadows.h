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

	struct BendSettings
	{
		float SurfaceThickness = 0.01f;
		float BilinearThreshold = 0.02f;
		float ShadowContrast = 1.0f;
		uint EnableShadows = 1;
		uint EnableBlur = 1;
	};

	BendSettings bendSettings;

	struct alignas(16) RaymarchCB
	{
		// Runtime data returned from BuildDispatchList():
		float LightCoordinate[4];  // Values stored in DispatchList::LightCoordinate_Shader by BuildDispatchList()
		int WaveOffset[2];         // Values stored in DispatchData::WaveOffset_Shader by BuildDispatchList()

		// Renderer Specific Values:
		float FarDepthValue;   // Set to the Depth Buffer Value for the far clip plane, as determined by renderer projection matrix setup (typically 0).
		float NearDepthValue;  // Set to the Depth Buffer Value for the near clip plane, as determined by renderer projection matrix setup (typically 1).

		// Sampling data:
		float InvDepthTextureSize[2];	// Inverse of the texture dimensions for 'DepthTexture' (used to convert from pixel coordinates to UVs)
										// If 'PointBorderSampler' is an Unnormalized sampler, then this value can be hard-coded to 1.
										// The 'USE_HALF_PIXEL_OFFSET' macro might need to be defined if sampling at exact pixel coordinates isn't precise (e.g., if odd patterns appear in the shadow).
		
		BendSettings settings;
		uint pad[1];
	};

	ID3D11SamplerState* pointBorderSampler = nullptr;
	ID3D11SamplerState* linearSampler = nullptr;

	ConstantBuffer* raymarchCB = nullptr;
	ID3D11ComputeShader* raymarch = nullptr;
	ID3D11ComputeShader* blurH = nullptr;
	ID3D11ComputeShader* blurV = nullptr;

	Texture2D* shadowMaskTemp = nullptr;

	virtual void SetupResources();
	virtual void Reset();

	virtual void DrawSettings();

	virtual void ClearShaderCache() override;
	ID3D11ComputeShader* GetComputeShader();
	ID3D11ComputeShader* GetComputeShaderBlurH();
	ID3D11ComputeShader* GetComputeShaderBlurV();

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	void DrawShadows();
	void BlurShadowMask();

	virtual void RestoreDefaultSettings();
};
