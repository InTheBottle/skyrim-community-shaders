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

	REX::W32::XMFLOAT4X4 viewProjMat;
	float occlusionDistance = 10000;

	ID3D11ComputeShader* skylightingCS = nullptr;
	ID3D11ComputeShader* GetSkylightingCS();

	ID3D11SamplerState* comparisonSampler;

	struct alignas(16) PerFrameCB
	{
		REX::W32::XMFLOAT4X4 viewProjMat;
		float4 ShadowDirection;
		float4 BufferDim;
		float4 DynamicRes;
		float4 CameraData;
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

	enum class ShaderTechnique
	{
		// Sky
		SkySunOcclude = 0x2,

		// Grass
		GrassNoAlphaDirOnlyFlatLit = 0x3,
		GrassNoAlphaDirOnlyFlatLitSlope = 0x5,
		GrassNoAlphaDirOnlyVertLitSlope = 0x6,
		GrassNoAlphaDirOnlyFlatLitBillboard = 0x13,
		GrassNoAlphaDirOnlyFlatLitSlopeBillboard = 0x14,

		// Utility
		UtilityGeneralStart = 0x2B,

		// Effect
		EffectGeneralStart = 0x4000002C,

		// Lighting
		LightingGeneralStart = 0x4800002D,

		// DistantTree
		DistantTreeDistantTreeBlock = 0x5C00002E,
		DistantTreeDepth = 0x5C00002F,

		// Grass
		GrassDirOnlyFlatLit = 0x5C000030,
		GrassDirOnlyFlatLitSlope = 0x5C000032,
		GrassDirOnlyVertLitSlope = 0x5C000033,
		GrassDirOnlyFlatLitBillboard = 0x5C000040,
		GrassDirOnlyFlatLitSlopeBillboard = 0x5C000041,
		GrassRenderDepth = 0x5C00005C,

		// Sky
		SkySky = 0x5C00005E,
		SkyMoonAndStarsMask = 0x5C00005F,
		SkyStars = 0x5C000060,
		SkyTexture = 0x5C000061,
		SkyClouds = 0x5C000062,
		SkyCloudsLerp = 0x5C000063,
		SkyCloudsFade = 0x5C000064,

		// Particle
		ParticleParticles = 0x5C000065,
		ParticleParticlesGryColorAlpha = 0x5C000066,
		ParticleParticlesGryColor = 0x5C000067,
		ParticleParticlesGryAlpha = 0x5C000068,
		ParticleEnvCubeSnow = 0x5C000069,
		ParticleEnvCubeRain = 0x5C00006A,

		// Water
		WaterSimple = 0x5C00006B,
		WaterSimpleVc = 0x5C00006C,
		WaterStencil = 0x5C00006D,
		WaterStencilVc = 0x5C00006E,
		WaterDisplacementStencil = 0x5C00006F,
		WaterDisplacementStencilVc = 0x5C000070,
		WaterGeneralStart = 0x5C000071,

		// Sky
		SkySunGlare = 0x5C006072,

		// BloodSplater
		BloodSplaterFlare = 0x5C006073,
		BloodSplaterSplatter = 0x5C006074,
	};

	class BSUtilityShader : public RE::BSShader
	{
	public:
		enum class Flags
		{
			None = 0,
			Vc = 1 << 0,
			Texture = 1 << 1,
			Skinned = 1 << 2,
			Normals = 1 << 3,
			BinormalTangent = 1 << 4,
			AlphaTest = 1 << 7,
			LodLandscape = 1 << 8,
			RenderNormal = 1 << 9,
			RenderNormalFalloff = 1 << 10,
			RenderNormalClamp = 1 << 11,
			RenderNormalClear = 1 << 12,
			RenderDepth = 1 << 13,
			RenderShadowmap = 1 << 14,
			RenderShadowmapClamped = 1 << 15,
			GrayscaleToAlpha = 1 << 15,
			RenderShadowmapPb = 1 << 16,
			AdditionalAlphaMask = 1 << 16,
			DepthWriteDecals = 1 << 17,
			DebugShadowSplit = 1 << 18,
			DebugColor = 1 << 19,
			GrayscaleMask = 1 << 20,
			RenderShadowmask = 1 << 21,
			RenderShadowmaskSpot = 1 << 22,
			RenderShadowmaskPb = 1 << 23,
			RenderShadowmaskDpb = 1 << 24,
			RenderBaseTexture = 1 << 25,
			TreeAnim = 1 << 26,
			LodObject = 1 << 27,
			LocalMapFogOfWar = 1 << 28,
			OpaqueEffect = 1 << 29,
		};

		static BSUtilityShader* GetSingleton()
		{
			REL::Relocation<BSUtilityShader**> singleton{ RELOCATION_ID(528354, 415300) };
			return *singleton;
		}
		~BSUtilityShader() override;  // 00

		// override (BSShader)
		bool SetupTechnique(std::uint32_t globalTechnique) override;            // 02
		void RestoreTechnique(std::uint32_t globalTechnique) override;          // 03
		void SetupGeometry(RE::BSRenderPass* pass, uint32_t flags) override;    // 06
		void RestoreGeometry(RE::BSRenderPass* pass, uint32_t flags) override;  // 07
	};

	struct RenderPassArray
	{
		static RE::BSRenderPass* MakeRenderPass(RE::BSShader* shader, RE::BSShaderProperty* property, RE::BSGeometry* geometry,
			uint32_t technique, uint8_t numLights, RE::BSLight** lights)
		{
			using func_t = decltype(&RenderPassArray::MakeRenderPass);
			REL::Relocation<func_t> func{ RELOCATION_ID(100717, 107497) };
			return func(shader, property, geometry, technique, numLights, lights);
		}

		static void ClearRenderPass(RE::BSRenderPass* pass)
		{
			using func_t = decltype(&RenderPassArray::ClearRenderPass);
			REL::Relocation<func_t> func{ RELOCATION_ID(100718, 107498) };
			func(pass);
		}

		void Clear()
		{
			while (head != nullptr) {
				RE::BSRenderPass* next = head->next;
				ClearRenderPass(head);
				head = next;
			}
			head = nullptr;
		}

		RE::BSRenderPass* EmplacePass(RE::BSShader* shader, RE::BSShaderProperty* property, RE::BSGeometry* geometry,
			uint32_t technique, uint8_t numLights = 0, RE::BSLight* light0 = nullptr, RE::BSLight* light1 = nullptr,
			RE::BSLight* light2 = nullptr, RE::BSLight* light3 = nullptr)
		{
			RE::BSLight* lights[4];
			lights[0] = light0;
			lights[1] = light1;
			lights[2] = light2;
			lights[3] = light3;
			auto* newPass = MakeRenderPass(shader, property, geometry, technique, numLights, lights);
			if (head != nullptr) {
				RE::BSRenderPass* lastPass = head;
				while (lastPass->next != nullptr) {
					lastPass = lastPass->next;
				}
				lastPass->next = newPass;
			} else {
				head = newPass;
			}
			return newPass;
		}

		RE::BSRenderPass* head;  // 00
		uint64_t unk08;          // 08
	};

	float boundSize = 128;

	class BSBatchRenderer
	{
	public:
		struct PersistentPassList
		{
			RE::BSRenderPass* m_Head;
			RE::BSRenderPass* m_Tail;
		};

		struct GeometryGroup
		{
			BSBatchRenderer* m_BatchRenderer;
			PersistentPassList m_PassList;
			uintptr_t UnkPtr4;
			float m_Depth;  // Distance from geometry to camera location
			uint16_t m_Count;
			uint8_t m_Flags;  // Flags
		};

		struct PassGroup
		{
			RE::BSRenderPass* m_Passes[5];
			uint32_t m_ValidPassBits;  // OR'd with (1 << PassIndex)
		};

		RE::BSTArray<void*> unk008;               // 008
		RE::BSTHashMap<RE::UnkKey, RE::UnkValue> unk020;  // 020
		std::uint64_t unk050;                 // 050
		std::uint64_t unk058;                 // 058
		std::uint64_t unk060;                 // 060
		std::uint64_t unk068;                 // 068
		GeometryGroup* m_GeometryGroups[16];
		GeometryGroup* m_AlphaGroup;
		void* unk1;
		void* unk2;
	};

	bool inOcclusion = false;

	struct BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl
	{
		static void* thunk(RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry, [[maybe_unused]] uint32_t renderMode, [[maybe_unused]] RE::BSGraphics::BSShaderAccumulator* accumulator)
		{
			auto batch = (BSBatchRenderer*)accumulator->GetRuntimeData().batchRenderer;
			batch->m_GeometryGroups[14]->m_Flags &= ~1;

			using enum RE::BSShaderProperty::EShaderPropertyFlag;
			using enum BSUtilityShader::Flags;

			auto* precipitationOcclusionMapRenderPassList = reinterpret_cast<RenderPassArray*>(&property->unk0C8);

			precipitationOcclusionMapRenderPassList->Clear();
			if (GetSingleton()->inOcclusion) {
				if (property->flags.any(kSkinned) && property->flags.none(kTreeAnim))
					return precipitationOcclusionMapRenderPassList;
			} else {
				if (property->flags.any(kSkinned))
					return precipitationOcclusionMapRenderPassList;
			}

			if (property->flags.any(kZBufferWrite) && property->flags.none(kRefraction, kTempRefraction, kMultiTextureLandscape, kNoLODLandBlend, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal, kAnisotropicLighting) && !(property->flags.any(kSkinned) && property->flags.none(kTreeAnim))) {

				if (geometry->worldBound.radius > GetSingleton()->boundSize) {
					stl::enumeration<BSUtilityShader::Flags> technique;
					technique.set(RenderDepth);

					precipitationOcclusionMapRenderPassList->EmplacePass(
						BSUtilityShader::GetSingleton(),
						property,
						geometry,
						technique.underlying() + static_cast<uint32_t>(ShaderTechnique::UtilityGeneralStart));
				}
			}
			return precipitationOcclusionMapRenderPassList;
		};
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Hooks
	{
		static void Install()
		{
			logger::info("[SKYLIGHTING] Hooking BSLightingShaderProperty::GetPrecipitationOcclusionMapRenderPassesImp");
			stl::write_vfunc<0x2D, BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl>(RE::VTABLE_BSLightingShaderProperty[0]);
		}
	};

	bool SupportsVR() override { return false; };
};
