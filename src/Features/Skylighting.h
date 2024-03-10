#pragma once

#include "Buffer.h"
#include "Feature.h"

struct Skylighting : Feature
{
public:
	static Skylighting* GetSingleton()
	{
		static Skylighting singleton;
		return &singleton;
	}

	static void InstallHooks()
	{
		Hooks::Install();
	}

	std::unique_ptr<Texture2D> precipOcclusionTex = nullptr;
	RE::DirectX::XMFLOAT4X4 precipProj;

	struct alignas(16) PerPass
	{
		RE::DirectX::XMFLOAT4X4 PrecipProj;
		float4 Params;
	};

	float distanceMult = 10.0;
	float farMult = 1.0;

	std::unique_ptr<Buffer> perPass = nullptr;

	ID3D11ComputeShader* horizontalSSBlur = nullptr;
	ID3D11ComputeShader* verticalSSBlur = nullptr;
	
	virtual inline std::string GetName() { return "Skylighting"; }
	virtual inline std::string GetShortName() { return "Skylighting"; }
	inline std::string_view GetShaderDefineName() override { return "SKYLIGHTING"; }

	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	virtual void SetupResources();
	virtual void Reset();
	virtual void RestoreDefaultSettings();

	virtual void DrawSettings();
	virtual void Load(json& o_json);
	virtual void Save(json& o_json);

	virtual void Draw(const RE::BSShader* shader, const uint32_t descriptor);

	virtual void ClearShaderCache();
	ID3D11ComputeShader* GetComputeShaderHorizontalBlur();
	ID3D11ComputeShader* GetComputeShaderVerticalBlur();

	virtual void PostPostLoad() override { Hooks::Install(); }

	void BlurAndBind();

	struct Hooks
	{
		struct Main_RenderWorld_Start
		{
			static void thunk(RE::BSBatchRenderer* This, uint32_t StartRange, uint32_t EndRanges, uint32_t RenderFlags, int GeometryGroup)
			{
				GetSingleton()->BlurAndBind();
				func(This, StartRange, EndRanges, RenderFlags, GeometryGroup);  // RenderBatches
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSParticleShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
			{
				func(This, Pass, RenderFlags);

				auto particleShaderProperty = (RE::BSParticleShaderProperty*)Pass->shaderProperty;
				auto cube = (RE::BSParticleShaderCubeEmitter*)particleShaderProperty->particleEmitter;
				GetSingleton()->precipProj = cube->occlusionProjection;

				auto renderer = RE::BSGraphics::Renderer::GetSingleton();
				auto context = renderer->GetRuntimeData().context;
				auto precipation = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP];
				context->CopyResource(GetSingleton()->precipOcclusionTex->resource.get(), precipation.texture);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Precipitation_UpdateCamera_SetViewFrustum
		{
			static void thunk(RE::NiCamera* a1, RE::NiFrustum& frustum)
			{
				float distanceMult = GetSingleton()->distanceMult;
				float farMult = GetSingleton()->farMult;

				RE::NiFrustum newFrustum;
				newFrustum.fLeft = -250 * distanceMult;
				newFrustum.fRight = 250 * distanceMult;
				newFrustum.fTop = 250 * distanceMult;
				newFrustum.fBottom = -250 * distanceMult;
				newFrustum.fFar = 2000 * farMult;
				frustum = newFrustum;
				func(a1, frustum);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct CreateRenderTarget_Occlusion
		{
			static void thunk(RE::BSGraphics::Renderer* This, RE::RENDER_TARGETS_DEPTHSTENCIL a_target, RE::BSGraphics::DepthStencilTargetProperties* a_properties)
			{
				a_properties->height = 4096;
				a_properties->width = 4096;

				func(This, a_target, a_properties);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_thunk_call<Main_RenderWorld_Start>(REL::RelocationID(99938, 106583).address() + REL::Relocate(0x8E, 0x84));
			stl::write_thunk_call<Precipitation_UpdateCamera_SetViewFrustum>(REL::RelocationID(25643, 25643).address() + REL::Relocate(0x5D9, 0x5D9));
			stl::write_vfunc<0x6, BSParticleShader_SetupGeometry>(RE::VTABLE_BSParticleShader[0]);	
			stl::write_thunk_call<CreateRenderTarget_Occlusion>(REL::RelocationID(100458, 107175).address() + REL::Relocate(0x1245, 0x3F3, 0x548));

			logger::info("[SSS] Installed hooks");
		}
	};
};
