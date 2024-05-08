#pragma once

#include "Buffer.h"
#include "Feature.h"
#include "State.h"

struct SSVGI : Feature
{
public:
	static SSVGI* GetSingleton()
	{
		static SSVGI singleton;
		return &singleton;
	}

	virtual inline std::string GetName() { return "Screenspace Variance Global Illumination "; }
	virtual inline std::string GetShortName() { return "SSVGI"; }
	inline std::string_view GetShaderDefineName() override { return "SSVGI"; }

	bool HasShaderDefine(RE::BSShader::Type) override { return false; };
	bool SupportsVR() override { return false; };

	virtual void SetupResources();
	virtual inline void Reset() {}

	virtual void DrawSettings();

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual void RestoreDefaultSettings();

	Texture2D* blurredDepth = nullptr;
	Texture2D* blurredRadiance = nullptr;

	ID3D11ComputeShader* copyBuffersCS = nullptr;
	ID3D11ComputeShader* GetCopyBuffersCS();

	ID3D11ComputeShader* indirectLightingCS = nullptr;
	ID3D11ComputeShader* GetIndirectLightingCS();
	void ComputeIndirectLighting(Texture2D* giTexture);

	virtual void ClearShaderCache() override;
};

