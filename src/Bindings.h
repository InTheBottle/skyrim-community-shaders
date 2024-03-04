#pragma once

#include "Buffer.h"

class Bindings
{
public:
	static Bindings* GetSingleton()
	{
		static Bindings singleton;
		return &singleton;
	}

	void DepthStencilStateSetDepthMode(RE::BSGraphics::DepthStencilDepthMode a_mode);

	void AlphaBlendStateSetMode(uint32_t a_mode);
	void AlphaBlendStateSetAlphaToCoverage(uint32_t a_value);
	void AlphaBlendStateSetWriteMode(uint32_t a_value);

	void SetupResources();
	void Reset();

	void StartDeferred();
	void EndDeferred();

	struct Hooks
	{
		struct Main_RenderWorld_Start
		{
			static void thunk(RE::BSBatchRenderer* This, uint32_t StartRange, uint32_t EndRanges, uint32_t RenderFlags, int GeometryGroup)
			{
				// Here is where the first opaque objects start rendering
				GetSingleton()->StartDeferred();
				func(This, StartRange, EndRanges, RenderFlags, GeometryGroup);  // RenderBatches
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderWorld_End
		{
			static void thunk(RE::BSBatchRenderer* This, uint32_t StartRange, uint32_t EndRanges, uint32_t RenderFlags, int GeometryGroup)
			{
				// Here is where the the game starts rendering geometry with alpha (sky, decals)
				GetSingleton()->EndDeferred();
				func(This, StartRange, EndRanges, RenderFlags, GeometryGroup);  // RenderSky
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_thunk_call<Main_RenderWorld_Start>(REL::RelocationID(99938, 106583).address() + REL::Relocate(0x8E, 0x84));
			stl::write_thunk_call<Main_RenderWorld_End>(REL::RelocationID(99938, 106583).address() + REL::Relocate(0x247, 0x237));
			logger::info("[Bindings] Installed hooks");
		}
	};
};
