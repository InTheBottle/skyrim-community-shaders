#pragma once

#include <FidelityFX/dx11/ffx_fsr2_dx11.h>
#include <FidelityFX/ffx_fsr2.h>

#include "Buffer.h"
#include "State.h"

class FidelityFX
{
public:
	static FidelityFX* GetSingleton()
	{
		static FidelityFX singleton;
		return &singleton;
	}

	bool enableSuperResolution = true;
	float sharpness = 0.0f;

	FfxFsr2Interface ffxInterface;
	FfxFsr2ContextDescription initializationParameters = {};
	FfxFsr2Context fsrContext;

	Texture2D* upscalingTempTexture;

	Texture2D* opaqueOnlyColorTexture;
	Texture2D* reactiveMaskTexture;

	void DrawSettings();

	FfxErrorCode Initialize();
	void CreateUpscalingResources();

	void ReplaceJitter();
	void DispatchUpscaling();

	bool validTaaPass = false;

	struct UpdateJitter
	{
		static void thunk(RE::BSGraphics::State* a_state)
		{
			func(a_state);
			GetSingleton()->ReplaceJitter();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct TAA_BeginTechnique
	{
		static void thunk(RE::BSImagespaceShaderISTemporalAA* a_shader, RE::BSTriShape* a_null)
		{
			func(a_shader, a_null);
			GetSingleton()->validTaaPass = true;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct TAA_EndTechnique
	{
		static void thunk(RE::BSImagespaceShaderISTemporalAA* a_shader, RE::BSTriShape* a_null)
		{
			auto singleton = GetSingleton();
			if (singleton->enableSuperResolution && singleton->validTaaPass)
				singleton->DispatchUpscaling();
			else
				func(a_shader, a_null);
			singleton->validTaaPass = false;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void InstallHooks()
	{
		stl::write_thunk_call<UpdateJitter>(REL::RelocationID(75460, 77245).address() + REL::Relocate(0xE5, 0xE2, 0x104));
		stl::write_thunk_call<TAA_BeginTechnique>(REL::RelocationID(100540, 36559).address() + REL::Relocate(0x3E9, 0x841, 0x791));
		stl::write_thunk_call<TAA_EndTechnique>(REL::RelocationID(100540, 36559).address() + REL::Relocate(0x3F3, 0x841, 0x791));
	}
};
