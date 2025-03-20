#pragma once

#include "Buffer.h"
#include "State.h"

#include <d3d11_4.h>
#include <d3d12.h>

#include <xess/xess.h>
#include <xess/xess_d3d12.h>

class XeSS
{
public:
	static XeSS* GetSingleton()
	{
		static XeSS singleton;
		return &singleton;
	}

	inline std::string GetShortName() { return "XeSS"; }

	xess_context_handle_t m_xessContext = nullptr;

	xess_2d_t m_desiredOutputResolution;

	winrt::com_ptr<ID3D12Resource> m_xessOutput[2];

	winrt::com_ptr<ID3D12GraphicsCommandList4> commandList;

	void Init();

	void Upscale(Texture2D* a_color, Texture2D* a_alphaMask, float2 a_jitter);
};
