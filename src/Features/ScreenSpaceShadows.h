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

	struct Settings
	{
		uint32_t MaxSamples = !REL::Module::IsVR() ? 16u : 6u;
		float FarDistanceScale = 0.025f;
		float FarThicknessScale = 0.025f;
		float FarHardness = 8.0f;
		float NearDistance = 16.0f;
		float NearThickness = 4.0f;
		float NearHardness = 8.0f;
		bool Enabled = true;
	};

	struct alignas(16) RaymarchCB
	{
		float2 BufferDim;
		float2 RcpBufferDim;
		float4x4 ProjMatrix[2];
		float4x4 InvProjMatrix[2];
		float4 CameraData;
		float4 DynamicRes;
		float4 InvDirLightDirectionVS;
		float ShadowDistance = 10000;
		Settings Settings;
		uint32_t pad[3];
	};

	Settings settings;

	ID3D11SamplerState* computeSampler = nullptr;

	ConstantBuffer* raymarchCB = nullptr;
	ID3D11ComputeShader* raymarchProgram = nullptr;

	bool renderedScreenCamera = false;
	void DrawShadows();
	virtual void SetupResources();
	virtual void Reset();

	virtual void DrawSettings();
	virtual void ClearShaderCache() override;
	ID3D11ComputeShader* GetComputeShader();

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual void RestoreDefaultSettings();
};
