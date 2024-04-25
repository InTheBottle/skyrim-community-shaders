#pragma once

#include "Buffer.h"
#include "State.h"
#include <State.h>
#include <Features/SKylighting.h>

#define ALBEDO RE::RENDER_TARGETS::kINDIRECT
#define SPECULAR RE::RENDER_TARGETS::kINDIRECT_DOWNSCALED
#define REFLECTANCE RE::RENDER_TARGETS::kRAWINDIRECT
#define NORMALROUGHNESS RE::RENDER_TARGETS::kRAWINDIRECT_DOWNSCALED
#define MASKS RE::RENDER_TARGETS::kRAWINDIRECT_PREVIOUS
#define CLOUDSHADOWS RE::RENDER_TARGETS::kRAWINDIRECT_PREVIOUS_DOWNSCALED

class Deferred
{
public:
	static Deferred* GetSingleton()
	{
		static Deferred singleton;
		return &singleton;
	}

	void DepthStencilStateSetDepthMode(RE::BSGraphics::DepthStencilDepthMode a_mode);

	void AlphaBlendStateSetMode(uint32_t a_mode);
	void AlphaBlendStateSetAlphaToCoverage(uint32_t a_value);
	void AlphaBlendStateSetWriteMode(uint32_t a_value);

	void SetupResources();
	void Reset();

	void StartDeferred();
	void OverrideBlendStates();
	void ResetBlendStates();
	void DeferredPasses();
	void EndDeferred();

	ID3D11BlendState* deferredBlendStates[7];
	ID3D11BlendState* forwardBlendStates[7];
	RE::RENDER_TARGET forwardRenderTargets[4];

	ID3D11ComputeShader* directionalShadowCS = nullptr;
	ID3D11ComputeShader* directionalCS = nullptr;
	ID3D11ComputeShader* ambientCompositeCS = nullptr;
	ID3D11ComputeShader* mainCompositeCS = nullptr;

	std::unordered_set<std::string> perms;
	void UpdatePerms();

	void ClearShaderCache();
	ID3D11ComputeShader* GetComputeAmbientComposite();
	ID3D11ComputeShader* GetComputeMainComposite();
	ID3D11ComputeShader* GetComputeDirectionalShadow();
	ID3D11ComputeShader* GetComputeDirectional();

	bool inWorld = false;
	bool deferredPass = false;

	struct alignas(16) DeferredCB
	{
		float4 CamPosAdjust[2];
		float4 DirLightDirectionVS[2];
		float4 DirLightColor;
		float4 CameraData;
		float2 BufferDim;
		float2 RcpBufferDim;
		DirectX::XMFLOAT4X4 ViewMatrix[2];
		DirectX::XMFLOAT4X4 ProjMatrix[2];
		DirectX::XMFLOAT4X4 ViewProjMatrix[2];
		DirectX::XMFLOAT4X4 InvViewMatrix[2];
		DirectX::XMFLOAT4X4 InvProjMatrix[2];
		DirectX::XMFLOAT4X4 InvViewProjMatrix[2];
		DirectX::XMFLOAT3X4 DirectionalAmbient;
		uint FrameCount;
		uint pad0[3];
	};

	ConstantBuffer* deferredCB = nullptr;

	ID3D11SamplerState* linearSampler = nullptr;

	Texture2D* giTexture = nullptr;  // RGB - GI/IL, A - AO

	struct BSParticleShaderRainEmitter
	{
		void* vftable_BSParticleShaderRainEmitter_0;
		char _pad_8[4056];
	};

	static void Precipitation_SetupMask(RE::Precipitation* a_This)
	{
		using func_t = decltype(&Precipitation_SetupMask);
		REL::Relocation<func_t> func{ REL::RelocationID(25641, 25641) };
		func(a_This);
	}

	static void Precipitation_RenderMask(RE::Precipitation* a_This, BSParticleShaderRainEmitter* a_emitter)
	{
		using func_t = decltype(&Precipitation_RenderMask);
		REL::Relocation<func_t> func{ REL::RelocationID(25642, 25642) };
		func(a_This, a_emitter);
	}

	void UpdateConstantBuffer();
	
	struct alignas(16) ViewData
	{
		DirectX::XMVECTOR m_ViewUp;
		DirectX::XMVECTOR m_ViewRight;
		DirectX::XMVECTOR m_ViewDir;
		DirectX::XMMATRIX m_ViewMat;
		DirectX::XMMATRIX m_ProjMat;
		DirectX::XMMATRIX m_ViewProjMat;
		DirectX::XMMATRIX m_UnknownMat1;
		DirectX::XMMATRIX m_ViewProjMatrixUnjittered;
		DirectX::XMMATRIX m_PreviousViewProjMatrixUnjittered;
		DirectX::XMMATRIX m_ProjMatrixUnjittered;
		DirectX::XMMATRIX m_UnknownMat2;
		float m_ViewPort[4];// NiRect<float> { left = 0, right = 1, top = 1, bottom = 0 }
		RE::NiPoint2 m_ViewDepthRange;
		char _pad0[0x8];
	};

