#include "ShaderReflect.h"

#include <algorithm>

#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <winrt/base.h>

#include "Hooks.h"  // TryGetShaderBytecode

namespace vanilla
{
	std::uint64_t ShaderUsage::HashUsedCB(int slot, const std::uint8_t* data, std::size_t len) const
	{
		if (slot < 0 || slot >= static_cast<int>(cb.size()) || !cb[slot].present)
			return 0ULL;  // unused slot: fixed sentinel so equal-unused compares equal
		const auto& u = cb[slot];
		std::uint64_t h = 0xcbf29ce484222325ULL;
		// Fold the slot index in so an empty used-range set still distinguishes slots.
		h ^= static_cast<std::uint64_t>(slot) + 1u;
		h *= 0x100000001b3ULL;
		if (!data)
			return h;
		for (const auto& r : u.used) {
			const std::uint32_t begin = r.begin;
			const std::uint32_t end = std::min<std::uint32_t>(r.end, static_cast<std::uint32_t>(len));
			for (std::uint32_t i = begin; i < end; ++i) {
				h ^= data[i];
				h *= 0x100000001b3ULL;
			}
		}
		return h;
	}

	ShaderReflect* ShaderReflect::GetSingleton()
	{
		static ShaderReflect singleton;
		return &singleton;
	}

	bool ShaderReflect::WantsCapture()
	{
		static const bool on = [] {
			char b[8]{};
			return GetEnvironmentVariableA("CS_RE_REFLECT", b, sizeof(b)) && b[0] && b[0] != '0';
		}();
		return on;
	}

	// Merge overlapping/adjacent byte ranges in place (sorted by begin).
	static void MergeRanges(std::vector<ShaderUsage::Range>& r)
	{
		if (r.size() < 2)
			return;
		std::sort(r.begin(), r.end(), [](const auto& a, const auto& b) { return a.begin < b.begin; });
		std::size_t w = 0;
		for (std::size_t i = 1; i < r.size(); ++i) {
			if (r[i].begin <= r[w].end) {
				r[w].end = std::max(r[w].end, r[i].end);
			} else {
				r[++w] = r[i];
			}
		}
		r.resize(w + 1);
	}

	ShaderUsage ShaderReflect::Reflect(const void* a_dxbc, std::size_t a_len)
	{
		ShaderUsage u;
		winrt::com_ptr<ID3D11ShaderReflection> refl;
		if (FAILED(D3DReflect(a_dxbc, a_len, IID_PPV_ARGS(refl.put()))) || !refl)
			return u;  // valid stays false

		D3D11_SHADER_DESC sd{};
		if (FAILED(refl->GetDesc(&sd)))
			return u;

		for (UINT i = 0; i < sd.BoundResources; ++i) {
			D3D11_SHADER_INPUT_BIND_DESC bd{};
			if (FAILED(refl->GetResourceBindingDesc(i, &bd)))
				continue;
			switch (bd.Type) {
			case D3D_SIT_CBUFFER:
				{
					const UINT slot = bd.BindPoint;
					if (slot < u.cb.size())
						u.usedCBMask |= (1u << slot);
					auto* rcb = refl->GetConstantBufferByName(bd.Name);
					if (!rcb || slot >= u.cb.size())
						break;
					D3D11_SHADER_BUFFER_DESC bufDesc{};
					if (FAILED(rcb->GetDesc(&bufDesc)))
						break;
					auto& cbu = u.cb[slot];
					cbu.present = true;
					cbu.sizeBytes = bufDesc.Size;
					for (UINT v = 0; v < bufDesc.Variables; ++v) {
						auto* var = rcb->GetVariableByIndex(v);
						if (!var)
							continue;
						D3D11_SHADER_VARIABLE_DESC vd{};
						if (FAILED(var->GetDesc(&vd)))
							continue;
						// Only bytes the shader actually references. FXC sets D3D_SVF_USED per variable.
						if (vd.uFlags & D3D_SVF_USED)
							cbu.used.push_back({ vd.StartOffset, vd.StartOffset + vd.Size });
					}
					MergeRanges(cbu.used);
				}
				break;
			case D3D_SIT_TEXTURE:
			case D3D_SIT_STRUCTURED:
			case D3D_SIT_BYTEADDRESS:
			case D3D_SIT_TBUFFER:
				if (bd.BindPoint < 32)
					u.usedSRVMask |= (1u << bd.BindPoint);
				break;
			case D3D_SIT_SAMPLER:
				if (bd.BindPoint < 32)
					u.usedSamplerMask |= (1u << bd.BindPoint);
				break;
			default:
				break;  // UAVs etc. are not part of a shadow DrawIndexed; ignore
			}
		}

		u.valid = true;
		return u;
	}

	const ShaderUsage* ShaderReflect::Get(void* a_shaderPtr)
	{
		if (!a_shaderPtr)
			return nullptr;
		std::lock_guard lock(mutex);
		if (auto it = cache.find(a_shaderPtr); it != cache.end())
			return &it->second;

		const std::uint8_t* dxbc = nullptr;
		std::size_t         len = 0;
		ShaderUsage         u;  // valid=false if bytecode unavailable
		if (TryGetShaderBytecode(a_shaderPtr, dxbc, len) && dxbc && len)
			u = Reflect(dxbc, len);
		auto [it, _] = cache.emplace(a_shaderPtr, std::move(u));
		return &it->second;
	}
}
