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

	ID3D11ComputeShader* hdrOutputCS = nullptr;

	void SetupResources();
	void ClearShaderCache();

	ID3D11ComputeShader* GetHDROutputCS();

	void HDROutput();

	RE::BSGraphics::RenderTargetData framebuffer;

	struct MenuManagerDrawInterfaceStartHook
	{
		static void thunk(int64_t a1)
		{
			auto hdr = GetSingleton();
			if (!hdr->enabled)
				return func(a1);

			auto renderer = RE::BSGraphics::Renderer::GetSingleton();

			auto& data = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];

			hdr->framebuffer = data;

			data.RTV = GetSingleton()->uiTexture->rtv.get();

			auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
			GET_INSTANCE_MEMBER(stateUpdateFlags, shadowState)

			stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

			State::GetSingleton()->context->OMSetRenderTargets(1, &data.RTV, nullptr);

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

			renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER] = GetSingleton()->framebuffer;

			auto shadowState = RE::BSGraphics::RendererShadowState::GetSingleton();
			GET_INSTANCE_MEMBER(stateUpdateFlags, shadowState)

			stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

			GetSingleton()->HDROutput();

			State::GetSingleton()->context->OMSetRenderTargets(1, &renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER].RTV, nullptr);

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

			data = GetSingleton()->framebuffer;
			stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);

			func(a1, a3, er8_0);

			data.RTV = GetSingleton()->uiTexture->rtv.get();
			stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void InstallHooks()
	{
		stl::write_thunk_call<MenuManagerDrawInterfaceStartHook>(REL::RelocationID(79947, 82084).address() + REL::Relocate(0x7E, 0x83, 0x17F));
		stl::write_thunk_call<MenuManagerDrawInterfaceEndHook>(REL::RelocationID(79947, 82084).address() + REL::Relocate(0x277, 0x2EA, 0x17F));
		stl::write_thunk_call<RenderMenuImagespace>(REL::RelocationID(51855, 52727).address() + REL::Relocate(0x7A1, 0x7A4, 0x17F));
	}
};