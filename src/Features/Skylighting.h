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

	REX::W32::XMFLOAT4X4 viewProjMat;

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

	enum class RenderMode : uint32_t
	{
		Depth = 0xC,
		SpotShadowmap = 0xD,
		DirectionalShadowmap = 0xE,
		OmnidirectionalShadowmap = 0xF,
		Unk17 = 0x11,
		LocalMap = 0x12,
		LocalMapFogOfWar = 0x13,
		FirstPersonView = 0x16,
		BloodSplatter = 0x17,
		DebugColor = 0x1B,
		PrecipitationOcclusionMap = 0x1C,
	};

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

	
	static void EmplacePass(RE::BSLightingShaderProperty::Data* This, RE::BSShader* shader, RE::BSShaderProperty* property, RE::BSGeometry* geometry, uint32_t technique, uint8_t numLights, RE::BSLight* light0, RE::BSLight* light1, RE::BSLight* light2, RE::BSLight* light3)
	{
		using func_t = decltype(&EmplacePass);
		REL::Relocation<func_t> func{ REL::RelocationID(98884, 25642) };
		func(This, shader, property, geometry, technique, numLights, light0, light1, light2, light3);
	}

	static RE::BSShader* GetUtilityShaderSingleton()
	{
		REL::Relocation<RE::BSShader**> singleton{ RELOCATION_ID(528354, 415300) };
		return *singleton;
	}

	stl::enumeration<RE::BSLightingShaderProperty::EShaderPropertyFlag, std::uint64_t> flagsBackup;

	struct BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl
	{
		static RE::BSLightingShaderProperty::Data* thunk(RE::BSLightingShaderProperty* This, RE::BSGeometry* geometry, [[maybe_unused]] RenderMode renderMode, [[maybe_unused]] RE::BSShaderAccumulator* accumulator)
		{
			//using enum RE::BSLightingShaderProperty::EShaderPropertyFlag;
			//using enum Flags;

			//auto& flags = This->flags;

			//if (flags.none(kRefraction, kTempRefraction, kZBufferWrite, kSkinned, kMultiTextureLandscape, kNoLODLandBlend, kLODLandscape, kEyeReflect, kDecal, kDynamicDecal, kAnisotropicLighting)) {
			//	const auto alphaProperty = static_cast<RE::NiAlphaProperty*>(geometry->GetGeometryRuntimeData().properties[0].get());

			//	stl::enumeration<Flags> technique;
			//	technique.set(RenderDepth, Texture);

			//	if (flags.any(kModelSpaceNormals)) {
			//		technique.set(Normals, BinormalTangent);
			//	}
			//	if (flags.any(kVertexColors)) {
			//		technique.set(Vc);
			//	}
			//	if (alphaProperty && alphaProperty->GetAlphaTesting()) {
			//		technique.set(AlphaTest);
			//	}
			//	if (flags.any(kLODObjects, kHDLODObjects)) {
			//		technique.set(LodObject);
			//	}
			//	if (flags.any(kTreeAnim)) {
			//		technique.set(TreeAnim);
			//	}
			//	EmplacePass(&This->unk0C8, GetUtilityShaderSingleton(), This, geometry, technique.underlying() + static_cast<uint32_t>(0x2B), 0, 0, 0, 0, 0);
			//}
			//return &This->unk0C8;
			using enum RE::BSLightingShaderProperty::EShaderPropertyFlag;

			GetSingleton()->flagsBackup = This->flags;
			This->flags.reset(kRefraction, kTempRefraction, kZBufferWrite, kSkinned, kMultiTextureLandscape, kNoLODLandBlend, kLODLandscape, kEyeReflect, kTreeAnim, kDecal, kDynamicDecal, kAnisotropicLighting);

			auto ret = func(This, geometry, renderMode, accumulator);
			This->flags = GetSingleton()->flagsBackup;
			return ret;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	
	struct EmplacePassesHook
	{
		static RE::BSRenderPass* thunk(RE::BSLightingShaderProperty::Data* This, RE::BSShader* shader, RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry, uint32_t, uint8_t numLights, RE::BSLight* light0, RE::BSLight* light1, RE::BSLight* light2, RE::BSLight* light3)
		{
			using enum RE::BSLightingShaderProperty::EShaderPropertyFlag;
			using enum Flags;

			stl::enumeration<Flags> techniqueE;

		//	const auto alphaProperty = static_cast<RE::NiAlphaProperty*>(geometry->GetGeometryRuntimeData().properties[0].get());

			auto flags = GetSingleton()->flagsBackup;

			stl::enumeration<Flags> technique;
			//techniqueE.set(RenderDepth, Texture);
			techniqueE.set(RenderShadowmap);

			//if (flags.any(kModelSpaceNormals)) {
			//	techniqueE.set(Normals, BinormalTangent);
			//}
			//if (flags.any(kVertexColors)) {
			//	techniqueE.set(Vc);
			//}
			//if (alphaProperty && alphaProperty->GetAlphaTesting()) {
			//	techniqueE.set(AlphaTest);
			//}
			//if (flags.any(kLODObjects, kHDLODObjects)) {
			//	techniqueE.set(LodObject);
			//}		
			//if (flags.any(kTreeAnim)) {
			//	techniqueE.set(TreeAnim);
			//}
			//if (flags.any(kSkinned)) {
			//	techniqueE.set(Skinned);
			//}

			property->flags = GetSingleton()->flagsBackup;

			auto pass = func(This, shader, property, geometry, techniqueE.underlying() + +static_cast<uint32_t>(0x2B), numLights, light0, light1, light2, light3);
			if (flags.any(kTreeAnim)) {
			//	pass->accumulationHint = 11;
			}
			return pass;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct CreateRenderTarget_Occlusion
	{
		static void thunk(RE::BSGraphics::Renderer* This, RE::RENDER_TARGETS_DEPTHSTENCIL a_target, RE::BSGraphics::DepthStencilTargetProperties* a_properties)
		{
			a_properties->height = 2048;
			a_properties->width = 2048;

			func(This, a_target, a_properties);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Hooks
	{
		static void Install()
		{
			stl::write_vfunc<0x2D, BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl>(RE::VTABLE_BSLightingShaderProperty[0]);
			stl::write_thunk_call<EmplacePassesHook>(REL::RelocationID(99874, 36559).address() + REL::Relocate(0x1AD, 0x841, 0x791));
			stl::write_thunk_call<CreateRenderTarget_Occlusion>(REL::RelocationID(100458, 107175).address() + REL::Relocate(0x1245, 0x3F3, 0x548));

		}
	};

	bool SupportsVR() override { return false; };
};
