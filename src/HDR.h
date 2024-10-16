#pragma once

#include "Buffer.h"

#include "State.h"
#include "Util.h"

#include "Deferred.h"

#include "Menu.h"

class HDR
{
public:
	static HDR* GetSingleton()
	{
		static HDR singleton;
		return &singleton;
	}

	bool enabled = true;

	bool QueryHDRSupport();

	Texture2D* uiTexture = nullptr;
	Texture2D* hdrTexture = nullptr;
	Texture2D* outputTexture = nullptr;

	ID3D11ComputeShader* uiBlendCS = nullptr;
	ID3D11ComputeShader* GetUIBlendCS();

	ID3D11ComputeShader* hdrOutputCS = nullptr;
	ID3D11ComputeShader* GetHDROutputCS();

	ID3D11Resource* swapChainResource;

	void SetupResources();
	void CheckSwapchain();
	void ClearShaderCache();

	void HDROutput();

	RE::BSGraphics::RenderTargetData framebufferData;

	struct MenuManagerDrawInterfaceStartHook
	{
		static void thunk(int64_t a1)
		{
			auto hdr = GetSingleton();
			if (!hdr->enabled)
				return func(a1);

			static auto renderer = RE::BSGraphics::Renderer::GetSingleton();

			auto& data = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];

			hdr->framebufferData = data;

			data.RTV = hdr->uiTexture->rtv.get();

			static auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
			GET_INSTANCE_MEMBER(stateUpdateFlags, shadowState)

			stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

			func(a1);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct MenuManagerDrawInterfaceEndHook
	{
		static void thunk(int64_t a1)
		{
			Menu::GetSingleton()->DrawOverlay();

			auto hdr = GetSingleton();
			if (!hdr->enabled)
				return func(a1);

			auto renderer = RE::BSGraphics::Renderer::GetSingleton();

			renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER] = hdr->framebufferData;

			auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
			GET_INSTANCE_MEMBER(stateUpdateFlags, shadowState)

			stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

			func(a1);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct RenderMenuImagespace
	{
		static void thunk(uint64_t a1, uint32_t a3, uint32_t er8_0)
		{
			auto hdr = GetSingleton();
			if (!hdr->enabled)
				return func(a1, a3, er8_0);

			auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
			GET_INSTANCE_MEMBER(stateUpdateFlags, shadowState)

			auto renderer = RE::BSGraphics::Renderer::GetSingleton();
			auto& data = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];

			data = hdr->framebufferData;
			stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

			func(a1, a3, er8_0);

			data.RTV = hdr->uiTexture->rtv.get();
			stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSGraphics_Begin_Unk
	{
		static void thunk(int64_t a1, int64_t a2)
		{
			func(a1, a2);
			auto hdr = GetSingleton();
			if (!hdr->enabled)
				return;
			hdr->CheckSwapchain();
			auto renderer = RE::BSGraphics::Renderer::GetSingleton();
			auto& swapChain = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
			swapChain.SRV = hdr->hdrTexture->srv.get();
			swapChain.RTV = hdr->hdrTexture->rtv.get();
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSGraphics_End_Unk
	{
		static void thunk(int32_t a1)
		{
			GetSingleton()->HDROutput();
			func(a1);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void InstallHooks()
	{
		stl::write_thunk_call<MenuManagerDrawInterfaceStartHook>(REL::RelocationID(79947, 82084).address() + REL::Relocate(0x7E, 0x83, 0x17F));
		stl::write_thunk_call<MenuManagerDrawInterfaceEndHook>(REL::RelocationID(79947, 82084).address() + REL::Relocate(0x277, 0x2EA, 0x17F));
		stl::write_thunk_call<RenderMenuImagespace>(REL::RelocationID(51855, 52727).address() + REL::Relocate(0x7A1, 0x7A4, 0x17F));
		stl::write_thunk_call<BSGraphics_Begin_Unk>(REL::RelocationID(75460, 52727).address() + REL::Relocate(0x1D3, 0x7A4, 0x17F));
		stl::write_thunk_call<BSGraphics_End_Unk>(REL::RelocationID(75461, 52727).address() + REL::Relocate(0x9, 0x7A4, 0x17F));
	}
};