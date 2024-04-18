#pragma once

#include "Buffer.h"
#include "Feature.h"
#include "State.h"

struct Skylighting : Feature
{
public:
	static Skylighting* GetSingleton()
	{
		static Skylighting singleton;
		return &singleton;
	}

	virtual inline std::string GetName() { return "Skylighting"; }
	virtual inline std::string GetShortName() { return "Skylighting"; }
	inline std::string_view GetShaderDefineName() override { return "SKYLIGHTING"; }

	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	virtual void SetupResources();
	virtual void Reset();

	virtual void DrawSettings();

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual inline void PostPostLoad() override { Hooks::Install(); }

	virtual void RestoreDefaultSettings();

	ID3D11ComputeShader* skylightingCS = nullptr;
	ID3D11ComputeShader* GetSkylightingCS();

	struct alignas(16) PerFrameCB
	{
		float4 BufferDim;
		float4 DynamicRes;
		uint FrameCount;
		uint pad0[3];
	};

	ConstantBuffer* perFrameCB = nullptr;

	virtual void ClearShaderCache() override;
	
	struct alignas(16) PerGeometry
	{
		float4 VPOSOffset;
		float4 ShadowSampleParam;    // fPoissonRadiusScale / iShadowMapResolution in z and w
		float4 EndSplitDistances;    // cascade end distances int xyz, cascade count int z
		float4 StartSplitDistances;  // cascade start ditances int xyz, 4 int z
		float4 FocusShadowFadeParam;
		float4 DebugColor;
		float4 PropertyColor;
		float4 AlphaTestRef;
		float4 ShadowLightParam;  // Falloff in x, ShadowDistance squared in z
		DirectX::XMFLOAT4X3 FocusShadowMapProj[4];
		DirectX::XMFLOAT4X3 ShadowMapProj[4];
		DirectX::XMFLOAT4X4 CameraViewProjInverse;
	};

	ID3D11ComputeShader* copyShadowCS = nullptr;

	void CopyShadowData();

	Buffer* perShadow = nullptr;
	ID3D11ShaderResourceView* shadowView = nullptr;
	Texture2D* skylightingTexture = nullptr;

	ID3D11ShaderResourceView* noiseView = nullptr;

	void Bind();
	void Compute();

	struct Hooks
	{

		static void Install()
		{
		}
	};

	bool SupportsVR() override { return false; };
};