	struct CameraStateData
	{
		RE::NiCamera* pReferenceCamera;
		ViewData CamViewData;
		RE::NiPoint3 PosAdjust;
		RE::NiPoint3 CurrentPosAdjust;
		RE::NiPoint3 PreviousPosAdjust;
		bool UseJitter;
		char _pad0[0x8];
	};

	bool inPrecip = false;

	struct Hooks
	{
		struct Main_Precipitation_RenderOcclusion
		{
			static void thunk()
			{
				if (auto sky = RE::Sky::GetSingleton()) {
					if (auto precip = sky->precip) {
						State::GetSingleton()->BeginPerfEvent("SKYLIGHTING MASK");
						State::GetSingleton()->SetPerfMarker("SKYLIGHTING MASK");

						static float& PrecipitationShaderCubeSize = (*(float*)REL::RelocationID(515451, 515451).address());
						float originalPrecipitationShaderCubeSize = PrecipitationShaderCubeSize;

						static RE::NiPoint3& PrecipitationShaderDirection = (*(RE::NiPoint3*)REL::RelocationID(515509, 515509).address());
						RE::NiPoint3 originalParticleShaderDirection = PrecipitationShaderDirection;

						GetSingleton()->inPrecip = true;
						PrecipitationShaderCubeSize = 5000;
						PrecipitationShaderDirection = { 0, 0, -1 };
						Precipitation_SetupMask(precip);
						BSParticleShaderRainEmitter* rain = new BSParticleShaderRainEmitter;
						Precipitation_RenderMask(precip, rain);
						GetSingleton()->inPrecip = false;
						RE::BSParticleShaderCubeEmitter* cube = (RE::BSParticleShaderCubeEmitter*)rain;
						Skylighting::GetSingleton()->viewProjMat = cube->occlusionProjection;
						//Skylighting::GetSingleton()->Bind();

						cube = nullptr;
						delete rain;

						PrecipitationShaderCubeSize = originalPrecipitationShaderCubeSize;
						PrecipitationShaderDirection = originalParticleShaderDirection;


						State::GetSingleton()->EndPerfEvent();
					}
				}
				State::GetSingleton()->BeginPerfEvent("PRECIPITATION MASK");
				State::GetSingleton()->SetPerfMarker("PRECIPITATION MASK");
				func();
				State::GetSingleton()->EndPerfEvent();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderWorld
		{
			static void thunk(bool a1)
			{
				GetSingleton()->inWorld = true;
				func(a1);
			}

			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderWorld_Start
		{
			static void thunk(RE::BSBatchRenderer* This, uint32_t StartRange, uint32_t EndRanges, uint32_t RenderFlags, int GeometryGroup)
			{
				// Here is where the first opaque objects start rendering
				GetSingleton()->OverrideBlendStates();
				GetSingleton()->StartDeferred();
				func(This, StartRange, EndRanges, RenderFlags, GeometryGroup);  // RenderBatches
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderWorld_End
		{
			static void thunk(RE::BSShaderAccumulator* This, uint32_t RenderFlags)
			{
				func(This, RenderFlags);
				// After this point, water starts rendering
				GetSingleton()->ResetBlendStates();
				GetSingleton()->EndDeferred();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		//struct UpdateCameraData_UpdateBuffer
		//{
		//	static void thunk(RE::BSGraphics::State* This, CameraStateData* a_cameraStateData, RE::NiRect<float> a_rect, char a_flags)
		//	{
		//		if (GetSingleton()->inPrecip)
		//		{
		//			Skylighting::GetSingleton()->viewProjMat = a_cameraStateData->CamViewData.m_ViewProjMat;
		//		}
		//		func(This, a_cameraStateData, a_rect, a_flags);
		//	}
		//	static inline REL::Relocation<decltype(thunk)> func;
		//};

		//struct UpdateCameraData_BuildCameraData
		//{
		//	static void thunk(RE::BSGraphics::State* This, CameraStateData* a_cameraStateData, RE::NiCamera* a_camera, bool a_jitter)
		//	{
		//		func(This, a_cameraStateData, a_camera, a_jitter);
		//		if (GetSingleton()->inPrecip) {
		//			Skylighting::GetSingleton()->viewProjMat = a_cameraStateData->CamViewData.m_ViewProjMat;
		//		}
		//	}
		//	static inline REL::Relocation<decltype(thunk)> func;
		//};

		static void Install()
		{
			stl::write_thunk_call<Main_Precipitation_RenderOcclusion>(REL::RelocationID(35560, 36559).address() + REL::Relocate(0x3A1, 0x841, 0x791));

			stl::write_thunk_call<Main_RenderWorld>(REL::RelocationID(35560, 36559).address() + REL::Relocate(0x831, 0x841, 0x791));

			stl::write_thunk_call<Main_RenderWorld_Start>(REL::RelocationID(99938, 106583).address() + REL::Relocate(0x8E, 0x84));
			stl::write_thunk_call<Main_RenderWorld_End>(REL::RelocationID(99938, 106583).address() + REL::Relocate(0x319, 0x308, 0x321));
			
		//	stl::write_thunk_call<UpdateCameraData_BuildCameraData>(REL::RelocationID(75694, 106583).address() + REL::Relocate(0xCD, 0x308, 0x321));

			logger::info("[Deferred] Installed hooks");
		}
	};
};