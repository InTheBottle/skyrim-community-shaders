#include "UtilityPassReplica.h"

#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/D3D.h"
#include "DrawState.h"

#include <DirectXPackedVector.h>
#include <algorithm>
#include <chrono>
#include <immintrin.h>
#include <intrin.h>
#include <mutex>
#include <unordered_map>
#include <winrt/base.h>

#include <RE/B/BSRenderPass.h>
#include <RE/B/BSShader.h>
#include <RE/B/BSUtilityShader.h>

// The replicated setup functions transcribe float math (alpha-test refs, split distances, world-
// matrix transforms) that Skyrim's 2021 SSE2 build computes as SEPARATE mul then add (two IEEE
// roundings). This project compiles with /arch:AVX2, under which MSVC contracts `a*b+c` into a
// single-rounded FMA -- which diverges from the engine by 1 ULP and false-fails the byte-exact
// parity gate (observed on the SetupGeometry alpha-test ref: engine 0x3E34B4B2 vs FMA 0x3E34B4B3).
// Disabling FP contraction for this TU makes every replicated multiply-add round exactly like the
// engine, so the recorded constant-buffer bytes match bit-for-bit.
#pragma fp_contract(off)

namespace
{
	using Recorded = UtilityPassReplica::RecordedCall;
	using Kind = UtilityPassReplica::RecordedCall::Kind;

	// Active recording sink (render thread only; null = recorder disarmed and every
	// hook below is a single-branch passthrough).
	std::vector<Recorded>* g_sink = nullptr;

	// DIAGNOSTIC: when CS_UTIL_RE_DUMP is set (to any non-zero value), snapshot each recorded CB
	// Map's written dwords onto the RecordedCall so DiffWindows can report the exact engine-vs-
	// replica dword for a content-hash divergence -- the rerunnable "find the deviating field" tool.
	std::uint32_t g_dumpMapSize = 0;

	// WRITE_DISCARD maps opened inside the current window: mapped pointer + size, so the
	// matching Unmap can hash the bytes the caller wrote. The utility window maps at most
	// a few buffers (geometry CB, technique CB, dynamic VB), so a tiny fixed table does.
	struct OpenMap
	{
		ID3D11Resource* resource = nullptr;
		const void*     data = nullptr;
		std::uint32_t   size = 0;
	};
	std::array<OpenMap, 8> g_openMaps{};

	// CommunityShaders.dll address range. The baseline captures only GAME-ENGINE-originated
	// D3D11 calls: CS features hook the engine's draw leaves and inject their own CBs/binds
	// (e.g. a per-object 64B CB from CommunityShaders.dll+0xAD56F). Those are CS overlays,
	// not the engine command stream we're replicating, so a call whose return address lands
	// inside our own module is not recorded. Set once at Setup.
	std::uintptr_t g_csBase = 0;
	std::uintptr_t g_csEnd = 0;

	// Set while EITHER window records: drop D3D11 calls whose return address lands inside
	// the CS module. CS features hook the engine's shader-bind and draw leaves (e.g. the
	// BeginTechnique VS/PS write-thunks in Hooks.cpp, per-object CB injection) and issue
	// D3D11 calls from CS code on the REAL render -- those are overlays on top of the
	// engine command stream and fire identically in both windows, so symmetric filtering
	// keeps the diff exact. The replica's own engine-equivalent calls (engine helpers,
	// vfuncs, and its hand-coded draw leaf) are routed through an executable stub OUTSIDE
	// the module image (see EngineCall) so their return addresses do not match the filter;
	// engine-internal TAIL-CALLED binds also inherit the stub's return address, which is
	// what makes the symmetric filter sound (a direct call from CS would mis-attribute
	// them as CS injections).
	bool g_filterCs = false;

	// The out-of-module call stub. Forwards the four register args untouched plus the
	// 5th/6th STACK args (IASetVertexBuffers takes six) into a fresh frame, then calls
	// the target from the slot at +0x28. Compare mode is single-threaded (owner gate),
	// so one slot suffices.
	std::uint8_t* g_stub = nullptr;
	void**        g_stubSlot = nullptr;
	bool          g_stubActive = false;

	void BuildEngineCallStub()
	{
		auto* mem = static_cast<std::uint8_t*>(
			VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		if (!mem)
			return;
		static constexpr std::uint8_t kStub[] = {
			0x48, 0x83, 0xEC, 0x38,              // +0x00 sub  rsp, 38h
			0x48, 0x8B, 0x44, 0x24, 0x60,        // +0x04 mov  rax, [rsp+60h]   (caller 5th arg)
			0x48, 0x89, 0x44, 0x24, 0x20,        // +0x09 mov  [rsp+20h], rax
			0x48, 0x8B, 0x44, 0x24, 0x68,        // +0x0E mov  rax, [rsp+68h]   (caller 6th arg)
			0x48, 0x89, 0x44, 0x24, 0x28,        // +0x13 mov  [rsp+28h], rax
			0xFF, 0x15, 0x0A, 0x00, 0x00, 0x00,  // +0x18 call qword ptr [rip+0Ah]  (slot @ +0x28)
			0x48, 0x83, 0xC4, 0x38,              // +0x1E add  rsp, 38h
			0xC3,                                // +0x22 ret
			0xCC, 0xCC, 0xCC, 0xCC, 0xCC,        // +0x23 pad to +0x28
		};
		static_assert(sizeof(kStub) == 0x28);
		std::memcpy(mem, kStub, sizeof(kStub));
		g_stub = mem;
		g_stubSlot = reinterpret_cast<void**>(mem + 0x28);
	}

	// Call an engine function (or D3D11 vfunc) the way the engine would: when the replica
	// window is recording, indirect through the out-of-module stub so downstream
	// tail-called D3D11 binds carry a non-CS return address; otherwise a direct call.
	// The stub forwards up to six args (four register + two stack).
	template <class R, class... Args>
	inline R EngineCall(const void* a_fn, Args... a_args)
	{
		using Fn = R (*)(Args...);
		if (g_stubActive && g_stub) {
			*g_stubSlot = const_cast<void*>(a_fn);
			return reinterpret_cast<Fn>(g_stub)(a_args...);
		}
		return reinterpret_cast<Fn>(const_cast<void*>(a_fn))(a_args...);
	}

	// Vtable-indexed EngineCall.
	template <std::size_t IDX, class R, class T, class... Args>
	inline R EngineCallV(T* a_obj, Args... a_args)
	{
		auto* vt = *reinterpret_cast<void** const*>(a_obj);
		return EngineCall<R>(vt[IDX], static_cast<void*>(a_obj), a_args...);
	}

	// Unsupported-reason tally (diagnostic): which CanReplicate gate sends a pass to the
	// engine fallback. [0]=no-geom [1]=skinned [2]=custom [3]=non-trishape [4]=no-rd [5]=stencil.
	std::atomic<std::uint64_t> g_unsupReason[6]{};

	// GeometryType histogram (geom+0x150) of fallback passes outside the covered
	// {TRISHAPE, SUB_INDEX_TRISHAPE} set, so remaining coverage work targets what
	// actually occurs in-game.
	std::atomic<std::uint64_t> g_geomTypeHist[16]{};

	inline bool EngineCaller(const void* a_ret)
	{
		const auto a = reinterpret_cast<std::uintptr_t>(a_ret);
		return a < g_csBase || a >= g_csEnd;  // true = not inside CommunityShaders.dll
	}

	inline void Record(Kind a_kind, std::uint16_t a_slot, std::uint64_t a_a, std::uint64_t a_b = 0, std::uint64_t a_c = 0)
	{
		if (g_sink)
			g_sink->push_back(Recorded{ a_kind, a_slot, a_a, a_b, a_c });
	}

	inline std::uint64_t HashBytes(const void* a_data, std::uint32_t a_size)
	{
		// FNV-1a 64. The comparison only needs equality, not cryptographic strength.
		const auto*   p = static_cast<const std::uint8_t*>(a_data);
		std::uint64_t h = 0xcbf29ce484222325ull;
		for (std::uint32_t i = 0; i < a_size; ++i) {
			h ^= p[i];
			h *= 0x100000001b3ull;
		}
		return h;
	}

	inline std::uint64_t HashPointers(ID3D11Buffer* const* a_ptrs, UINT a_count)
	{
		return HashBytes(a_ptrs, static_cast<std::uint32_t>(a_count * sizeof(void*)));
	}

	// ---------------------------------------------------------------------------
	// Immediate-context vtable detours. Indices are the standard ID3D11DeviceContext
	// layout (Map=14/Unmap=15).
	// Every thunk records iff a window is armed, then forwards.
	// ---------------------------------------------------------------------------

	struct VSSetConstantBuffers_Hook  // vfunc 7
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, UINT slot, UINT n, ID3D11Buffer* const* bufs)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kVSSetConstantBuffers, static_cast<std::uint16_t>(slot), n, HashPointers(bufs, n));
			func(ctx, slot, n, bufs);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PSSetShaderResources_Hook  // vfunc 8
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, UINT slot, UINT n, ID3D11ShaderResourceView* const* views)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kPSSetShaderResources, static_cast<std::uint16_t>(slot), n,
					HashBytes(views, static_cast<std::uint32_t>(n * sizeof(void*))));
			func(ctx, slot, n, views);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PSSetShader_Hook  // vfunc 9
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps, ID3D11ClassInstance* const* inst, UINT n)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kPSSetShader, 0, reinterpret_cast<std::uint64_t>(ps));
			func(ctx, ps, inst, n);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PSSetSamplers_Hook  // vfunc 10
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, UINT slot, UINT n, ID3D11SamplerState* const* samplers)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kPSSetSamplers, static_cast<std::uint16_t>(slot), n,
					HashBytes(samplers, static_cast<std::uint32_t>(n * sizeof(void*))));
			func(ctx, slot, n, samplers);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct VSSetShader_Hook  // vfunc 11
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs, ID3D11ClassInstance* const* inst, UINT n)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kVSSetShader, 0, reinterpret_cast<std::uint64_t>(vs));
			func(ctx, vs, inst, n);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct DrawIndexed_Hook  // vfunc 12
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kDrawIndexed, 0, indexCount, startIndex, static_cast<std::uint64_t>(static_cast<std::int64_t>(baseVertex)));
			// Regime-B draw-state fingerprint (no-op unless the shadow driver armed capture on this thread).
			vanilla::DrawStateValidator::GetSingleton()->OnDrawIndexed(ctx, indexCount, startIndex, baseVertex);
			func(ctx, indexCount, startIndex, baseVertex);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Map_Hook  // vfunc 14
	{
		static HRESULT __stdcall thunk(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub, D3D11_MAP mapType, UINT flags, D3D11_MAPPED_SUBRESOURCE* mapped)
		{
			const HRESULT hr = func(ctx, res, sub, mapType, flags, mapped);
			if (g_sink && SUCCEEDED(hr) && mapped && mapped->pData &&
				(mapType == D3D11_MAP_WRITE_DISCARD || mapType == D3D11_MAP_WRITE_NO_OVERWRITE)) {
				// Zero the mapped window (cap = hash window) so UNWRITTEN bytes hash
				// identically in both windows. The engine leaves flag-gated constants
				// unwritten in freshly renamed DISCARD memory -- stale ring garbage that
				// differs between the engine and replica maps and false-diffs the content
				// hash. The GPU-visible change is benign: those constants were undefined
				// garbage anyway (the shader permutation doesn't read them).
				if (mapType == D3D11_MAP_WRITE_DISCARD && mapped->RowPitch && mapped->RowPitch <= 4096)
					std::memset(mapped->pData, 0, mapped->RowPitch);
				if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress()))) {
					for (auto& slotEntry : g_openMaps) {
						if (!slotEntry.resource) {
							slotEntry = OpenMap{ res, mapped->pData, mapped->RowPitch };
							break;
						}
					}
				}
			}
			return hr;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Unmap_Hook  // vfunc 15
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub)
		{
			if (g_sink) {
				for (auto& slotEntry : g_openMaps) {
					if (slotEntry.resource == res) {
						// Hash what the caller wrote between Map and Unmap: this is the
						// "same data" half of command equality for constant buffers and
						// dynamic vertex data. Cap the hash window to keep compare mode
						// cheap; CBs are <= 4 KB, dynamic VB chunks can be larger but
						// their leading bytes diverge immediately when wrong.
						const std::uint32_t n = std::min<std::uint32_t>(slotEntry.size ? slotEntry.size : 256u, 4096u);
						if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress()))) {
							Record(Kind::kMapDiscardData, 0, reinterpret_cast<std::uint64_t>(res), n, HashBytes(slotEntry.data, n));
							// When dumping is enabled, snapshot the written dwords onto the record so
							// DiffWindows can pinpoint the exact diverging dword for any diverging Map.
							if (g_dumpMapSize && g_sink && !g_sink->empty()) {
								const auto* p = static_cast<const std::uint32_t*>(slotEntry.data);
								g_sink->back().mapData = std::make_shared<std::vector<std::uint32_t>>(p, p + n / 4);
							}
						}
						slotEntry = OpenMap{};
						break;
					}
				}
			}
			func(ctx, res, sub);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PSSetConstantBuffers_Hook  // vfunc 16
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, UINT slot, UINT n, ID3D11Buffer* const* bufs)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kPSSetConstantBuffers, static_cast<std::uint16_t>(slot), n, HashPointers(bufs, n));
			func(ctx, slot, n, bufs);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct IASetInputLayout_Hook  // vfunc 17
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, ID3D11InputLayout* layout)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kIASetInputLayout, 0, reinterpret_cast<std::uint64_t>(layout));
			func(ctx, layout);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct IASetVertexBuffers_Hook  // vfunc 18
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, UINT slot, UINT n, ID3D11Buffer* const* bufs, const UINT* strides, const UINT* offsets)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kIASetVertexBuffers, static_cast<std::uint16_t>(slot), HashPointers(bufs, n),
					HashBytes(strides, n * 4u), HashBytes(offsets, n * 4u));
			func(ctx, slot, n, bufs, strides, offsets);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct IASetIndexBuffer_Hook  // vfunc 19
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, ID3D11Buffer* buf, DXGI_FORMAT fmt, UINT offset)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kIASetIndexBuffer, 0, reinterpret_cast<std::uint64_t>(buf), fmt, offset);
			func(ctx, buf, fmt, offset);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct IASetPrimitiveTopology_Hook  // vfunc 24
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, D3D11_PRIMITIVE_TOPOLOGY topo)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kIASetPrimitiveTopology, 0, topo);
			func(ctx, topo);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct VSSetShaderResources_Hook  // vfunc 25
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, UINT slot, UINT n, ID3D11ShaderResourceView* const* views)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kVSSetShaderResources, static_cast<std::uint16_t>(slot), n,
					HashBytes(views, static_cast<std::uint32_t>(n * sizeof(void*))));
			func(ctx, slot, n, views);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct VSSetSamplers_Hook  // vfunc 26
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, UINT slot, UINT n, ID3D11SamplerState* const* samplers)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kVSSetSamplers, static_cast<std::uint16_t>(slot), n,
					HashBytes(samplers, static_cast<std::uint32_t>(n * sizeof(void*))));
			func(ctx, slot, n, samplers);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct OMSetRenderTargets_Hook  // vfunc 33
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, UINT n, ID3D11RenderTargetView* const* rtvs, ID3D11DepthStencilView* dsv)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kOMSetRenderTargets, 0, n,
					HashBytes(rtvs, static_cast<std::uint32_t>(n * sizeof(void*))), reinterpret_cast<std::uint64_t>(dsv));
			func(ctx, n, rtvs, dsv);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct OMSetBlendState_Hook  // vfunc 35
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, ID3D11BlendState* state, const FLOAT blendFactor[4], UINT sampleMask)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kOMSetBlendState, 0, reinterpret_cast<std::uint64_t>(state),
					blendFactor ? HashBytes(blendFactor, 16) : 0, sampleMask);
			func(ctx, state, blendFactor, sampleMask);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct OMSetDepthStencilState_Hook  // vfunc 36
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, ID3D11DepthStencilState* state, UINT stencilRef)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kOMSetDepthStencilState, 0, reinterpret_cast<std::uint64_t>(state), stencilRef);
			func(ctx, state, stencilRef);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct RSSetState_Hook  // vfunc 43
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, ID3D11RasterizerState* state)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kRSSetState, 0, reinterpret_cast<std::uint64_t>(state));
			func(ctx, state);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct RSSetViewports_Hook  // vfunc 44
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, UINT n, const D3D11_VIEWPORT* viewports)
		{
			if (g_sink && (!g_filterCs || EngineCaller(_ReturnAddress())))
				Record(Kind::kRSSetViewports, 0, n, viewports ? HashBytes(viewports, n * sizeof(D3D11_VIEWPORT)) : 0);
			func(ctx, n, viewports);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// ---------------------------------------------------------------------------
	// RenderPassImmediately detour (SE REL::ID 100854, 1.5.97 0x141308440).
	// ---------------------------------------------------------------------------
	struct RenderPassImmediately_Hook
	{
		static void thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
		{
			UtilityPassReplica::GetSingleton()->OnRenderPassImmediately(a_pass, a_technique, a_alphaTest, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;

		static void Engine(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
		{
			func(a_pass, a_technique, a_alphaTest, a_renderFlags);
		}
	};

	const char* KindName(Kind a_kind)
	{
		switch (a_kind) {
		case Kind::kVSSetShader: return "VSSetShader";
		case Kind::kPSSetShader: return "PSSetShader";
		case Kind::kVSSetConstantBuffers: return "VSSetConstantBuffers";
		case Kind::kPSSetConstantBuffers: return "PSSetConstantBuffers";
		case Kind::kVSSetShaderResources: return "VSSetShaderResources";
		case Kind::kPSSetShaderResources: return "PSSetShaderResources";
		case Kind::kVSSetSamplers: return "VSSetSamplers";
		case Kind::kPSSetSamplers: return "PSSetSamplers";
		case Kind::kIASetInputLayout: return "IASetInputLayout";
		case Kind::kIASetVertexBuffers: return "IASetVertexBuffers";
		case Kind::kIASetIndexBuffer: return "IASetIndexBuffer";
		case Kind::kIASetPrimitiveTopology: return "IASetPrimitiveTopology";
		case Kind::kOMSetRenderTargets: return "OMSetRenderTargets";
		case Kind::kOMSetBlendState: return "OMSetBlendState";
		case Kind::kOMSetDepthStencilState: return "OMSetDepthStencilState";
		case Kind::kRSSetState: return "RSSetState";
		case Kind::kRSSetViewports: return "RSSetViewports";
		case Kind::kMapDiscardData: return "MapDiscardData";
		case Kind::kDrawIndexed: return "DrawIndexed";
		}
		return "?";
	}
}

// ---------------------------------------------------------------------------------
// 1.5.97 engine internals used by the replica. Every address is from the IDA dossier
// (docs/development/utility-pass-re-dossier.md); "S" = RendererShadowState 0x143027EB0,
// "G" = BSGraphics globals 0x143025EF0. All engine-call fallbacks are STAGE-scoped:
// each stage of the RE replaces one with a replicated body, compare-validated.
// ---------------------------------------------------------------------------------
namespace engine
{
	// Batch-renderer cross-pass caches (written by the walk, shared with engine passes).
	inline REL::Relocation<std::uint32_t*> g_currentTechnique{ REL::Offset(0x3283BA4) };
	inline REL::Relocation<RE::BSShader**> g_currentShader{ REL::Offset(0x3283BA8) };
	inline REL::Relocation<void**>         g_currentMaterial{ REL::Offset(0x3490BB0) };
	inline REL::Relocation<std::uint32_t*> g_debugTechnique{ REL::Offset(0x1E0DF8C) };  // write-only
	inline REL::Relocation<std::uint8_t*>  g_useEarlyZ{ REL::Offset(0x302C8E5) };
	inline REL::Relocation<void**>         g_skyShaderInstance{ REL::Offset(0x32336C0) };

	// RendererShadowState (S) fields the replicated DrawTriShape touches.
	inline REL::Relocation<std::uint32_t*> S_stateUpdateFlags{ REL::Offset(0x3027EB0) };
	inline REL::Relocation<std::uint64_t*> S_vertexDesc{ REL::Offset(0x30281F0) };       // S+0x340
	inline REL::Relocation<std::uint32_t*> S_topology{ REL::Offset(0x3028208) };         // S+0x358
	// Snapshot span for compare mode: S+0x00 .. S+0x5D8 covers every field the utility
	// setup path reads or writes (dirty words, RT/depth/raster/blend modes, sampler and
	// SRV caches, vertex desc, current shaders, topology, camera data, blend-extra).
	inline REL::Relocation<std::uint8_t*> S_base{ REL::Offset(0x3027EB0) };
	constexpr std::uint32_t               kSnapshotBytes = 0x5D8;
	// The engine's "current context" pointer slot (state-pool[926] == 0x143027EA0). BSGraphics::
	// Renderer::SetShader/SetPixelShader bind VS/PS on *this* slot, so temporarily swapping it to a
	// worker's deferred context routes BeginTechnique's binds onto the worker's command list.
	inline REL::Relocation<ID3D11DeviceContext**> g_engineCtxSlot{ REL::Offset(0x3027EA0) };
	// BSBatchRenderer::sub_141307DD0 -- advances the batch-group iterator; returns whether more
	// groups remain. BeginPassReplica calls it verbatim (pure iterator logic, no D3D11).
	inline REL::Relocation<std::uint8_t (*)(void*, void*, void*, void*)> BatchAdvance{ REL::Offset(0x1307DD0) };
	inline REL::Relocation<std::uint8_t*> g_beginPassFlagE5D{ REL::Offset(0x31D0E5D) };  // unk_1431D0E5D

	// Cross-pass shadow token written by SetupGeometry (0x14130EC70) and read+reset by
	// RestoreTechnique (0x141310300) to decide the alpha-blend dirty bit. It lives well
	// outside the RendererShadowState span, so the compare harness must snapshot it
	// separately or the replica's RestoreTechnique reads the engine window's leftover
	// value and sets a spurious OMSetBlendState.
	inline REL::Relocation<std::uint32_t*> g_shadowGeomToken{ REL::Offset(0x1E10660) };

	// BSGraphics::Renderer singleton (first arg of the draw leaves).
	inline REL::Relocation<std::uint8_t*> g_renderer{ REL::Offset(0x3028490) };

	// Bone-palette CB ring cursor (0x143027A00, values 0..3): GetID3D11Resource
	// (0x140D6FFD0) hands out one of four 3840-byte dynamic CBs (0x143027A08..20) per
	// bone upload and advances this cursor. The compare harness must restore it before
	// the replica window or the double-render binds DIFFERENT (content-identical) ring
	// CBs and every skinned pass false-diffs on the buffer pointer.
	inline REL::Relocation<std::uint32_t*> g_boneCBRingCursor{ REL::Offset(0x3027A00) };

	// Dynamic-VB ring state (0x143025F30, one qword: LO = ring-buffer index 0..2, HI =
	// byte offset into the 4MB buffer). FUN_140D6C8A0 allocates skinned dynamic-shape
	// slices here; restoring it lets the replica re-map the same slice (rewriting
	// byte-identical data under WRITE_NO_OVERWRITE) so the recorded bind offsets match.
	inline REL::Relocation<std::uint64_t*> g_dynVBRingState{ REL::Offset(0x3025F30) };

	// Engine TLS index (0x143497408): the render path stamps per-thread markers into the
	// module TLS block -- +1896 is the memory-context tag the standard path sets to 26,
	// +10752 is the accumulator the skinned dispatcher zeroes (FUN_14131f7c0).
	inline REL::Relocation<std::uint32_t*> g_tlsIndex{ REL::Offset(0x3497408) };
	inline std::uint8_t* TlsBlock()
	{
		auto* tlsArray = reinterpret_cast<std::uint8_t**>(__readgsqword(0x58));
		return tlsArray[*g_tlsIndex];
	}

	// BSSubIndexTriShape (GeometryType 8) helpers: segment-coalesce pass (CPU-only state
	// maintenance, 0x140D59430) and the "always draw whole shape" global (0x1430243B0).
	using SubIndexPreDraw_t = void (*)(void*);
	inline REL::Relocation<SubIndexPreDraw_t> SubIndexPreDraw{ REL::Offset(0xD59430) };
	inline REL::Relocation<std::uint8_t*>     g_subIndexWholeDraw{ REL::Offset(0x30243B0) };

	// Skinned-path dynamic-data upload (BSDynamicTriShape under a skin, e.g. faces):
	// map a slice of the shared dynamic ring (out: ring offset), lock/fetch the CPU-side
	// dynamic vertex data, copy, unmap, unlock. All engine helpers, addresses verified
	// against the dispatcher disasm at 0x141308A05.
	using MapSkinDyn_t = void* (*)(void*, std::uint32_t, std::int32_t*);
	inline REL::Relocation<MapSkinDyn_t> MapSkinDynamicData{ REL::Offset(0xD6C8A0) };  // 0x140D6C8A0
	using UnmapSkinDyn_t = void (*)(void*, void*);
	inline REL::Relocation<UnmapSkinDyn_t> UnmapSkinDynamicData{ REL::Offset(0xD6C9E0) };  // 0x140D6C9E0
	using DynShapeLock_t = void* (*)(void*);
	inline REL::Relocation<DynShapeLock_t> DynShapeLockData{ REL::Offset(0xC723C0) };  // 0x140C723C0
	using DynShapeUnlock_t = void (*)(void*);
	inline REL::Relocation<DynShapeUnlock_t> DynShapeUnlock{ REL::Offset(0xC72420) };  // 0x140C72420

	// The draw-struct the skinned dispatcher builds on its stack and hands to the
	// skin-instance Render vfunc (37). Layout verified against the disasm at
	// 0x141308A05 (stack frame rsp+0x30..0x6D).
	struct SkinDrawStruct
	{
		void*         boneSetter;    // +0x00  shader+0x10 (NiBoneMatrixSetterI) or null
		void*         geometry;      // +0x08
		std::uint64_t unk10;         // +0x10  = 0
		std::int32_t  singleLevel;   // +0x18  (pass+0x1E >> 7) & 1
		std::int32_t  lodIndex;      // +0x1C  pass+0x1E & 0x7F
		float         unk20;         // +0x20  = 0.0f
		std::int32_t  dynOffset[6];  // +0x24  [0] = -1; dynamic-ring offset out-param
		std::uint16_t boneMode;      // +0x3C  = 1 only on the bone-setter branch
		std::uint16_t pad3E;
	};
	static_assert(sizeof(SkinDrawStruct) == 0x40);
	static_assert(offsetof(SkinDrawStruct, dynOffset) == 0x24);
	static_assert(offsetof(SkinDrawStruct, boneMode) == 0x3C);

	// Engine helpers still called in Stage A (replaced in later stages).
	using SetDirtyStates_t = void (*)(bool);
	inline REL::Relocation<SetDirtyStates_t> SetDirtyStates{ REL::Offset(0xD705B0) };

	// --- SetDirtyStates reimplementation support (FlushDirtyStatesReplica) ---
	// The state-object pool is the qword array AT 0x1430261B0 (ctx = pool[926], alpha CB =
	// pool[783]); the RT/DSV pool base is the pointer STORED AT 0x143025F00 (disasm-
	// verified). Both are read-only shared. The 0x5D8 render-state block is passed in.
	inline REL::Relocation<std::uint8_t*> g_statePool{ REL::Offset(0x30261B0) };   // qword array base
	inline REL::Relocation<std::uint8_t*> g_rtPoolPtr{ REL::Offset(0x3025F00) };   // *(u8**) = RT pool base
	inline REL::Relocation<float*>        g_slopeBiasTable{ REL::Offset(0x3026180) };  // unk_143026180
	inline REL::Relocation<const void*>   g_blendFactor{ REL::Offset(0x1E07168) };  // unk_141E07168 (4 floats)
	// Input-layout cache (BSTScatterTable at 0x141E07140; node stride 24: key@0, IL*@+8,
	// next@+16). Read walk is lock-free + thread-safe while nobody inserts/grows. In
	// compare mode the engine window pre-warms this SAME pass, so the replica walk hits;
	// the create/insert fallback (engine helpers) is faithful for the rare standalone miss.
	inline REL::Relocation<std::uint8_t*>  g_ilTable{ REL::Offset(0x1E07140) };     // table struct
	inline REL::Relocation<std::uint32_t*> g_ilCapacity{ REL::Offset(0x1E07144) };  // +4; mask = cap-1
	inline REL::Relocation<std::uintptr_t*> g_ilBase{ REL::Offset(0x1E07160) };     // +0x20 bucket base
	inline REL::Relocation<std::uintptr_t> g_ilSentinel{ REL::Offset(0x1E07150) };  // +0x10 end-of-chain
	using ILHash_t = void (*)(std::uint64_t*, std::uint64_t);
	inline REL::Relocation<ILHash_t> ILHash{ REL::Offset(0xC06570) };               // pure CRC hash
	using ILCreate_t = void* (*)(std::uint64_t);
	inline REL::Relocation<ILCreate_t> ILCreate{ REL::Offset(0xD70F90) };           // create ID3D11InputLayout
	using ILInsert_t = bool (*)(void*, std::uintptr_t, std::uint32_t, std::uint64_t*, void**);
	inline REL::Relocation<ILInsert_t> ILInsert{ REL::Offset(0xD730E0) };           // scatter-table insert
	using ILGrow_t = void (*)(void*);
	inline REL::Relocation<ILGrow_t> ILGrow{ REL::Offset(0xD73F70) };               // grow/rehash

	// --- Per-pass setup reimplementation support (FlushSetupTechnique/Material/GeometryReplica).
	//     These reimplement BSUtilityShader::SetupTechnique/Material/Geometry ctx-parameterized
	//     so a worker can fill+bind its OWN PerTechnique/Material/Geometry CBs on its own context.
	//     The CBs are owned by the current VS/PS shader objects (*0x1430281F8 / *0x143028200 =
	//     block+0x348 / +0x350); at N=1 the shared shader CBs are passed for byte-identical parity.
	inline REL::Relocation<std::uint32_t*> g_utilMaterialFillIndex{ REL::Offset(0x1E0DFF0) };  // dword_141E0DFF0
	// SetupTechnique helpers + data
	inline REL::Relocation<std::uint32_t (*)(std::uint32_t)> UtilVSIndex{ REL::Offset(0x1334900) };
	inline REL::Relocation<std::uint32_t (*)(std::uint32_t)> UtilPSIndex{ REL::Offset(0x1334970) };
	inline REL::Relocation<bool (*)(RE::BSShader*, std::uint32_t, std::uint32_t, bool)> BeginTechnique{ REL::Offset(0x131FBD0) };
	inline REL::Relocation<std::uint32_t (*)(void*)> RTGetWidth{ REL::Offset(0xD74C20) };
	inline REL::Relocation<std::uint32_t (*)(void*)> RTGetHeight{ REL::Offset(0xD74C60) };
	inline REL::Relocation<std::int32_t (*)()>       GetDepthStencilTargetMain{ REL::Offset(0xD74E50) };
	inline REL::Relocation<float (*)(float, float)>  PowF{ REL::Offset(0x134BEAC) };
	inline REL::Relocation<std::uint8_t*>  g_mainRTDesc{ REL::Offset(0x302BB20) };
	inline REL::Relocation<float*>         g_utilDepthConst{ REL::Offset(0x1E0DF04) };
	inline REL::Relocation<std::uint8_t*>  g_dsvDirty{ REL::Offset(0x30284C2) };          // OUT of 0x5D8 block
	// DS-target table base (unk_14302A4D0 = g_renderer+0x2040); entry i (the depth resource ptr of
	// target i) is at +152*i. RestoreTechnique compares the bound-RTV cache (S+0x150) against it.
	inline REL::Relocation<std::uint8_t*>  g_dsTargetTable{ REL::Offset(0x302A4D0) };
	inline REL::Relocation<std::uint8_t*>  g_focusShadowEnable{ REL::Offset(0x1E0DE43) };
	inline REL::Relocation<std::uint32_t*> g_focusShadowCount{ REL::Offset(0x31D0FB8) };
	inline REL::Relocation<std::uint8_t**> g_viewCamera{ REL::Offset(0x31D0E68) };
	inline REL::Relocation<std::uint8_t**> g_shadowSceneNode{ REL::Offset(0x1E0DED0) };
	inline REL::Relocation<std::uint32_t*> g_shadowFixedCount{ REL::Offset(0x1867188) };
	inline REL::Relocation<std::uint8_t*>  g_copySplitToVS{ REL::Offset(0x1E0DE4C) };
	inline REL::Relocation<std::uint32_t*> g_shadowMode{ REL::Offset(0x1E0DE34) };
	inline REL::Relocation<std::uint32_t*> g_poissonDenom{ REL::Offset(0x3283B90) };
	inline REL::Relocation<float*>         g_poissonRadiusScale{ REL::Offset(0x1E10670) };
	inline REL::Relocation<float*>         g_fixedSplit{ REL::Offset(0x3283B78) };
	inline REL::Relocation<float*>         g_maxFocusDist{ REL::Offset(0x1E106B8) };
	inline REL::Relocation<float**>        g_focusShadowData{ REL::Offset(0x31D0FA8) };
	inline REL::Relocation<float*>         g_fadeFracStart{ REL::Offset(0x1E106A0) };
	inline REL::Relocation<float*>         g_shadowRadius{ REL::Offset(0x1E10B78) };
	inline REL::Relocation<float*>         g_shadowSign{ REL::Offset(0x1E10B7C) };
	inline REL::Relocation<float*>         g_biasBase{ REL::Offset(0x3283B7C) };
	// SetupGeometry helpers + data
	inline REL::Relocation<void*> SG_BuildMatrix{ REL::Offset(0x12C3440) };
	inline REL::Relocation<void*> SG_MatrixTranspose{ REL::Offset(0x134C1DC) };
	inline REL::Relocation<void*> SG_Vec3TransformCoord{ REL::Offset(0x134C206) };
	inline REL::Relocation<void*> SG_ShadowFill{ REL::Offset(0x130F960) };
	inline REL::Relocation<void*> SG_SetupShadowLightParams{ REL::Offset(0x130FBE0) };
	inline REL::Relocation<void*> SG_ScissorFromBBox{ REL::Offset(0xD70100) };
	inline REL::Relocation<void*> SG_ScissorApply{ REL::Offset(0xD6FCF0) };
	inline REL::Relocation<void*> SG_WorldToView{ REL::Offset(0xD42C50) };
	inline REL::Relocation<void*> SG_GetAccumulator{ REL::Offset(0x12966A0) };
	inline REL::Relocation<std::uintptr_t> SG_pCamNode{ REL::Offset(0x31D0F88) };
	inline REL::Relocation<std::uintptr_t> SG_pViewFrustumObj{ REL::Offset(0x31D0E68) };
	inline REL::Relocation<std::uintptr_t> SG_pFadeExclude{ REL::Offset(0x31D0DA8) };
	inline REL::Relocation<std::uintptr_t> SG_mode1D0E28{ REL::Offset(0x31D0E28) };
	inline REL::Relocation<std::uintptr_t> SG_flagDE4C{ REL::Offset(0x1E0DE4C) };
	inline REL::Relocation<std::uintptr_t> SG_modeDF94{ REL::Offset(0x1E0DF94) };
	inline REL::Relocation<std::uintptr_t> SG_windFadeMin{ REL::Offset(0x1E0DF70) };
	inline REL::Relocation<std::uintptr_t> SG_windFadeMax{ REL::Offset(0x1E0DF74) };
	inline REL::Relocation<std::uintptr_t> SG_stencilVal014{ REL::Offset(0x1E0E014) };
	inline REL::Relocation<std::uintptr_t> SG_c283B88{ REL::Offset(0x3283B88) };
	inline REL::Relocation<std::uintptr_t> SG_c283B7C{ REL::Offset(0x3283B7C) };
	inline REL::Relocation<std::uintptr_t> SG_recip127{ REL::Offset(0x156302C) };
	inline REL::Relocation<std::uintptr_t> SG_recip255{ REL::Offset(0x1540648) };
	inline REL::Relocation<std::uintptr_t> SG_alphaBias{ REL::Offset(0x1866724) };

	using GetNiProperty_t = RE::NiAlphaProperty* (*)(RE::BSRenderPass*);
	inline REL::Relocation<GetNiProperty_t> GetNiProperty{ REL::Offset(0x12FD8A0) };
	using GrassShadowBlacklist_t = bool (*)(std::uint32_t);
	inline REL::Relocation<GrassShadowBlacklist_t> IsGrassShadowBlacklist{ REL::Offset(0x12CCE20) };
	using SetupGeomAlphaBlend_t = void (*)(RE::BSShader*, RE::NiAlphaProperty*, RE::BSShaderProperty*, bool);
	inline REL::Relocation<SetupGeomAlphaBlend_t> SetupGeometryAlphaBlending{ REL::Offset(0x131F440) };
	using SetupAlphaTestRef_t = void (*)(RE::BSShader*, RE::NiAlphaProperty*, RE::BSShaderProperty*);
	inline REL::Relocation<SetupAlphaTestRef_t> SetupAlphaTestRef{ REL::Offset(0x131F2A0) };

	// BSGraphics::TriShape (QRendererData): +0 vertexBuffer, +8 indexBuffer, +0x10 vertexDesc.
	struct TriShapeData
	{
		ID3D11Buffer* vertexBuffer;
		ID3D11Buffer* indexBuffer;
		std::uint64_t vertexDesc;
	};
}

namespace
{
	// Per-worker private render state for the multithreaded shadow recorder. A worker
	// thread records one shadow map from ITS OWN state; each field points at a private
	// buffer/cache/context. At N=1 (single-threaded validation) t_worker is null and the
	// accessors below fall through to the engine globals, so the parity gate stays green
	// while the plumbing is proven. Set t_worker for the duration of a worker's map record.
	struct ShadowWorkerState
	{
		std::uint8_t*        block = nullptr;      // 0x5D8 render-state block (FlushDirtyStates)
		ID3D11DeviceContext* ctx = nullptr;        // deferred context to record into
		std::uint32_t*       technique = nullptr;  // g_currentTechnique cache
		RE::BSShader**       shader = nullptr;     // g_currentShader cache
		void**               material = nullptr;   // g_currentMaterial cache
		std::uint32_t*       shadowToken = nullptr;  // g_shadowGeomToken (dword_141E10660) cache
		std::uint32_t*       techFlags = nullptr;  // shader+0x90 (technique flags) -- shared singleton scratch
		std::uint32_t*       techSub = nullptr;    // shader+0x94 (technique & 0x7F)
		std::uint8_t*        dsvDirty = nullptr;   // g_dsvDirty (0x1430284C2), OUT-of-block DSV-rebind flag
	};
	thread_local ShadowWorkerState* t_worker = nullptr;

	// BeginPass-level command/orchestration compare (kOwnBeginPassVerify). During a compare, one run
	// of the engine's BeginPass and one of BeginPassReplica each append the (pass,key,alpha,flags)
	// they dispatch to this sink -- the engine passes via OnRenderPassImmediately, the replica passes
	// via BeginPassReplica's loop. Comparing the two sequences + the block delta + the group-advance
	// proves the reimplemented orchestration matches (per-pass DX11 is already byte-exact). Non-null
	// only for the duration of one captured run; null on the normal render path (zero overhead).
	struct VerifyPassRec
	{
		const void*   pass;
		std::uint32_t tech;
		std::uint8_t  alpha;
		std::uint32_t flags;
	};
	thread_local std::vector<VerifyPassRec>* t_bpSeqSink = nullptr;
	std::uint64_t                            g_bpCompareGroups = 0;    // pure-covered groups compared
	std::uint64_t                            g_bpCompareDiverged = 0;  // of those, how many diverged

	// Accessors: return the worker's private state when a worker is active, else the engine
	// global (identical object at N=1). The replica render path reads/writes through these
	// so the exact same code drives the render-thread compare and a worker's private record.
	// (Skinned passes' bone-CB / dyn-VB rings are NOT yet worker-private -- static-TRISHAPE
	// maps thread first; skinned stays on the serial remainder until those rings are split.)
	inline std::uint8_t*        WsBlock() { return t_worker ? t_worker->block : reinterpret_cast<std::uint8_t*>(engine::S_base.address()); }
	inline ID3D11DeviceContext* WsCtx() { return t_worker ? t_worker->ctx : globals::d3d::context; }
	inline std::uint32_t&       WsTechnique() { return t_worker ? *t_worker->technique : *engine::g_currentTechnique; }
	inline RE::BSShader*&       WsShader() { return t_worker ? *t_worker->shader : *engine::g_currentShader; }
	inline void*&               WsMaterial() { return t_worker ? *t_worker->material : *reinterpret_cast<void**>(engine::g_currentMaterial.address()); }
	inline std::uint32_t&       WsShadowToken() { return t_worker ? *t_worker->shadowToken : *engine::g_shadowGeomToken; }
	// Technique flags live on the SHARED singleton BSUtilityShader (shader+0x90/0x94); a worker reads
	// its OWN cell so concurrent per-pass technique stamps don't tear. At N=1 the cell is null so the
	// read falls through to the shared object -- identical to before, byte-exact.
	inline std::uint32_t        WsTechFlags(std::uint8_t* a_shader) { return t_worker ? *t_worker->techFlags : *reinterpret_cast<std::uint32_t*>(a_shader + 0x90); }
	inline std::uint8_t&        WsDsvDirty() { return t_worker ? *t_worker->dsvDirty : *engine::g_dsvDirty; }
}

// Bring up the recorder/replica infrastructure (module range, engine-call stub, RenderPassImmediately
// detour + immediate-context recorder vfuncs) exactly once. Idempotent and mode-independent: the
// ShadowThreaded BeginPass-ownership modes need the RenderPassImmediately trampoline (for the uncovered
// engine fallback) and the OnRenderPassImmediately detour (to observe the engine's dispatch) even when
// CS_UTIL_RE_MODE is off, so they call this directly. With mode kOff, OnRenderPassImmediately is a thin
// passthrough for the rest of the game.
void UtilityPassReplica::EnsureInitialized()
{
	if (hooksInstalled)
		return;

	// Establish our own module range so the recorder can exclude CS-originated calls.
	{
		HMODULE mod = nullptr;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&EngineCaller), &mod) &&
			mod) {
			g_csBase = reinterpret_cast<std::uintptr_t>(mod);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(mod);
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(g_csBase + dos->e_lfanew);
			g_csEnd = g_csBase + nt->OptionalHeader.SizeOfImage;
		}
	}

	BuildEngineCallStub();

	engineWindow.reserve(64);
	replicaWindow.reserve(64);
	InstallHooks();
}

void UtilityPassReplica::Setup()
{
	char buf[8] = {};
	if (GetEnvironmentVariableA("CS_UTIL_RE_MODE", buf, sizeof(buf)) && buf[0]) {
		const int v = atoi(buf);
		if (v >= 0 && v <= 2)
			mode.store(static_cast<Mode>(v), std::memory_order_relaxed);
	}
	if (!IsActive())
		return;

	EnsureInitialized();
	logger::info("[UtilityPassReplica] active, mode={} ({})", static_cast<std::uint32_t>(GetMode()),
		GetMode() == Mode::kCompare ? "compare" : "replace");
}

void UtilityPassReplica::InstallHooks()
{
	if (hooksInstalled)
		return;
	// SE-only: every RE'd offset below is 1.5.97.
	if (!REL::Module::IsSE()) {
		logger::info("[UtilityPassReplica] SE-only; not installing on this runtime");
		return;
	}
	auto* context = globals::d3d::context;
	if (!context) {
		logger::warn("[UtilityPassReplica] no immediate context at setup; hooks not installed");
		return;
	}

	stl::detour_thunk<RenderPassImmediately_Hook>(REL::RelocationID(100854, 107644));

	stl::detour_vfunc<7, VSSetConstantBuffers_Hook>(context);
	stl::detour_vfunc<8, PSSetShaderResources_Hook>(context);
	stl::detour_vfunc<9, PSSetShader_Hook>(context);
	stl::detour_vfunc<10, PSSetSamplers_Hook>(context);
	stl::detour_vfunc<11, VSSetShader_Hook>(context);
	stl::detour_vfunc<12, DrawIndexed_Hook>(context);
	stl::detour_vfunc<14, Map_Hook>(context);
	stl::detour_vfunc<15, Unmap_Hook>(context);
	stl::detour_vfunc<16, PSSetConstantBuffers_Hook>(context);
	stl::detour_vfunc<17, IASetInputLayout_Hook>(context);
	stl::detour_vfunc<18, IASetVertexBuffers_Hook>(context);
	stl::detour_vfunc<19, IASetIndexBuffer_Hook>(context);
	stl::detour_vfunc<24, IASetPrimitiveTopology_Hook>(context);
	stl::detour_vfunc<25, VSSetShaderResources_Hook>(context);
	stl::detour_vfunc<26, VSSetSamplers_Hook>(context);
	stl::detour_vfunc<33, OMSetRenderTargets_Hook>(context);
	stl::detour_vfunc<35, OMSetBlendState_Hook>(context);
	stl::detour_vfunc<36, OMSetDepthStencilState_Hook>(context);
	stl::detour_vfunc<43, RSSetState_Hook>(context);
	stl::detour_vfunc<44, RSSetViewports_Hook>(context);

	hooksInstalled = true;
	logger::info("[UtilityPassReplica] installed RenderPassImmediately detour + 20 context recorder hooks");
}

void UtilityPassReplica::BeginWindow(std::vector<RecordedCall>& a_sink)
{
	a_sink.clear();
	g_openMaps.fill({});
	g_sink = &a_sink;
	g_filterCs = true;
	static const bool s_dumpInit = [] {
		char buf[16] = {};
		if (GetEnvironmentVariableA("CS_UTIL_RE_DUMP", buf, sizeof(buf)))
			g_dumpMapSize = static_cast<std::uint32_t>(strtoul(buf, nullptr, 16));
		return true;
	}();
	(void)s_dumpInit;
}

void UtilityPassReplica::EndWindow()
{
	g_sink = nullptr;
	g_filterCs = false;
}

void UtilityPassReplica::OnRenderPassImmediately(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
{
	// BeginPass-compare engine run: the engine's BeginPass reaches every pass through this detour.
	// Record what it dispatched (to diff against BeginPassReplica's sequence) and render via the
	// real function -- no settle gate, no per-pass compare, no capture hook. This branch is active
	// only while BeginPassCompare has armed the sink; the normal render path never enters it.
	if (t_bpSeqSink) {
		t_bpSeqSink->push_back({ a_pass, a_technique, static_cast<std::uint8_t>(a_alphaTest), a_renderFlags });
		RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
		return;
	}

	// Only utility passes are in scope; everything else is always the engine's.
	const bool isUtility = a_pass && a_pass->shader &&
	                       a_pass->shader->shaderType.get() == RE::BSShader::Type::Utility;
	if (!isUtility) {
		RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
		return;
	}
	// When the UtilityPassReplica mode is off we still proceed IF a shadow-capture hook is armed
	// (ShadowThreaded arms it only during the shadow-map walk, so main-scene utility passes are
	// unaffected). This is the seam the draw-state/MT modes ride even without CS_UTIL_RE_MODE set.
	if (GetMode() == Mode::kOff && !shadowCaptureHook.load(std::memory_order_acquire)) {
		RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
		return;
	}

	// Only replicate in fully-SETTLED in-world rendering. Load-screen / main-menu 3D renders (the
	// rotating loading-screen model, the main-menu preview) AND the world-init window right after a
	// save load drive utility shadow passes with transiently-uninitialized shadow state -- the
	// focus-shadow array, the shadow camera node (*0x1431D0F88), the scene light list, and
	// per-shader constant buffers are all mid-init. Worse, the load->gameplay transition renders
	// utility passes from TWO threads (the loading-screen renderer vs the main render thread, see
	// the ownership note below): the loading-screen thread tears down/rebuilds the shadow camera
	// while the main-thread replica is mid-compare, so the camera/light pointers flip to garbage
	// BETWEEN the engine and replica windows. The engine tolerates all this by rendering each pass
	// once; the double-rendering replica re-reads the raced transient and faults. No instantaneous
	// flag is race-proof, so gate on the standard CS "in world" signal (player + parent cell + no
	// main/loading menu) AND require it to have held continuously for a short settle window, so the
	// transition race is fully past before the replica runs. Outside that, the engine draws as-is.
	auto* const player = RE::PlayerCharacter::GetSingleton();
	const bool  rawInWorld = player && player->GetParentCell() &&
	                     globals::state && !globals::state->IsMainOrLoadingMenuOpen();
	// Settle: proceed only after we've been CONTINUOUSLY in-world for kSettleMs. Track the first-in-world
	// timestamp and reset it whenever we leave the world (menu/load), so the load->gameplay transition
	// race is past before the replica double-renders. Unlike the old s_everUnsettled latch, this does NOT
	// require a prior !rawInWorld pass to have routed through here -- that latch could stay closed forever
	// when the boot menu never drives a utility pass to this detour (observed: the capture-hook modes
	// never settled because s_everUnsettled stayed false the whole session).
	static std::atomic<std::int64_t> s_inWorldSinceMs{ 0 };
	const std::int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
	                               std::chrono::steady_clock::now().time_since_epoch())
	                               .count();
	if (rawInWorld) {
		std::int64_t expected = 0;
		s_inWorldSinceMs.compare_exchange_strong(expected, nowMs, std::memory_order_relaxed);  // stamp first in-world frame
	} else {
		s_inWorldSinceMs.store(0, std::memory_order_relaxed);  // left world -> restart the settle window
	}
	constexpr std::int64_t kSettleMs = 750;
	const std::int64_t     inWorldSince = s_inWorldSinceMs.load(std::memory_order_relaxed);
	const bool             settled = rawInWorld && inWorldSince != 0 && (nowMs - inWorldSince) >= kSettleMs;
	if (!settled) {
		RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
		return;
	}

	// Shadow-capture fan-out (ShadowThreaded). While a shadow-map walk is being captured, offer
	// each utility pass to the hook; if it claims ownership (worker pool will replay it), skip the
	// inline render here entirely. Otherwise fall through to the normal per-mode path.
	if (auto hook = shadowCaptureHook.load(std::memory_order_acquire)) {
		if (hook(a_pass, a_technique, a_alphaTest, a_renderFlags, CanReplicate(a_pass)))
			return;
	}

	// The smoke test showed utility passes arriving from TWO threads (loading-screen
	// renderer vs main render thread). The recorder windows are single-threaded state,
	// so only one thread may compare at a time; a contender just renders via the engine.
	static std::atomic<std::uint32_t> s_compareOwner{ 0 };
	const std::uint32_t               tid = GetCurrentThreadId();
	if (GetMode() == Mode::kCompare) {
		std::uint32_t expected = 0;
		if (!s_compareOwner.compare_exchange_strong(expected, tid, std::memory_order_acquire) && expected != tid) {
			RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
			return;
		}
	}
	struct OwnerRelease
	{
		std::atomic<std::uint32_t>* owner;
		~OwnerRelease()
		{
			if (owner)
				owner->store(0, std::memory_order_release);
		}
	} ownerRelease{ GetMode() == Mode::kCompare ? &s_compareOwner : nullptr };

	// Outside current replica coverage: whole-pass engine render, tallied.
	if (!CanReplicate(a_pass)) {
		++passesUnsupported;
		RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
		return;
	}

	if (GetMode() == Mode::kCompare) {
		// Snapshot the cross-pass state the pass mutates (batch caches + the whole
		// RendererShadowState span) so the REPLICA window starts from the exact state
		// the ENGINE window started from -- otherwise the engine run warms the caches
		// and the replica legitimately emits fewer calls (cache hits), breaking equality.
		static std::vector<std::uint8_t> s_snapshot(engine::kSnapshotBytes);
		std::memcpy(s_snapshot.data(), engine::S_base.address() ? reinterpret_cast<void*>(engine::S_base.address()) : nullptr, engine::kSnapshotBytes);
		const auto savedTechnique = *engine::g_currentTechnique;
		auto* const savedShader = *engine::g_currentShader;
		auto* const savedMaterial = *engine::g_currentMaterial;

		// The engine window's SetupTechnique overwrites the current technique flag stored on
		// the shader object (BSShader+0x90). RestoreTechnique/SetupMaterial read that flag,
		// and the replica's RestoreTechnique reads it BEFORE its own SetupTechnique rewrites
		// it -- so it must see the pre-engine value, not the one the engine window left. This
		// field lives on the shader object, outside the RendererShadowState span, so snapshot
		// the outgoing and incoming shaders' flags explicitly.
		auto flagAt = [](void* s) -> std::uint32_t* {
			return s ? reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(s) + 0x90) : nullptr;
		};
		auto* const     outFlag = flagAt(savedShader);
		auto* const     inFlag = flagAt(a_pass->shader);
		const std::uint32_t savedOutFlag = outFlag ? *outFlag : 0u;
		const std::uint32_t savedInFlag = inFlag ? *inFlag : 0u;
		const std::uint32_t savedShadowGeomToken = *engine::g_shadowGeomToken;
		const std::uint32_t savedBoneCBRingCursor = *engine::g_boneCBRingCursor;
		const std::uint64_t savedDynVBRingState = *engine::g_dynVBRingState;

		// Regime-B fingerprint-soundness verify (opt-in via CS_RE_REFLECT): capture the engine's and the
		// replica's per-draw effective-state fingerprints for this pass and diff them. Byte-exact commands
		// MUST yield identical fingerprints -- this proves the fingerprint is deterministic + correctly
		// built before it is trusted to gate MT. The Get*/staging-read capture adds no records to the
		// command windows (Get*/CopyResource unhooked; MAP_READ ignored by the recorder).
		auto* const ds = vanilla::DrawStateValidator::GetSingleton();
		const bool  fpv = ds->FpVerifyActive();
		thread_local std::vector<vanilla::DrawFingerprint> s_fpE, s_fpR;
		if (fpv) {
			s_fpE.clear();
			ds->SetFingerprintSink(&s_fpE);
		}

		// Ground truth first: the engine renders and we record its command window
		// (BeginWindow arms the symmetric CS-injection filter).
		BeginWindow(engineWindow);
		RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
		EndWindow();
		if (fpv)
			ds->SetFingerprintSink(nullptr);

		// Restore pre-engine state, then the replica issues its own window from the
		// same starting point. Depth-only work is idempotent, so the double render is
		// visually harmless; the diff is the mechanical proof of command equality.
		std::memcpy(reinterpret_cast<void*>(engine::S_base.address()), s_snapshot.data(), engine::kSnapshotBytes);
		*engine::g_currentTechnique = savedTechnique;
		*engine::g_currentShader = savedShader;
		*engine::g_currentMaterial = savedMaterial;
		if (outFlag)
			*outFlag = savedOutFlag;
		if (inFlag)
			*inFlag = savedInFlag;
		*engine::g_shadowGeomToken = savedShadowGeomToken;
		// Re-mapping the same ring CBs is safe: WRITE_DISCARD renames, and the replica
		// writes byte-identical palettes.
		*engine::g_boneCBRingCursor = savedBoneCBRingCursor;
		*engine::g_dynVBRingState = savedDynVBRingState;

		// Replica window: same symmetric filter; the replica's engine-equivalent calls
		// go through the out-of-module stub (g_stubActive) so they and the engine's
		// tail-called binds survive the filter while CS-hook injections drop out
		// exactly as they did in the engine window.
		if (fpv) {
			s_fpR.clear();
			ds->SetFingerprintSink(&s_fpR);
		}
		g_stubActive = true;
		BeginWindow(replicaWindow);
		ReplicaRenderPassImmediately(a_pass, a_technique, a_alphaTest, a_renderFlags);
		EndWindow();
		g_stubActive = false;
		if (fpv)
			ds->SetFingerprintSink(nullptr);

		DiffWindows(a_pass, a_technique);
		if (fpv)
			ds->CompareFingerprints(s_fpE, s_fpR, a_pass, a_technique);
		return;
	}

	// Mode::kReplace: the engine is switched off for this pass. If we reached here in kOff (a covered
	// pass a capture hook declined to claim -- not expected, since the verify hook claims all covered
	// passes), fall back to the engine rather than replacing, so the hook-armed kOff seam never silently
	// swaps rendering.
	if (GetMode() == Mode::kReplace)
		ReplicaRenderPassImmediately(a_pass, a_technique, a_alphaTest, a_renderFlags);
	else
		RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
}


bool UtilityPassReplica::CanReplicate(RE::BSRenderPass* a_pass) const
{
	// Stage A coverage: unskinned, non-custom-render, plain TRISHAPE geometry. Anything
	// else takes the engine path whole-pass (never mid-pass) and is tallied for coverage.
	auto* geom = a_pass->geometry;
	if (!geom) {
		g_unsupReason[0].fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	const auto* geomBytes = reinterpret_cast<const std::uint8_t*>(geom);
	// STENCIL_ABOVE_WATER releases the bound PS on first use -- running it twice in
	// compare mode would double-Release. Excluded until the replica owns that path.
	const std::uint32_t f = a_pass->passEnum - 0x2B;
	if ((f & 0x1200) == 0x1200) {
		g_unsupReason[5].fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	if (*reinterpret_cast<void* const*>(geomBytes + 0x130)) {  // skin instance
		// Skinned coverage (Stage B): the static skin-instance Render branch. The
		// dynamic bone-setter branch (geometry vfunc 54 non-zero) routes into the full
		// Draw dispatch and stays whole-pass engine until that path is replicated.
		if (EngineCallV<54, std::uint64_t>(const_cast<RE::BSGeometry*>(geom)) != 0) {
			g_unsupReason[1].fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		return true;
	}
	if (geomBytes[0x109] & 8) {  // needs-custom-render
		g_unsupReason[2].fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	if (geomBytes[0x150] != 3 && geomBytes[0x150] != 8) {  // TRISHAPE / SUB_INDEX_TRISHAPE
		g_unsupReason[3].fetch_add(1, std::memory_order_relaxed);
		g_geomTypeHist[geomBytes[0x150] & 15].fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	if (!*reinterpret_cast<void* const*>(geomBytes + 0x138)) {  // rendererData
		g_unsupReason[4].fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	return true;
}

namespace
{
	// Forward declarations: the per-pass setup reimplementations are defined below (same TU /
	// anonymous namespace) but called from ReplicaRenderPassImmediately here.
	int  CsSetupMask();
	void FlushSetupMaterialReplica(ID3D11DeviceContext*, ID3D11Buffer*, ID3D11Buffer*, std::uint8_t*, std::uint8_t*, std::uint8_t*);
	bool FlushSetupTechniqueReplica(ID3D11DeviceContext*, ID3D11Buffer*, ID3D11Buffer*, std::uint8_t*, RE::BSShader*, std::int32_t);
	void FlushSetupGeometryReplica(ID3D11DeviceContext*, ID3D11Buffer*, ID3D11Buffer*, std::uint8_t*, RE::BSShader*, RE::BSRenderPass*);
	// Block-only helpers (no context/CB): reimplement the two engine alpha-state functions against
	// a caller-supplied block S. They hardcode the global block (0x143027EB0), so a worker needs
	// its OWN copy. Wired behind CsSetupMask bit 8; validated byte-exact via the parity gate at N=1.
	void SetupGeomAlphaBlendReplica(std::uint8_t* S, RE::NiAlphaProperty* a2, RE::BSShaderProperty* a3, bool a4);
	void SetAlphaTestRefReplica(std::uint8_t* S, RE::NiAlphaProperty* a2, RE::BSShaderProperty* a3);
	// Block-only reimpls of the two engine restore vfuncs (wired behind CsSetupMask bit 16). They
	// hardcode the global block + g_shadowGeomToken; a worker restores its OWN block/token cache so
	// per-pass change detection stays private. Validated byte-exact via the parity gate at N=1.
	void RestoreTechniqueReplica(std::uint8_t* S, std::uint8_t* shader, std::uint32_t a_technique);
	void RestoreGeometryReplica(std::uint8_t* S, std::uint8_t* shader, RE::BSRenderPass* a_pass);
}

void UtilityPassReplica::ReplicaRenderPassImmediately(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
{
	// ---- RenderPassImmediately body (1.5.97 0x141308440), replicated ----
	auto* shader = a_pass->shader;
	auto* geom = a_pass->geometry;

	// Technique cache: BeginPass only when the technique or shader changed. 0x5C006076
	// never caches (the engine forces re-setup for that sentinel technique). The caches
	// are per-worker (BeginPass zeroes them) -- read/write through the worker accessors so
	// a worker uses its OWN caches; at N=1 these alias the engine globals.
	const bool cached = WsTechnique() == a_technique &&
	                    a_technique != 0x5C006076 &&
	                    shader == WsShader();
	if (!cached) {
		*engine::g_debugTechnique = a_technique;
		// BeginPass (0x1413086C0): RestoreTechnique on the outgoing shader, clear the
		// caches, SetupTechnique on the incoming one, then re-stamp the caches.
		if (auto* prev = WsShader()) {
			if (CsSetupMask() & 16)
				RestoreTechniqueReplica(WsBlock(), reinterpret_cast<std::uint8_t*>(prev), WsTechnique());
			else
				EngineCallV<3, void>(prev, WsTechnique());  // RestoreTechnique
		}
		WsShader() = nullptr;
		WsTechnique() = 0;
		WsMaterial() = nullptr;
		if (CsSetupMask() & 2) {
			// PerTechnique CBs resolved inside (block+0x348/0x350 are only set by BeginTechnique).
			if (!FlushSetupTechniqueReplica(WsCtx(), nullptr, nullptr, WsBlock(),
					shader, static_cast<std::int32_t>(a_technique)))
				return;  // engine bails the whole pass on setup failure
		} else {
			if (!EngineCallV<2, bool>(shader, a_technique))  // SetupTechnique
				return;  // engine bails the whole pass on setup failure
		}
		WsShader() = shader;
		WsTechnique() = a_technique;
	}

	// Material change detection (cache at 0x143490BB0).
	void* material = a_pass->shaderProperty ? *reinterpret_cast<void* const*>(
												  reinterpret_cast<const std::uint8_t*>(a_pass->shaderProperty) + 0x78) :
	                                          nullptr;
	if (material != WsMaterial()) {
		if (material) {
			if (CsSetupMask() & 1) {
				auto* Sb = WsBlock();
				auto* vsSh = *reinterpret_cast<std::uint8_t**>(Sb + 0x348);
				auto* psSh = *reinterpret_cast<std::uint8_t**>(Sb + 0x350);
				FlushSetupMaterialReplica(WsCtx(),
					vsSh ? *reinterpret_cast<ID3D11Buffer**>(vsSh + 0x28) : nullptr,
					psSh ? *reinterpret_cast<ID3D11Buffer**>(psSh + 0x20) : nullptr,
					Sb, reinterpret_cast<std::uint8_t*>(shader), reinterpret_cast<std::uint8_t*>(material));
			} else {
				EngineCallV<4, void>(shader, material);  // SetupMaterial
			}
		}
		WsMaterial() = material;
	}


	// ucCurrentMeshLODLevel: the walk stamps the pass's LOD index onto the geometry.
	auto* geomBytes = reinterpret_cast<std::uint8_t*>(geom);
	geomBytes[0x108] = static_cast<std::uint8_t>(a_pass->LODMode.index);

	// ---- geometry dispatch (RenderPassImmediately tail, 1.5.97 0x1413084C5) ----
	if (*reinterpret_cast<void**>(geomBytes + 0x130)) {
		ReplicaRenderSkinned(a_pass, a_alphaTest, a_renderFlags);
		return;
	}

	// ---- _Standard path (0x1413088C0): TLS tag -> ShaderSetup -> Draw -> Restore ----
	// The standard path stamps memory-context tag 26 into the module TLS block for the
	// duration of the draw and restores the previous tag after (no D3D11 effect; kept
	// for faithful replication of engine-visible state).
	auto* const         tlsTag = reinterpret_cast<std::uint32_t*>(engine::TlsBlock() + 1896);
	const std::uint32_t savedTlsTag = *tlsTag;
	*tlsTag = 26;

	const bool alphaTest = a_alphaTest || *engine::g_useEarlyZ != 0;

	// ShaderSetup (0x141309F80): alpha-blend + alpha-test-ref setup, then SetupGeometry. The two
	// alpha helpers hardcode the global block; bit 8 routes them through the block-parameterized
	// reimplementations (WsBlock) so a worker mutates its OWN block.
	const bool alphaReimpl = (CsSetupMask() & 8) != 0;
	if (shader != *reinterpret_cast<RE::BSShader**>(engine::g_skyShaderInstance.address())) {
		if ((a_renderFlags & 4) && !EngineCall<bool>(reinterpret_cast<void*>(engine::IsGrassShadowBlacklist.address()), a_pass->passEnum)) {
			auto* alphaProp = EngineCall<RE::NiAlphaProperty*>(reinterpret_cast<void*>(engine::GetNiProperty.address()), a_pass);
			if (alphaReimpl)
				SetupGeomAlphaBlendReplica(WsBlock(), alphaProp, a_pass->shaderProperty, alphaTest);
			else
				EngineCall<void>(reinterpret_cast<void*>(engine::SetupGeometryAlphaBlending.address()), shader,
					alphaProp, a_pass->shaderProperty, alphaTest);
		}
		if (alphaTest) {
			if (auto* alphaProp = EngineCall<RE::NiAlphaProperty*>(reinterpret_cast<void*>(engine::GetNiProperty.address()), a_pass)) {
				if (alphaReimpl)
					SetAlphaTestRefReplica(WsBlock(), alphaProp, a_pass->shaderProperty);
				else
					EngineCall<void>(reinterpret_cast<void*>(engine::SetupAlphaTestRef.address()), shader, alphaProp, a_pass->shaderProperty);
			}
		}
	}
	if (CsSetupMask() & 4) {
		auto* Sg = WsBlock();
		auto* vsSh = *reinterpret_cast<std::uint8_t**>(Sg + 0x348);
		auto* psSh = *reinterpret_cast<std::uint8_t**>(Sg + 0x350);
		FlushSetupGeometryReplica(WsCtx(),
			vsSh ? *reinterpret_cast<ID3D11Buffer**>(vsSh + 0x38) : nullptr,
			psSh ? *reinterpret_cast<ID3D11Buffer**>(psSh + 0x30) : nullptr,
			Sg, shader, a_pass);
	} else {
		EngineCallV<6, void>(shader, a_pass, a_renderFlags);  // SetupGeometry
	}

	// ---- Draw dispatch (0x141307160) on GeometryType (geom+0x150) ----
	auto* rd = *reinterpret_cast<engine::TriShapeData**>(geomBytes + 0x138);
	const std::uint16_t wholeTriCount = *reinterpret_cast<const std::uint16_t*>(geomBytes + 0x158);

	switch (geomBytes[0x150]) {
	case 3:  // TRISHAPE -> DrawTriShape whole
		DrawTriShapeReplica(rd, 0, wholeTriCount);
		break;
	case 8:  // SUB_INDEX_TRISHAPE (case 7): segment-coalesce, then whole or per-segment
		EngineCall<void>(reinterpret_cast<void*>(engine::SubIndexPreDraw.address()), static_cast<void*>(geom));
		if (*engine::g_subIndexWholeDraw) {
			DrawTriShapeReplica(rd, 0, wholeTriCount);
		} else {
			// drawAll byte geom+0x171; active-segment count geom+0x168; segment array
			// geom+0x160, stride 0x14: +0x00 startIndex, +0x0C numTris, +0x10 enabled.
			const bool          drawAll = geomBytes[0x171] != 0;
			const std::uint32_t count = drawAll ? 1u : *reinterpret_cast<const std::uint32_t*>(geomBytes + 0x168);
			const auto*         seg = *reinterpret_cast<const std::uint8_t* const*>(geomBytes + 0x160);
			for (std::uint32_t i = 0; i < count; ++i, seg += 0x14) {
				if (!seg[0x10])
					continue;
				const std::uint32_t numTris = drawAll ? wholeTriCount : *reinterpret_cast<const std::uint32_t*>(seg + 0x0C);
				const std::uint32_t start = drawAll ? 0u : *reinterpret_cast<const std::uint32_t*>(seg + 0x00);
				DrawTriShapeReplica(rd, start, numTris);
			}
		}
		break;
	}

	if (CsSetupMask() & 16)
		RestoreGeometryReplica(WsBlock(), reinterpret_cast<std::uint8_t*>(shader), a_pass);
	else
		EngineCallV<7, void>(shader, a_pass, a_renderFlags);  // RestoreGeometry
	*tlsTag = savedTlsTag;
}

// BSGraphics::SetDirtyStates 0x140D705B0 reimplemented against a caller-supplied render-
// state block (S = the 0x5D8 block, 0x143027EB0 for N=1) + context. A worker thread needs
// this to flush its PRIVATE state to its OWN deferred context; the engine's version hard-
// codes the global block + global context (*(0x143027EA0)), which cannot be shared across
// threads. Byte-exact transcription (see docs/development/shadow-keystone-re.md): the
// state-object pool (array @0x1430261B0), RT pool (*(0x143025F00)), slope-bias table, blend
// factor and input-layout cache are read-only shared globals; context calls route through
// the out-of-module stub (EngineCallV) so the compare recorder captures them exactly as it
// captures the engine's. Main-word branches emit in the engine's order
// RT -> DS -> raster(+bias) -> viewport -> blend -> alphaCB -> IL -> topology (viewport
// AFTER raster because the bias branch re-raises the viewport bit), then the five per-stage
// mask loops. Validated by CS_UTIL_RE_CSFLUSH=1 + the parity gate showing 0 divergence.
namespace
{
	void FlushDirtyStatesReplica(std::uint8_t* S, ID3D11DeviceContext* ctx, bool a1)
	{
		const auto u16 = [&](std::size_t o) { return *reinterpret_cast<std::uint16_t*>(S + o); };
		const auto u32 = [&](std::size_t o) { return *reinterpret_cast<std::uint32_t*>(S + o); };
		const auto i32 = [&](std::size_t o) { return *reinterpret_cast<std::int32_t*>(S + o); };
		const auto f32 = [&](std::size_t o) { return *reinterpret_cast<float*>(S + o); };
		const auto u64 = [&](std::size_t o) { return *reinterpret_cast<std::uint64_t*>(S + o); };
		const auto setU32 = [&](std::size_t o, std::uint32_t v) { *reinterpret_cast<std::uint32_t*>(S + o) = v; };
		// dword_143027EBC[N] lives at S + 0x0C + 4N.
		const auto ebc = [&](int n) { return static_cast<std::size_t>(0x0C + 4 * n); };

		auto* pool = reinterpret_cast<std::uintptr_t*>(engine::g_statePool.address());  // qword array
		auto* rtBase = *reinterpret_cast<std::uint8_t**>(engine::g_rtPoolPtr.address());

		std::uint16_t dirty = u16(0);
		if (dirty == 0)
			goto perStage;

		// -- bit0 0x0001 render targets / DSV --
		if (dirty & 1) {
			std::uint32_t numViews = 0;
			std::uintptr_t rtvs[8] = {};
			if (i32(ebc(13)) == -1) {  // MRT
				for (numViews = 0; numViews < 8; ++numViews) {
					const std::int32_t idx = i32(0x18 + 4 * numViews);  // RT index[i]
					if (idx == -1)
						break;
					const std::uintptr_t rtv = *reinterpret_cast<std::uintptr_t*>(rtBase + 48 * idx + 0xA58);
					rtvs[numViews] = rtv;
					if (u32(0x48 + 4 * numViews) == 0) {  // clear-flag[i]
						EngineCallV<50, void>(ctx, reinterpret_cast<void*>(rtv), rtBase + 0x2768);  // ClearRTV
						setU32(0x48 + 4 * numViews, 4);
					}
				}
			} else {  // single DSV
				numViews = 1;
				const std::uintptr_t rtv = *reinterpret_cast<std::uintptr_t*>(
					rtBase + 8 * (static_cast<std::size_t>(i32(ebc(14))) + 8 * static_cast<std::size_t>(i32(ebc(13)))) + 0x26D0);
				rtvs[0] = rtv;
				if (u32(ebc(24)) == 0) {
					EngineCallV<50, void>(ctx, reinterpret_cast<void*>(rtv), rtBase + 0x2768);  // ClearRTV
					setU32(ebc(24), 4);
				}
			}
			// DSV clear (flag [23]); depth = [12]; stencil 0. rtBase+0x22 is the DSV byte.
			const std::uint32_t clearMode = u32(ebc(23));
			if (clearMode <= 2 || clearMode == 6)
				*reinterpret_cast<std::uint8_t*>(rtBase + 0x22) = 0;
			std::uintptr_t dsv = 0;
			if (i32(ebc(11)) != -1) {
				const std::size_t dsIdx = static_cast<std::size_t>(i32(ebc(12))) + 19 * static_cast<std::size_t>(i32(ebc(11)));
				dsv = *reinterpret_cast<std::uint8_t*>(rtBase + 0x22) ?
				          *reinterpret_cast<std::uintptr_t*>(rtBase + 8 * dsIdx + 0x1FF0) :
				          *reinterpret_cast<std::uintptr_t*>(rtBase + 8 * dsIdx + 0x1FB0);
				if (dsv) {
					std::int32_t flags = -1;
					switch (clearMode) {
					case 0: case 6: flags = 3; break;
					case 2: flags = 2; break;
					case 1: flags = 1; break;
					default: flags = -1; break;
					}
					if (flags != -1) {
						EngineCallV<53, void>(ctx, reinterpret_cast<void*>(dsv), flags, i32(ebc(12)), 0);  // ClearDSV
						setU32(ebc(23), 4);
					}
				}
			}
			EngineCallV<33, void>(ctx, numViews, rtvs, reinterpret_cast<void*>(dsv));  // OMSetRenderTargets
			dirty = u16(0);
		}

		// -- bits2,3 0x000C depth-stencil state --
		if (dirty & 0xC) {
			EngineCallV<36, void>(ctx,
				reinterpret_cast<void*>(pool[40 * u32(0x88) + u32(0x90)]),  // 40*F38 + F40
				u32(0x94));                                                  // ref F44
			dirty = u16(0);
		}

		// -- bits4,5,6,12 0x1070 rasterizer (+ depth bias) --
		if (dirty & 0x1070) {
			EngineCallV<43, void>(ctx,
				reinterpret_cast<void*>(pool[72 * u32(0x98) + 240 + 24 * u32(0x9C) + 2 * u32(0xA0) + u32(0xA4)]));  // RSSetState
			dirty = u16(0);
			if (dirty & 0x40) {
				// Target bias is flt_143028470[0]/[1] = S+0x5C0/0x5C4 (in-block); the slope
				// subtraction uses the shared table unk_143026180 (g_slopeBiasTable).
				float v14 = f32(0x84);  // [30]
				if (f32(0x80) != f32(0x5C0) || f32(0x84) != f32(0x5C4)) {
					v14 = f32(0x5C4);
					*reinterpret_cast<float*>(S + 0x80) = f32(0x5C0);
					*reinterpret_cast<float*>(S + 0x84) = f32(0x5C4);
					setU32(0, u32(0) | 2);
					dirty = u16(0);
				}
				const std::uint32_t slopeIdx = u32(0xA0);  // dword_143027F50
				if (slopeIdx != 0) {
					v14 = v14 - engine::g_slopeBiasTable.get()[slopeIdx];
					*reinterpret_cast<float*>(S + 0x84) = v14;
					setU32(0, u32(0) | 2);
					dirty = u16(0);
				}
			}
		}

		// -- bit1 0x0002 viewport (AFTER raster) --
		if (dirty & 2) {
			EngineCallV<44, void>(ctx, 1u, S + 0x70);  // RSSetViewports (&[25])
			dirty = u16(0);
		}

		// -- bit7 0x0080 blend --
		if (dirty & 0x80) {
			EngineCallV<35, void>(ctx,
				reinterpret_cast<void*>(pool[52 * u32(0xA8) + 384 + 26 * u32(0xAC) + 2 * u32(0xB0) + static_cast<std::int32_t>(f32(0x5D0))]),
				engine::g_blendFactor.get(), 0xFFFFFFFFu);  // OMSetBlendState
			dirty = u16(0);
		}

		// -- bits8,9 0x0300 alpha CB --
		if (dirty & 0x300) {
			D3D11_MAPPED_SUBRESOURCE mapped{};
			auto* alphaCB = reinterpret_cast<ID3D11Resource*>(pool[783]);
			EngineCallV<14, HRESULT>(ctx, alphaCB, 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped);  // Map
			if (mapped.pData)
				*reinterpret_cast<float*>(mapped.pData) = u32(0xB4) /*F64*/ ? f32(0xB8) /*F68*/ : 0.0f;
			EngineCallV<15, void>(ctx, alphaCB, 0u);  // Unmap
			dirty = u16(0);
		}

		// -- bit10 0x0400 input layout (only if a1==0) --
		if (!a1 && (dirty & 0x400)) {
			const std::uint64_t key = u64(0x340) & *reinterpret_cast<std::uint64_t*>(u64(0x348) + 72);
			std::uint64_t       hash = 0;
			engine::ILHash(&hash, key);
			void*                il = nullptr;
			const std::uintptr_t base = *engine::g_ilBase;
			bool                 found = false;
			if (base) {
				std::uintptr_t node = base + 24 * (hash & (*engine::g_ilCapacity - 1));
				if (*reinterpret_cast<std::uintptr_t*>(node + 16)) {  // occupied
					while (*reinterpret_cast<std::uint64_t*>(node) != key) {
						node = *reinterpret_cast<std::uintptr_t*>(node + 16);
						if (node == engine::g_ilSentinel.address())
							goto ilMiss;
					}
					il = *reinterpret_cast<void**>(node + 8);
					found = true;
				}
			}
		ilMiss:
			if (!found) {
				// Faithful engine fallback (only hit on a genuine standalone miss; in compare
				// mode the engine window pre-warmed this key). NOTE: mutates the shared cache
				// -- for threading, pre-warm so this path is never taken.
				il = engine::ILCreate(key);
				if (il || key != 0x300000000407ull) {
					std::uint64_t h2 = 0;
					engine::ILHash(&h2, key);
					std::uint64_t k2 = key;
					while (!engine::ILInsert(engine::g_ilTable.get(), *engine::g_ilBase, static_cast<std::uint32_t>(h2), &k2, &il))
						engine::ILGrow(engine::g_ilTable.get());
				}
			}
			if (t_worker) {
				static std::atomic<int> s_ilDbg{ 8 };
				if (s_ilDbg.fetch_sub(1, std::memory_order_relaxed) > 0)
					logger::warn("[ILDBG] worker key={:016X} found={} il={}", key, found, il);
			}
			EngineCallV<17, void>(ctx, il);  // IASetInputLayout
			dirty = u16(0);
		}

		// -- bit11 0x0800 topology --
		if (dirty & 0x800) {
			EngineCallV<24, void>(ctx, u32(0x358));  // IASetPrimitiveTopology (dword_143028070[102])
			dirty = u16(0);
		}

		// main-word writeback: keep only IL bit if a1, else clear.
		setU32(0, a1 ? (u32(0) & 0x400) : 0);

	perStage:
		// CS UAV (block+0x14), PS SAMP (+0x08), PS SRV (+0x04), CS SAMP (+0x10), CS SRV (+0x0C).
		for (std::uint32_t m = u32(0x14); m; m = u32(0x14)) {
			unsigned long b;
			_BitScanForward(&b, m);
			setU32(0x14, m & ~(1u << b));
			EngineCallV<68, void>(ctx, b, 1u, S + 0x1C0 + 4 * (2 * b + 80), reinterpret_cast<const std::uint32_t*>(nullptr));  // CSSetUAV
		}
		for (std::uint32_t m = u32(0x08); m; m = u32(0x08)) {
			unsigned long b;
			_BitScanForward(&b, m);
			setU32(0x08, m & ~(1u << b));
			EngineCallV<10, void>(ctx, b, 1u, &pool[5 * u32(0xBC + 4 * b) + 748 + u32(0xFC + 4 * b)]);  // PSSetSamplers
		}
		for (std::uint32_t m = u32(0x04); m; m = u32(0x04)) {
			unsigned long b;
			_BitScanForward(&b, m);
			setU32(0x04, m & ~(1u << b));
			EngineCallV<8, void>(ctx, b, 1u, reinterpret_cast<void*>(S + 0x140 + 8 * b));  // PSSetShaderResources (qword_143027FF0)
		}
		for (std::uint32_t m = u32(0x10); m; m = u32(0x10)) {
			unsigned long b;
			_BitScanForward(&b, m);
			setU32(0x10, m & ~(1u << b));
			EngineCallV<70, void>(ctx, b, 1u, &pool[5 * u32(0x1C0 + 4 * b) + 748 + u32(0x1C0 + 4 * (b + 16))]);  // CSSetSamplers
		}
		for (std::uint32_t m = u32(0x0C); m; m = u32(0x0C)) {
			unsigned long b;
			_BitScanForward(&b, m);
			setU32(0x0C, m & ~(1u << b));
			EngineCallV<67, void>(ctx, b, 1u, S + 0x1C0 + 4 * (2 * b + 32));  // CSSetShaderResources
		}
	}

	// CS_UTIL_RE_CSFLUSH=1 routes the replica's state flush through the CS reimplementation
	// (FlushDirtyStatesReplica) instead of the engine's SetDirtyStates, so the parity gate
	// can prove them byte-identical before the reimpl backs the multithreaded per-map driver.
	bool CsFlushRequested()
	{
		static const bool s_on = [] {
			char buf[8] = {};
			return GetEnvironmentVariableA("CS_UTIL_RE_CSFLUSH", buf, sizeof(buf)) && buf[0] == '1';
		}();
		return s_on || t_worker != nullptr;  // a bound worker MUST flush onto its own context
	}

	// CS_UTIL_RE_CSSETUP bitmask: 1=SetupMaterial, 2=SetupTechnique, 4=SetupGeometry route
	// through the CS reimplementation (validate each byte-exact via the parity gate before
	// it backs the private-CB worker path).
	int CsSetupMask()
	{
		static const int s_mask = [] {
			char buf[16] = {};
			return GetEnvironmentVariableA("CS_UTIL_RE_CSSETUP", buf, sizeof(buf)) ? atoi(buf) : 0;
		}();
		return t_worker ? 31 : s_mask;  // a bound worker MUST use every block-parameterized reimpl
	}

	// BSEffectShader::SetupGeometryAlphaBlending (1.5.97 0x14131F440) reimplemented against a
	// caller-supplied block S. Selects a blend-state index from the NiAlphaProperty flags and
	// stamps the block's blend dirty bits + change-detection caches (S+0 main word, S+0xA8 mode
	// cache, S+0xB4 flag cache). Byte-exact transcription; engine writes 0x143027EB0/F58/F64.
	void SetupGeomAlphaBlendReplica(std::uint8_t* S, RE::NiAlphaProperty* a2, RE::BSShaderProperty* a3, bool a4)
	{
		auto& main = *reinterpret_cast<std::uint32_t*>(S + 0x00);       // MEMORY[0x143027EB0]
		auto& modeCache = *reinterpret_cast<std::uint32_t*>(S + 0xA8);  // unk_143027F58
		auto& flagCache = *reinterpret_cast<std::uint32_t*>(S + 0xB4);  // unk_143027F64

		bool v4 = false, v5 = false, v6 = false;
		if (a2) {
			if (a4)
				v6 = (a2->alphaFlags & 0x200) != 0;
			if (a2->alphaFlags & 1) {
				v4 = true;
				v5 = true;
			}
		}
		if (a3 && a3->alpha >= 1.0f && !v4) {
			if (v6) {
				std::uint32_t v7 = main;
				if (modeCache) {
					v7 = main | 0x80;
					modeCache = 0;
					main |= 0x80;
				}
				if (flagCache != 1) {
					flagCache = 1;
					main = v7 | 0x100;
				}
			} else if (modeCache) {
				modeCache = 0;
				main |= 0x80;
			}
			return;
		}

		std::uint32_t v10 = 0;
		const auto label37 = [&] {  // set mode dirty (|0x80), fall through to label39
			v10 = main | 0x80;
			main |= 0x80;
		};
		const auto label39 = [&] {
			if (flagCache != static_cast<std::uint32_t>(v6)) {
				flagCache = static_cast<std::uint32_t>(v6);
				main = v10 | 0x100;
			}
		};
		bool did37 = false;

		if (!v5) {
			if (modeCache != 1) {
				modeCache = 1;
				label37();
				did37 = true;
			}
			// else: fall to label38 (v10 = main)
		} else {
			const std::uint16_t v8 = a2->alphaFlags;
			const std::uint16_t v9 = (v8 >> 1) & 0xF;  // src blend factor
			int target = -1;                           // which modeCache value / label to apply
			bool toL31 = false, toL18 = false;
			if (v9 == 6) {
				if ((v8 & 0x1E0) == 0xE0)
					toL18 = true;
				else if ((v8 & 0x1E0) == 0)
					toL31 = true;
			}
			if (!toL18 && !toL31) {
				if (!v9 && (v8 & 0x1E0) == 0) {
					toL31 = true;
				} else if ((v9 != 1 || (v8 & 0x1E0) != 0x40) && (v9 != 4 || (v8 & 0x1E0) != 0x20)) {
					if (v9 != 6 || (v8 & 0x1E0) != 0x120) {
						if (v9 == 4 && (v8 & 0x1E0) == 0xE0)
							target = 3;
						// else label38
					} else {
						toL31 = true;
					}
				} else {
					target = 4;
				}
			}
			if (toL18) {
				if (modeCache != 1) {
					modeCache = 1;
					label37();
					did37 = true;
				}
			} else if (toL31) {
				if (modeCache != 2) {
					modeCache = 2;
					label37();
					did37 = true;
				}
			} else if (target == 3) {
				if (modeCache != 3) {
					modeCache = 3;
					label37();
					did37 = true;
				}
			} else if (target == 4) {
				if (modeCache != 4) {
					modeCache = 4;
					label37();
					did37 = true;
				}
			}
		}
		if (!did37)
			v10 = main;  // label38
		label39();
	}

	// BSEffectShader::SetAlphaTestRef (1.5.97 0x14131F2A0) reimplemented against a caller-supplied
	// block S. Computes the alpha-test reference (threshold * material-alpha / 255) and, when it
	// changed, raises the alpha-CB dirty bit (0x200) + updates the cache (S+0xB8). Byte-exact.
	void SetAlphaTestRefReplica(std::uint8_t* S, RE::NiAlphaProperty* a2, RE::BSShaderProperty* a3)
	{
		auto& main = *reinterpret_cast<std::uint32_t*>(S + 0x00);   // MEMORY[0x143027EB0]
		auto& refCache = *reinterpret_cast<float*>(S + 0xB8);      // unk_143027F68 is a 4-byte FLOAT (movss)
		// Reproduce the engine's ALL-single-precision sequence exactly (0x14131F2A0):
		//   v4     = (int)truncate((float)alphaThreshold * material.alpha)      ; mulss + cvttss2si
		//   newRef = (float)v4 * (1/255 as float, dword_141540648 = 0x3B808081) ; cvtsi2ss + mulss
		//   if (cache != newRef) raise 0x200 + store                            ; ucomiss + movss
		// (0.0039215689 in the decompile was a MISREAD double; a double intermediate diverged by a ULP,
		// and an 8-byte store clobbered the adjacent field at S+0xBC -> load crash.)
		const __m128 thr = _mm_cvtsi32_ss(_mm_setzero_ps(), static_cast<int>(a2->alphaThreshold));  // (float)threshold
		const __m128 prod = _mm_mul_ss(thr, _mm_set_ss(a3 ? a3->alpha : 1.0f));
		const int    v4 = _mm_cvtt_ss2si(prod);
		const float  newRef = _mm_cvtss_f32(_mm_mul_ss(_mm_cvtsi32_ss(_mm_setzero_ps(), v4), _mm_set_ss(1.0f / 255.0f)));
		if (refCache != newRef) {
			main |= 0x200;
			refCache = newRef;
		}
	}

	// BSUtilityShader::RestoreGeometry (vf7, 1.5.97 0x141310300) reimplemented against a caller-
	// supplied block S + the worker's shadow-geom-token cache. Resets the per-pass geometry dirty
	// caches so the next pass's SetupGeometry re-emits from a clean baseline. Byte-exact.
	void RestoreGeometryReplica(std::uint8_t* S, std::uint8_t* shader, RE::BSRenderPass* a_pass)
	{
		const auto u32 = [&](std::size_t o) -> std::uint32_t& { return *reinterpret_cast<std::uint32_t*>(S + o); };
		auto&               main = u32(0x00);
		const std::uint32_t v2 = WsTechFlags(shader);
		std::uint32_t       v3 = main;
		if (v2 & 0x100000) {
			if (u32(0xA0)) {
				v3 = main | 0x40;
				u32(0xA0) = 0;
				main |= 0x40;
			}
			if (u32(0x88) != 3) {
				u32(0x88) = 3;
				const std::uint32_t v4 = v3 | 4;
				v3 &= ~4u;
				if (u32(0x8C) != 3)
					v3 = v4;
				main = v3;
			}
		}
		if ((v2 & 0x1200) != 0x1200 && a_pass->accumulationHint == 10 &&
			*reinterpret_cast<std::uint64_t*>(S + 0x90) != 0xFF00000000ull) {
			v3 |= 8u;
			*reinterpret_cast<std::uint64_t*>(S + 0x90) = 0xFF00000000ull;
			main = v3;
		}
		std::uint32_t&      token = WsShadowToken();
		const std::uint32_t t = token;
		if (t != 13) {
			if (u32(0xB0) != t) {
				u32(0xB0) = t;
				main = v3 | 0x80;
			}
			token = 13;
		}
	}

	// BSUtilityShader::RestoreTechnique (vf3, 1.5.97 0x14130DD80) reimplemented against a caller-
	// supplied block S. Resets the RT/DS-binding dirty caches (S+4, S+0x150..0x170) and the raster
	// caches when the technique's flag bits require it. Byte-exact; tail FUN_14131FCE0 is a no-op.
	void RestoreTechniqueReplica(std::uint8_t* S, std::uint8_t* shader, std::uint32_t a_technique)
	{
		const auto u32 = [&](std::size_t o) -> std::uint32_t& { return *reinterpret_cast<std::uint32_t*>(S + o); };
		const auto u64 = [&](std::size_t o) -> std::uint64_t& { return *reinterpret_cast<std::uint64_t*>(S + o); };
		auto&      main = u32(0x00);
		auto&      dirty4 = u32(0x04);  // unk_143027EB4
		std::uint32_t v4;
		if ((a_technique & 0x1E00000) != 0) {
			std::uint32_t v3 = dirty4;
			if (u64(0x150)) {  // unk_143028000 (RTV cache 0)
				v3 = dirty4 | 4;
				u64(0x150) = 0;
				dirty4 |= 4u;
			}
			if (u64(0x158)) {  // unk_143028008
				v3 |= 8u;
				u64(0x158) = 0;
				dirty4 = v3;
			}
			if (u64(0x160)) {  // unk_143028010
				v3 |= 0x10u;
				u64(0x160) = 0;
				dirty4 = v3;
			}
			if (u64(0x170)) {  // unk_143028020
				u64(0x170) = 0;
				dirty4 = v3 | 0x40;
			}
			v4 = main;
			if (u32(0xA4)) {  // unk_143027F54
				v4 = main | 0x1000;
				u32(0xA4) = 0;
				main |= 0x1000;
			}
			if (((a_technique - 43) & 0x2000) != 0) {
				if (u32(0xB4)) {  // unk_143027F64
					u32(0xB4) = 0;
					main = v4 | 0x100;
				}
				const std::int32_t dsIdx = EngineCall<std::int32_t>(reinterpret_cast<void*>(engine::GetDepthStencilTargetMain.address()));
				v4 = main;
				const std::uint64_t tgt = *reinterpret_cast<std::uint64_t*>(
					engine::g_dsTargetTable.address() + 152 * static_cast<std::size_t>(dsIdx));
				if (u64(0x150) == tgt) {
					dirty4 |= 4u;
					v4 = main | 1;
					u64(0x150) = 0;
					main |= 1;
					WsDsvDirty() = 0;  // unk_1430284C2 (OUT-of-block DSV dirty byte); worker cell under MT
				}
				if (u32(0x88) != 3) {  // unk_143027F38
					u32(0x88) = 3;
					const std::uint32_t v6 = v4 | 4;
					v4 &= ~4u;
					if (u32(0x8C) != 3)  // unk_143027F3C
						v4 = v6;
					main = v4;
				}
				if (u32(0xA0)) {  // dword_143027F50
					v4 |= 0x40u;
					u32(0xA0) = 0;
					main = v4;
				}
			}
		} else {
			v4 = main;
		}
		if ((WsTechFlags(shader) & 0x1200) == 0x1200) {
			if (u64(0x90) != 0xFF00000000ull) {  // unk_143027F40
				v4 |= 8u;
				u64(0x90) = 0xFF00000000ull;
				main = v4;
			}
			if (u32(0xB0) != 1) {  // unk_143027F60
				u32(0xB0) = 1;
				main = v4 | 0x80;
			}
		}
	}

	// BSUtilityShader::SetupMaterial (vf4, 1.5.97 0x14130E890) reimplemented against a caller-
	// supplied render-state block (S = the 0x5D8 block, 0x143027EB0 at N=1), context, and the
	// VS/PS PerMaterial constant buffers. Maps both PerMaterial CBs (WRITE_DISCARD), fills them
	// from the material (a2), stamps the block's PS SRV/sampler dirty bits + caches, then binds
	// VS slot 1 / PS slot 1. Byte-exact transcription; context calls via EngineCallV so the
	// compare recorder captures them exactly as it captures the engine's.
	void FlushSetupMaterialReplica(ID3D11DeviceContext* ctx, ID3D11Buffer* vsCB, ID3D11Buffer* psCB,
		std::uint8_t* S, std::uint8_t* shader /*a1*/, std::uint8_t* material /*a2*/)
	{
		auto* vsShader = *reinterpret_cast<std::uint8_t**>(S + 0x348);  // *0x1430281F8
		auto* psShader = *reinterpret_cast<std::uint8_t**>(S + 0x350);  // *0x143028200

		const auto mU8 = [&](std::size_t o) { return *reinterpret_cast<std::uint8_t*>(material + o); };
		const auto mU32 = [&](std::size_t o) { return *reinterpret_cast<std::uint32_t*>(material + o); };
		const auto mF32 = [&](std::size_t o) { return *reinterpret_cast<float*>(material + o); };
		const auto mPtr = [&](std::size_t o) { return *reinterpret_cast<std::uint8_t**>(material + o); };
		const auto sU32 = [&](std::size_t o) -> std::uint32_t& { return *reinterpret_cast<std::uint32_t*>(S + o); };
		const auto sPtr = [&](std::size_t o) -> std::uint8_t*& { return *reinterpret_cast<std::uint8_t**>(S + o); };

		void* vsMapped = nullptr;
		void* psMapped = nullptr;
		if (vsCB) {
			D3D11_MAPPED_SUBRESOURCE m{};
			EngineCallV<14, HRESULT>(ctx, reinterpret_cast<ID3D11Resource*>(vsCB), 0u, D3D11_MAP_WRITE_DISCARD, 0u, &m);
			vsMapped = m.pData;
		}
		if (psShader && psCB) {
			D3D11_MAPPED_SUBRESOURCE m{};
			EngineCallV<14, HRESULT>(ctx, reinterpret_cast<ID3D11Resource*>(psCB), 0u, D3D11_MAP_WRITE_DISCARD, 0u, &m);
			psMapped = m.pData;
		}

		const std::uint32_t flags = WsTechFlags(shader);
		if ((flags & 2) != 0 && (flags & 0x2040280) != 0) {
			const std::uint32_t idx = *engine::g_utilMaterialFillIndex;
			auto*               vd = reinterpret_cast<std::uint8_t*>(vsMapped) + 4u * (*reinterpret_cast<std::uint8_t*>(vsShader + 0x51));
			*reinterpret_cast<std::uint32_t*>(vd + 0) = mU32(8ull * idx + 0x0C);
			*reinterpret_cast<std::uint32_t*>(vd + 4) = mU32(8ull * idx + 0x10);
			*reinterpret_cast<std::uint32_t*>(vd + 8) = mU32(8ull * idx + 0x1C);
			*reinterpret_cast<std::uint32_t*>(vd + 12) = mU32(8ull * idx + 0x20);

			if ((flags & 0x2040080) != 0) {
				std::uint32_t v14 = 0;
				std::uint8_t* v15 = nullptr;
				if (EngineCallV<7, std::uint32_t>(material) == 2) {
					v14 = mU32(0x70);
					v15 = mPtr(0x48);
				} else if (EngineCallV<7, std::uint32_t>(material) == 1) {
					v14 = mU8(0x80);
					v15 = mPtr(0x58);
				}

				if ((flags & 0x20000000) != 0) {
					auto* pd = reinterpret_cast<std::uint8_t*>(psMapped) + 4u * (*reinterpret_cast<std::uint8_t*>(psShader + 0x43));
					*reinterpret_cast<float*>(pd + 0) = mF32(0x48) * mF32(0x6C);
					*reinterpret_cast<float*>(pd + 4) = mF32(0x4C) * mF32(0x6C);
					*reinterpret_cast<float*>(pd + 8) = mF32(0x50) * mF32(0x6C);
					*reinterpret_cast<std::uint32_t*>(pd + 12) = mU32(0x54);

					auto*         tex7 = *reinterpret_cast<std::uint8_t**>(mPtr(0x60) + 0x48);
					std::uint8_t* srv7 = tex7 ? *reinterpret_cast<std::uint8_t**>(tex7 + 0x10) : nullptr;
					if (sPtr(0x178) != srv7) {
						sU32(0x04) |= 0x80;
						sPtr(0x178) = srv7;
					}
					if (sU32(0xD8) != 0) { sU32(0x08) |= 0x80; sU32(0xD8) = 0; }
					if (sU32(0x118) != 1) { sU32(0x08) |= 0x80; sU32(0x118) = 1; }
				}

				auto*         tex0 = *reinterpret_cast<std::uint8_t**>(v15 + 0x48);
				std::uint8_t* srv0 = tex0 ? *reinterpret_cast<std::uint8_t**>(tex0 + 0x10) : nullptr;
				if (sPtr(0x140) != srv0) {
					sU32(0x04) |= 1;
					sPtr(0x140) = srv0;
				}
				if (sU32(0xBC) != v14) { sU32(0x08) |= 1; sU32(0xBC) = v14; }
				if (sU32(0xFC) != 3) { sU32(0x08) |= 1; sU32(0xFC) = 3; }
			}

			if ((flags & 0x200) != 0) {
				auto*         tex1 = *reinterpret_cast<std::uint8_t**>(mPtr(0x48) + 0x48);
				std::uint8_t* srv1 = tex1 ? *reinterpret_cast<std::uint8_t**>(tex1 + 0x10) : nullptr;
				if (sPtr(0x148) != srv1) {
					sU32(0x04) |= 2;
					sPtr(0x148) = srv1;
				}
				if (sU32(0xC0) != mU32(0x70)) { sU32(0x08) |= 2; sU32(0xC0) = mU32(0x70); }
				if (sU32(0x100) != 3) { sU32(0x08) |= 2; sU32(0x100) = 3; }
				auto* pd = reinterpret_cast<std::uint8_t*>(psMapped) + 4u * (*reinterpret_cast<std::uint8_t*>(psShader + 0x41));
				*reinterpret_cast<std::uint32_t*>(pd) = mU32(0x84);
			}
		}

		if (psShader) {
			if (vsCB) EngineCallV<15, void>(ctx, reinterpret_cast<ID3D11Resource*>(vsCB), 0u);
			if (psCB) EngineCallV<15, void>(ctx, reinterpret_cast<ID3D11Resource*>(psCB), 0u);
			EngineCallV<7, void>(ctx, 1u, 1u, &vsCB);
			EngineCallV<16, void>(ctx, 1u, 1u, &psCB);
		} else {
			if (vsCB) EngineCallV<15, void>(ctx, reinterpret_cast<ID3D11Resource*>(vsCB), 0u);
			EngineCallV<7, void>(ctx, 1u, 1u, &vsCB);
		}
	}

// The anonymous namespace opened above stays open through the next two definitions; it closes
// after FlushSetupGeometryReplica, before the UtilityPassReplica class-method definitions.

// Load-screen transient guard. During load-screen shadow rendering (a BSShadowParabolicLight
// pass under the MistMenu) some shader PerTechnique/PerGeometry CBs are momentarily unmapped
// (their buffer pointer is null) while the pass still runs. The engine tolerates this because it
// renders each pass exactly once; the compare/replace replica re-renders the pass and can read
// the transient. Routing a CB write to a per-thread scratch when the mapped pointer is null keeps
// the replica from faulting. In steady in-world rendering every CB the replica fills IS mapped
// (non-null), so the scratch is never used and the parity gate stays byte-exact. Sized for the
// largest register offset (4 * 255 + a 16-byte movups tail).
inline std::uint8_t* CbOrScratch(std::uint8_t* mapped)
{
	alignas(16) static thread_local std::uint8_t s_scratch[1088];
	return mapped ? mapped : s_scratch;
}

// Canonical user-space pointer test. The in-world settle latch keeps the replica off during load,
// but the load->gameplay boundary still leaves some shadow objects reachable-yet-uninitialized for
// a pass or two -- e.g. a valid shadow light (v30) whose light._ptr is momentarily a garbage,
// non-canonical value. Guarding an object deref on this test skips only those transient reads; in
// established gameplay every such pointer is a real heap object, so the guard never fires and the
// parity gate stays byte-exact.
inline bool IsCanonicalPtr(const void* p)
{
	const auto v = reinterpret_cast<std::uintptr_t>(p);
	return v >= 0x10000 && v < 0x0000800000000000ull;
}

// BSUtilityShader::SetupTechnique (vf2, 1.5.97 0x14130DF90) reimplemented against a caller-
// supplied context + the two PerTechnique constant buffers + the 0x5D8 state block S. Runs the
// technique->VS/PS index lookups + BeginTechnique (which VS/PS-binds, routed via EngineCall so
// the recorder captures it), maps/fills/binds the PerTechnique CBs (slot 0) with the shadow /
// depth / split / poisson / focus constants at the shader-declared byte offsets. At N=1 vsCB/psCB
// are null and the shader-owned CBs (vs+0x18 / ps+0x10) are resolved internally, so it is
// byte-identical. Returns BeginTechnique's result; the caller must bail the whole pass on false.
bool FlushSetupTechniqueReplica(ID3D11DeviceContext* ctx, ID3D11Buffer* vsCB, ID3D11Buffer* psCB,
                                std::uint8_t* S, RE::BSShader* a1, std::int32_t a2)
{
	// ---- 0x5D8 render-state block accessors (S = 0x143027EB0 at N=1) ----
	const auto u32    = [&](std::size_t o) { return *reinterpret_cast<std::uint32_t*>(S + o); };
	const auto setU32 = [&](std::size_t o, std::uint32_t v) { *reinterpret_cast<std::uint32_t*>(S + o) = v; };
	const auto orU32  = [&](std::size_t o, std::uint32_t v) { *reinterpret_cast<std::uint32_t*>(S + o) |= v; };
	const auto u8b    = [&](std::size_t o) { return *reinterpret_cast<std::uint8_t*>(S + o); };
	const auto setU8  = [&](std::size_t o, std::uint8_t v) { *reinterpret_cast<std::uint8_t*>(S + o) = v; };
	const auto f32S   = [&](std::size_t o) { return *reinterpret_cast<float*>(S + o); };
	const auto qw     = [&](std::size_t o) { return *reinterpret_cast<std::uint64_t*>(S + o); };
	const auto setQw  = [&](std::size_t o, std::uint64_t v) { *reinterpret_cast<std::uint64_t*>(S + o) = v; };
	const auto wf     = [](std::uint8_t* base, std::size_t byteOff, float v) { *reinterpret_cast<float*>(base + byteOff) = v; };

	const std::int32_t v2 = a2 - 0x2B;

	// Technique -> VS/PS shader-index lookups (pure CPU, no context calls) then BeginTechnique
	// (0x14131FBD0) which internally VSSetShader/PSSetShader-binds -- routed via EngineCall so
	// its (possibly tail-called) binds carry the out-of-module return address and are captured.
	const std::uint32_t vsIndex = EngineCall<std::uint32_t>(reinterpret_cast<void*>(engine::UtilVSIndex.address()), static_cast<std::uint32_t>(v2));   // FUN_141334900
	const std::uint32_t psIndex = EngineCall<std::uint32_t>(reinterpret_cast<void*>(engine::UtilPSIndex.address()), static_cast<std::uint32_t>(v2));   // FUN_141334970

	const bool v6 = ((v2 & 0x14000) == 0x14000) ||
	                (((v2 & 0x20004000) != 0x4000) && ((v2 & 0x1E02000) != 0x2000)) ||
	                ((v2 & 0x80) != 0) ||
	                ((v2 & 0x14000) == 0x10000);

	// BeginTechnique binds VS/PS on the engine's global context slot (0x143027EA0) and writes the
	// global VS/PS shader-object slots (block+0x348/0x350). For a worker that is fatal two ways: the
	// binds land on the shared IMMEDIATE context (used by every worker thread -> not thread-safe ->
	// the garbage-VS AV) and the global slots tear across workers. Under one mutex, temporarily point
	// the global context slot at the worker's OWN deferred context so the binds record onto the
	// worker's command list, and read back the resolved shader objects while still holding the lock.
	// At N=1 (t_worker==null) nothing is swapped or locked -> byte-exact.
	static std::mutex s_beginTechMutex;
	bool              v59;
	std::uint8_t*     gvs;
	std::uint8_t*     gps;
	{
		std::unique_lock<std::mutex> lk;
		ID3D11DeviceContext*         savedCtx = nullptr;
		if (t_worker) {
			lk = std::unique_lock<std::mutex>(s_beginTechMutex);
			savedCtx = *engine::g_engineCtxSlot;
			*engine::g_engineCtxSlot = ctx;  // ctx == WsCtx() == this worker's deferred context
		}
		v59 = EngineCall<bool>(reinterpret_cast<void*>(engine::BeginTechnique.address()), a1, vsIndex, psIndex, !v6);
		gvs = *reinterpret_cast<std::uint8_t**>(engine::S_base.address() + 0x348);
		gps = *reinterpret_cast<std::uint8_t**>(engine::S_base.address() + 0x350);
		if (t_worker)
			*engine::g_engineCtxSlot = savedCtx;
	}
	if (!v59)
		return false;

	// Propagate BeginTechnique's resolved VS/PS shader objects (read from the GLOBAL block under the
	// lock above) into the caller's block S so a worker with a PRIVATE block sees them; no-op at N=1.
	auto* vs = gvs;
	auto* ps = gps;
	*reinterpret_cast<std::uint8_t**>(S + 0x348) = vs;
	*reinterpret_cast<std::uint8_t**>(S + 0x350) = ps;

	// PerTechnique CBs to Map/fill/bind: passed private copies, or the shader-owned CBs at N=1.
	ID3D11Buffer*            vsBuf = vsCB ? vsCB : *reinterpret_cast<ID3D11Buffer**>(vs + 0x18);
	D3D11_MAPPED_SUBRESOURCE mapped{};  // == var_C8 (reused as the powf scratch far below)
	// Keep the mapped pointers in LOCALS: the engine stashes them on the SHARED vs/ps shader object
	// (+0x20 / +0x18), which two workers would clobber. The stashes below are kept for engine parity
	// but the fills read the locals, so a worker never depends on the shared field.
	void* pDataVSraw = nullptr;
	void* pDataPSraw = nullptr;

	if (vsBuf) {
		EngineCallV<14, HRESULT>(ctx, vsBuf, 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped);  // Map VS CB
		pDataVSraw = mapped.pData;
		*reinterpret_cast<void**>(vs + 0x20) = mapped.pData;                              // shader+0x20 = pData (parity)
	}
	ID3D11Buffer* psBuf = nullptr;
	if (v6) {
		psBuf = psCB ? psCB : *reinterpret_cast<ID3D11Buffer**>(ps + 0x10);
		if (psBuf) {
			EngineCallV<14, HRESULT>(ctx, psBuf, 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped);  // Map PS CB
			pDataPSraw = mapped.pData;
			*reinterpret_cast<void**>(ps + 0x18) = mapped.pData;                              // shader+0x18 = pData (parity)
		}
	}

	// Technique flags on the shader object (BSShader+0x90 / +0x94). Keep the shared-object write (an
	// engine consumer / N=1 reads it) AND mirror into the worker's cell so concurrent workers read
	// their OWN technique flags instead of the last writer's.
	*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(a1) + 0x90) = static_cast<std::uint32_t>(v2);
	*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(a1) + 0x94) = static_cast<std::uint32_t>(v2 & 0x7F);
	if (t_worker) {
		*t_worker->techFlags = static_cast<std::uint32_t>(v2);
		*t_worker->techSub = static_cast<std::uint32_t>(v2 & 0x7F);
	}

	// The engine reloads [shader+0x20]/[shader+0x18] lazily at each fill site; hoisting here is
	// equivalent because pDataVS is consumed only in VS fills (vs always present) and pDataPS
	// only in PS fills (ps present). A vertex-only technique (v6 false) leaves ps null, so the
	// deref must be guarded. CbOrScratch also absorbs the load-screen transient where the CB is
	// momentarily unmapped (see the note above); in steady state both are the mapped pointers.
	auto* pDataVS = CbOrScratch(vs ? reinterpret_cast<std::uint8_t*>(pDataVSraw) : nullptr);
	auto* pDataPS = CbOrScratch(ps ? reinterpret_cast<std::uint8_t*>(pDataPSraw) : nullptr);

	// -- Block 1: VS PerTechnique depth constants (offset vs[0x53]) --
	if ((v2 & 0x1E00100) == 0x100) {
		const std::size_t o = 4u * vs[0x53];
		const float*      c = engine::g_utilDepthConst.get();  // xmmword_141E0DF04 (4 floats)
		wf(pDataVS, o + 0, c[0] - f32S(0x35C));                // - unk_14302820C (block-local)
		wf(pDataVS, o + 4, c[1] - f32S(0x360));                // - qword_143028210 (block-local, dword)
		wf(pDataVS, o + 8, c[2] - 15.0f);                      // - dword_1415E29B4
		wf(pDataVS, o + 12, c[3] - 15.0f);
	}

	if (v2 & 0x1E00000) {
		// ---- shadow-cascade setup (writes PS CB + block dirty bits) ----
		std::uint8_t  v16 = u8b(0xB4);   // unk_143027F64 (byte)
		std::uint32_t flags = u32(0x00); // dword_143027EB0 (cached, threaded exactly as the asm)
		if (v16) {
			v16 = 0;
			flags |= 0x100;
			setU8(0xB4, 0);
			setU32(0x00, flags);
		}

		const std::uint8_t off47 = ps[0x47];  // ShadowSampleParam base (PS CB)

		if (v2 & 0x2000) {
			if (v16) {
				flags |= 0x100;
				setU8(0xB4, 0);
				setU32(0x00, flags);
			}
			if (v2 & 0x200000) {
				if (u32(0x88) != 0) {  // unk_143027F38
					const std::uint32_t alt = flags | 4u;
					std::uint32_t       nf = flags & ~4u;
					if (u32(0x8C) != 0)  // unk_143027F3C
						nf = alt;
					setU32(0x88, 0);
					setU32(0x00, nf);
				}
			} else {
				if (u32(0x88) != 5) {
					const bool eq = (u32(0x8C) == 5);
					setU32(0x88, 5);
					flags = eq ? (flags & ~4u) : (flags | 4u);
					setU32(0x00, flags);
				}
			}

			// 1/width, 1/height into PS CB at ps[0x4A]; .z/.w = 0.
			auto* const         rtDesc = reinterpret_cast<void*>(engine::g_mainRTDesc.get());  // &unk_14302BB20
			const std::size_t   o4A = 4u * ps[0x4A];
			const std::uint32_t w = EngineCall<std::uint32_t>(reinterpret_cast<void*>(engine::RTGetWidth.address()), rtDesc);   // FUN_140D74C20
			wf(pDataPS, o4A + 0, 1.0f / static_cast<float>(w));
			const std::uint32_t h = EngineCall<std::uint32_t>(reinterpret_cast<void*>(engine::RTGetHeight.address()), rtDesc);  // FUN_140D74C60
			*reinterpret_cast<std::uint64_t*>(pDataPS + o4A + 8) = 0;
			wf(pDataPS, o4A + 4, 1.0f / static_cast<float>(h));

			// Depth-target SRV cache -> RT dirty (bit0) + PS-SRV masks. rtPool base = &g_renderer.
			auto* const        rtPool = reinterpret_cast<std::uint8_t*>(engine::g_renderer.address());  // 0x143028490
			const std::int32_t dsIdx = EngineCall<std::int32_t>(reinterpret_cast<void*>(engine::GetDepthStencilTargetMain.address()));
			const std::uint64_t depthSRV = dsIdx == -1 ? 0ull :
				*reinterpret_cast<std::uint64_t*>(rtPool + 152u * static_cast<std::size_t>(dsIdx) + 0x2040);
			if (qw(0x150) != depthSRV) {  // unk_143028000
				orU32(0x04, 4);           // unk_143027EB4
				orU32(0x00, 1);
				setQw(0x150, depthSRV);
				WsDsvDirty() = 1;  // unk_1430284C2 (OUT of block); worker cell under MT
			}
			std::uint32_t v27 = u32(0x08);  // unk_143027EB8
			if (u32(0xC4) != 0) {           // unk_143027F74
				v27 |= 4;
				setU32(0xC4, 0);
				setU32(0x08, v27);
			}
			if (u32(0x104) != 0) {  // unk_143027FB4
				setU32(0x104, 0);
				setU32(0x08, v27 | 4);
			}
			if (*engine::g_focusShadowEnable /*byte_141E0DE43*/ && *engine::g_focusShadowCount != 0) {
				const std::int32_t  dsIdx2 = EngineCall<std::int32_t>(reinterpret_cast<void*>(engine::GetDepthStencilTargetMain.address()));
				const std::uint64_t stencilSRV = dsIdx2 == -1 ? 0ull :
					*reinterpret_cast<std::uint64_t*>(rtPool + 152u * static_cast<std::size_t>(dsIdx2) + 0x2048);
				if (qw(0x168) != stencilSRV) {  // unk_143028018
					orU32(0x04, 0x20);
					orU32(0x00, 1);
					setQw(0x168, stencilSRV);
					WsDsvDirty() = 1;  // worker cell under MT
				}
			}
			if (u32(0xA0) != 1) {  // dword_143027F50 -> raster dirty (bit6)
				orU32(0x00, 0x40);
				setU32(0xA0, 1);
			}
		}

		// ---- camera-relative split distances into PS CB ----
		auto* const cam = *engine::g_viewCamera;  // unk_1431D0E68
		const float camNear = *reinterpret_cast<float*>(cam + 0x160);
		const float camFar = *reinterpret_cast<float*>(cam + 0x164);

		if (v2 & 0x200000) {
			auto* const        ssn = *engine::g_shadowSceneNode;                     // shadowSceneNode
			auto* const        sunDL = *reinterpret_cast<std::uint8_t**>(ssn + 0x210);  // sunShadowDirLight
			auto* const        endArr = pDataPS + 4u * ps[0x4B];                     // EndSplitDistances
			auto* const        startArr = pDataPS + 4u * ps[0x4C];                   // StartSplitDistances
			const std::int32_t count = *reinterpret_cast<std::int32_t*>(sunDL + 0x140);  // shadowMapCount
			const float        farNear = camFar * camNear;                          // hoisted (matches asm)
			const float        farMinusNear = camFar - camNear;
			for (std::int32_t i = 0; i < count; ++i) {
				const float end = *reinterpret_cast<float*>(sunDL + 4 * i + 0x5A4);
				wf(endArr, 4u * i, (end * camFar - farNear) / (farMinusNear * end));
				const float start = *reinterpret_cast<float*>(sunDL + 4 * i + 0x598);
				wf(startArr, 4u * i, (start * camFar - farNear) / (start * farMinusNear));
			}
			const std::int32_t lastIdx = count - 1;
			*reinterpret_cast<std::uint32_t*>(endArr + 8) = *reinterpret_cast<std::uint32_t*>(endArr + 4 * lastIdx);
			wf(endArr, 12, static_cast<float>(count));
			wf(startArr, 12, static_cast<float>(*engine::g_shadowFixedCount));  // dword_141867188
			if (*engine::g_copySplitToVS /*byte_141E0DE4C*/) {
				const std::size_t ov = 4u * vs[0x53];
				*reinterpret_cast<std::uint32_t*>(pDataVS + ov + 0) = *reinterpret_cast<std::uint32_t*>(endArr + 0);
				*reinterpret_cast<std::uint32_t*>(pDataVS + ov + 4) = *reinterpret_cast<std::uint32_t*>(endArr + 4);
				*reinterpret_cast<std::uint32_t*>(pDataVS + ov + 8) = *reinterpret_cast<std::uint32_t*>(endArr + 8);
				*reinterpret_cast<std::uint32_t*>(pDataVS + ov + 12) = 0;
			}
			if (static_cast<std::uint32_t>(*engine::g_shadowMode - 2u) <= 1u) {  // dword_141E0DE34
				const float denom = static_cast<float>(*engine::g_poissonDenom);  // unk_143283B90
				const float poisson = *engine::g_poissonRadiusScale;              // fPoissonRadiusScale_141E10670
				wf(pDataPS, 4u * off47 + 8, poisson / denom);
				wf(pDataPS, 4u * off47 + 12, poisson / denom);
			}
		} else {
			wf(pDataPS, 4u * ps[0x4B], *engine::g_fixedSplit);                                 // unk_143283B78
			wf(pDataPS, 4u * ps[0x4B] + 4, static_cast<float>(*engine::g_shadowFixedCount));   // dword_141867188
			wf(pDataPS, 4u * ps[0x4C] + 12, static_cast<float>(*engine::g_shadowFixedCount));
			if (u32(0xA4) != 1) {  // unk_143027F54 -> bit12
				orU32(0x00, 0x1000);
				setU32(0xA4, 1);
			}
			if (static_cast<std::uint32_t>(*engine::g_shadowMode - 2u) <= 1u) {
				const float denom = static_cast<float>(*engine::g_poissonDenom);
				const float poisson = *engine::g_poissonRadiusScale;
				const float v42 = poisson / denom;
				wf(pDataPS, 4u * off47 + 8, v42);
				wf(pDataPS, 4u * off47 + 12, poisson * v42);
			}
		}

		// ---- focus-shadow fade weights into PS CB (offset ps[0x4D]) ----
		auto* const focusArr = pDataPS + 4u * ps[0x4D];
		const std::uint32_t n = *engine::g_focusShadowCount;                                    // unk_1431D0FB8 (._used)
		const float*        fdata = *engine::g_focusShadowData;                                 // *(unk_1431D0FA8) heap array (stride 16B)
		// The engine reads fdata guarded only by (focusArr && n>0), relying on the invariant
		// n>0 => fdata valid. That holds in steady state but is transiently violated during
		// load-screen shadow rendering (a BSShadowParabolicLight pass sees n>0 with fdata a
		// non-canonical pointer, mid-init). Guard on a canonical user-space pointer so the load
		// transient is skipped; in steady state fdata is always a valid heap pointer, so this
		// never changes behavior and stays byte-exact under the parity gate.
		const bool fdataValid = fdata && reinterpret_cast<std::uintptr_t>(fdata) < 0x0000800000000000ull;
		if (focusArr && fdataValid) {
			const float        maxDistSq = (*engine::g_maxFocusDist) * (*engine::g_maxFocusDist);  // fMaxFocusShadowMapDistance
			for (std::uint32_t i = 0; i < n; ++i) {
				const float thresh = *engine::g_fadeFracStart * maxDistSq;  // fFadingFracStart_141E106A0
				const float d = fdata[4 * i];
				const float wv = (d >= thresh) ? (maxDistSq - d) / (maxDistSq - thresh) : 1.0f;
				wf(focusArr, 4u * i, wv);
			}
		}
	} else {
		// (v2 & 0x1E00000)==0: only the biased-depth PS constants (powf-interpolated pair).
		if ((v2 & 0x40000) && v6) {
			float*      scratch = reinterpret_cast<float*>(&mapped);  // reuses var_C8 (D3D11_MAPPED_SUBRESOURCE)
			const float base = *engine::g_biasBase;                   // MEMORY[0x143283B7C]
			const float v53 = base * 0.2f;
			const float v54 = base - 5.0f;
			for (int i = 0; i < 2; ++i) {
				const float t = (static_cast<float>(i) + 1.0f) * 0.5f;
				const float p = EngineCall<float>(reinterpret_cast<void*>(engine::PowF.address()), v53, t);  // thunk_powf_14134BEAC
				scratch[i] = (p * 5.0f * 0.5f) + ((v54 * t + 5.0f) * 0.5f);
			}
			std::memcpy(pDataPS + 4u * ps[0x4B], &mapped, 16);  // movups: 2 computed + 2 map-struct tail bytes
		}
	}

	// ---- tail: blend enable + shadow-radius (VS CB) + alpha-CB dirty ----
	if ((v2 & 0x20004000) == 0x4000) {
		if (u32(0xA8) != 0) {  // unk_143027F58 -> blend dirty (bit7)
			orU32(0x00, 0x80);
			setU32(0xA8, 0);
		}
		if (v2 & 0x10000) {
			const std::size_t o = 4u * vs[0x54];
			wf(pDataVS, o + 0, 1.0f / *engine::g_shadowRadius);  // ShadowRadiusMaybe_141E10B78
			wf(pDataVS, o + 4, *engine::g_shadowSign);           // ShadowSign_141E10B7C
		}
	}
	if (v2 & 0x100000) {
		if (u8b(0xB4) != 0) {  // unk_143027F64 -> alpha-CB dirty (bit8)
			orU32(0x00, 0x100);
			setU8(0xB4, 0);
		}
	}

	// ---- Unmap + bind the PerTechnique CBs to slot 0 ----
	if (vsBuf)
		EngineCallV<15, void>(ctx, vsBuf, 0u);  // Unmap VS CB
	if (v6) {
		if (psBuf)
			EngineCallV<15, void>(ctx, psBuf, 0u);   // Unmap PS CB
		EngineCallV<7, void>(ctx, 0u, 1u, &vsBuf);   // VSSetConstantBuffers(slot0, 1, &vsBuf)
		EngineCallV<16, void>(ctx, 0u, 1u, &psBuf);  // PSSetConstantBuffers(slot0, 1, &psBuf)
	} else {
		EngineCallV<7, void>(ctx, 0u, 1u, &vsBuf);   // VSSetConstantBuffers(slot0, 1, &vsBuf)
	}

	return v59;
}

// BSUtilityShader::SetupGeometry (vf6, 1.5.97 0x14130EC70) reimplemented against a caller-supplied
// context + the two PerGeometry constant buffers + the 0x5D8 state block S. Fills the PerGeometry
// VS CB (slot 2) and PS CB (slot 2) from the pass geometry / shader-property / shadow-light data.
// The register indices come from the shared VS/PS shader objects (block+0x348 / +0x350); only the
// buffers are private. Every D3D11 call routes through EngineCallV so the compare recorder captures
// it identically; the pure-CPU engine helpers (matrix build/transpose, shadow fill, scissor mutate,
// accumulator/property accessors) are EngineCall'd and issue no context call. At N=1 vsCB/psCB are
// the shader-owned CBs (VS+0x38 / PS+0x30) so the fill is byte-identical.
void FlushSetupGeometryReplica(ID3D11DeviceContext* ctx, ID3D11Buffer* vsCB, ID3D11Buffer* psCB,
                               std::uint8_t* S, RE::BSShader* a_shader, RE::BSRenderPass* a_pass)
{
	using u8v = std::uint8_t;
	using u32 = std::uint32_t;
	using i32 = std::int32_t;

	auto* shader = reinterpret_cast<u8v*>(a_shader);
	auto* pass   = reinterpret_cast<u8v*>(a_pass);

	const auto F  = [](u8v* p, std::size_t o) -> float { return *reinterpret_cast<float*>(p + o); };
	const auto U  = [](u8v* p, std::size_t o) -> u32 { return *reinterpret_cast<u32*>(p + o); };
	const auto PP = [](u8v* p, std::size_t o) -> u8v* { return *reinterpret_cast<u8v**>(p + o); };
	const auto RELf = [](std::uintptr_t a) -> float { return *reinterpret_cast<float*>(a); };

	// Current VS/PS shader objects (the register-index source), from the state block.
	u8v* VS = *reinterpret_cast<u8v**>(S + 0x348);   // *0x1430281F8
	u8v* PS = *reinterpret_cast<u8v**>(S + 0x350);   // *0x143028200

	u8v* v9 = PS ? PS + 0x30 : nullptr;              // PS CB slot object (SetupShadowLightParameters arg3)

	// ---- Map the two PerGeometry CBs (WRITE_DISCARD, subresource 0). ----
	D3D11_MAPPED_SUBRESOURCE mapped{};
	u8v* vsMapped = nullptr;
	u8v* psMapped = nullptr;
	if (vsCB) {
		EngineCallV<14, HRESULT>(ctx, reinterpret_cast<ID3D11Resource*>(vsCB), 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped);  // Map
		vsMapped = reinterpret_cast<u8v*>(mapped.pData);
		if (VS)
			*reinterpret_cast<void**>(VS + 0x40) = vsMapped;  // engine v6[1]: store map ptr on the shader
	}
	if (PS && psCB) {
		EngineCallV<14, HRESULT>(ctx, reinterpret_cast<ID3D11Resource*>(psCB), 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped);  // Map
		psMapped = reinterpret_cast<u8v*>(mapped.pData);
		// BSRenderPass::SetupShadowLightParameters (and other EngineCall'd fill helpers) write the
		// PS PerGeometry CB via *(ps+0x38) -- the map pointer the engine stashes on the shader object
		// (v9->_refCount). The replica maps its OWN buffer, so it must store the map pointer here or
		// the helper writes the stale engine-window buffer (missing the shadow-light matrix + bias).
		*reinterpret_cast<void**>(PS + 0x38) = psMapped;
	}

	// CB destinations: mapped + 4 * (register-index byte on the shader object). CbOrScratch
	// absorbs the load-screen transient where a CB is momentarily unmapped (null base); in
	// steady state vsMapped/psMapped are the real mapped pointers so this is byte-exact.
	const auto vcb = [&](std::size_t shOff) -> u8v* { return CbOrScratch(vsMapped) + 4u * static_cast<std::size_t>(VS[shOff]); };
	const auto pcb = [&](std::size_t shOff) -> u8v* { return CbOrScratch(psMapped) + 4u * static_cast<std::size_t>(PS[shOff]); };

	auto& DW = *reinterpret_cast<u32*>(S + 0);       // dirty word (MEMORY[0x143027EB0], block+0)

	u8v* geom           = *reinterpret_cast<u8v**>(pass + 0x10);  // a2->geometry
	u8v* shaderProperty = *reinterpret_cast<u8v**>(pass + 0x08);  // a2->shaderProperty
	u8v* v88            = nullptr;                                 // effect-shader data (alpha-test-ref source)

	const u32 tech = WsTechFlags(shader);       // technique flag (a1->_pad_20[112]); worker cell under MT

	// ================= world matrix -> VS CB[VS+0x50] (unless tech & 4) =================
	if ((tech & 4) == 0) {
		alignas(16) float m44[16];
		if (EngineCallV<16, void*>(geom)) {          // AsParticlesGeom (geom vfunc 16)
			// Modified NiTransform: same rotate/scale, translate = world * modelBound.center.
			alignas(16) float mt[16];
			u8v*  gw = geom + 0x7C;                    // world (NiTransform)
			float r[9]; std::memcpy(r, gw, 36);        // rotate 3x3
			std::memcpy(mt, r, 36);
			const float sc = F(gw, 0x30);              // world.scale (geom+0xAC)
			const float cx = F(geom, 0x110), cy = F(geom, 0x114), cz = F(geom, 0x118);  // modelBound.center
			mt[9]  = F(gw, 0x24) + sc * (r[0] * cx + r[1] * cy + r[2] * cz);  // translate.x + s*(rot*center)
			mt[10] = F(gw, 0x28) + sc * (r[3] * cx + r[4] * cy + r[5] * cz);
			mt[11] = F(gw, 0x2C) + sc * (r[6] * cx + r[7] * cy + r[8] * cz);
			mt[12] = sc;
			EngineCall<void>(reinterpret_cast<void*>(engine::SG_BuildMatrix.address()), static_cast<void*>(m44), static_cast<const void*>(mt));   // FUN_1412c3440
		} else {
			EngineCall<void>(reinterpret_cast<void*>(engine::SG_BuildMatrix.address()), static_cast<void*>(m44), static_cast<const void*>(geom + 0x7C));  // FUN_1412c3440(&world)
		}
		EngineCall<void>(reinterpret_cast<void*>(engine::SG_MatrixTranspose.address()), static_cast<void*>(vcb(0x50)), static_cast<const void*>(m44));  // D3DXMatrixTranspose
	}

	if (tech & 0x1E00000) {
		// ============================ SHADOW PATH ============================
		u8v** sceneLights = pass[0x1F] ? *reinterpret_cast<u8v***>(pass + 0x38) : nullptr;  // numLights ? a2->sceneLights : 0
		u8v*  v30 = *sceneLights;                                                           // first shadow light

		EngineCall<void>(reinterpret_cast<void*>(engine::SG_ShadowFill.address()), static_cast<void*>(pass), 0);                 // FUN_14130f960 (pure CPU)
		// SG_SetupShadowLightParams reads ONLY arg3+8 (the PS PerGeometry CB base) and writes the CB.
		// At N=1 pass the real slot (v9 = PS+0x30, whose +8 = PS+0x38 = psMapped) -> byte-exact. A
		// worker passes a PRIVATE {pad, psMapped} slot so the helper writes ITS OWN buffer instead of
		// the shared PS+0x38 the other workers overwrite (verified: no other v9 offset is read).
		void* psCbSlotFake[2] = { nullptr, psMapped };
		void* shadowLightArg = t_worker ? static_cast<void*>(psCbSlotFake) : static_cast<void*>(v9);
		EngineCall<void>(reinterpret_cast<void*>(engine::SG_SetupShadowLightParams.address()), static_cast<void*>(pass), 0, shadowLightArg);  // 0x14130fbe0 (pure CPU)

		// lodFade constant -> VS CB[VS+0x55].x and PS CB[PS+0x48].z (raw dword copy)
		const u32 v32 = v30[0x63] ? *reinterpret_cast<u32*>(engine::SG_c283B88.address()) : 1287568416u;  // dword_141667CD0
		*reinterpret_cast<u32*>(vcb(0x55))       = v32;
		*reinterpret_cast<u32*>(pcb(0x48) + 8)   = v32;

		// falloff -> PS CB[PS+0x48].x
		if (tech & 0x1800000) {
			u8v* lgtR = PP(v30, 0x48);
			if (IsCanonicalPtr(lgtR))
				*reinterpret_cast<u32*>(pcb(0x48)) = U(lgtR, 0x128);   // v30->light->radius.x
		} else if (tech & 0x400000) {
			*reinterpret_cast<u32*>(pcb(0x48)) = U(v30, 0x568);            // v30->falloff
		}

		if (!(tech & 0x200000)) {
			// spot/point sub-branch. FUN_140d70100 builds a D3D11_RECT and calls RSSetScissorRects on
			// the engine's GLOBAL context slot (*0x143027EA0) -- which, from N worker threads, both
			// races and lands on the shared immediate context (the scissor never reaches the worker's
			// command list). A worker binds the identical rect on ITS OWN deferred context instead; at
			// N=1 the engine call is used verbatim (byte-exact; the recorder does not track scissor).
			if (t_worker) {
				const std::int32_t sl = static_cast<std::int32_t>(U(v30, 0x544));
				const std::int32_t st = static_cast<std::int32_t>(U(v30, 0x550));
				const D3D11_RECT   rect{ sl, st, sl + static_cast<std::int32_t>(U(v30, 0x548)), st + static_cast<std::int32_t>(U(v30, 0x54C)) };
				ctx->RSSetScissorRects(1, &rect);
			} else {
				EngineCall<void>(reinterpret_cast<void*>(engine::SG_ScissorFromBBox.address()),
					reinterpret_cast<void*>(engine::g_renderer.address()),
					U(v30, 0x544), U(v30, 0x550), U(v30, 0x548), U(v30, 0x54C));   // FUN_140d70100(scissor,left,bottom,right,top)
			}

			// camera-relative view-depth falloff -> VS CB[VS+0x55].z. cam (*0x1431D0F88), the light
			// (v30->light._ptr) and the view frustum (*0x1431D0E68) are all engine objects that can
			// be momentarily non-canonical at the load boundary; skip the read when any is.
			u8v* cam = *reinterpret_cast<u8v**>(engine::SG_pCamNode.address());        // *0x1431D0F88
			u8v* lgt = PP(v30, 0x48);
			u8v* vf  = *reinterpret_cast<u8v**>(engine::SG_pViewFrustumObj.address());  // *0x1431D0E68
			if (IsCanonicalPtr(cam) && IsCanonicalPtr(lgt) && IsCanonicalPtr(vf)) {
				// Bit-exact SSE-scalar transcription of the falloff at 0x14130F02A-F0F7. Plain C
				// diverged by 1 ULP because MSVC auto-vectorizes/reassociates the dot product; the
				// _ss intrinsics pin each op to the engine's exact subss/mulss/addss/divss sequence.
				const auto    ld = [](float f) { return _mm_set_ss(f); };
				const __m128  dx = _mm_sub_ss(ld(F(lgt, 0xA0)), ld(F(cam, 0xA0)));
				const __m128  dy = _mm_sub_ss(ld(F(lgt, 0xA4)), ld(F(cam, 0xA4)));
				const __m128  dz = _mm_sub_ss(ld(F(lgt, 0xA8)), ld(F(cam, 0xA8)));
				__m128        distv = _mm_mul_ss(ld(F(cam, 0x88)), dy);                  // Data[3]*dy
				distv = _mm_add_ss(distv, _mm_mul_ss(ld(F(cam, 0x7C)), dx));             // + Data[0]*dx
				distv = _mm_add_ss(distv, _mm_mul_ss(ld(F(cam, 0x94)), dz));             // + Data[6]*dz
				distv = _mm_add_ss(distv, ld(F(lgt, 0x128)));                            // + light->radius.x
				__m128        v36 = ld(v30[0x63] ? RELf(engine::SG_c283B7C.address()) : 10000.0f);
				if (_mm_comige_ss(v36, distv))                                           // v36 >= distv ? distv : v36
					v36 = distv;
				const __m128  farv = ld(F(vf, 0x164));
				const __m128  nearv = ld(F(vf, 0x160));
				const __m128  invr = _mm_div_ss(_mm_set_ss(1.0f), _mm_sub_ss(farv, nearv));  // 1/(Far-Near)
				const __m128  t1 = _mm_mul_ss(_mm_mul_ss(farv, v36), invr);
				const __m128  t2 = _mm_mul_ss(_mm_mul_ss(farv, nearv), invr);
				float         v37 = _mm_cvtss_f32(_mm_div_ss(_mm_sub_ss(t1, t2), v36));
				v37 = (v37 <= 1.0f) ? ((v37 < 0.0f) ? 0.0f : v37) : 1.0f;
				*reinterpret_cast<float*>(vcb(0x55) + 8) = v37;
			}
		}

		if (pass[0x1C] == 10) {  // accumulationHint == 10: paired fade token S+0x90/0x94
			u8v* fn = PP(shaderProperty, 0x60);
			const float fade = (pass[0x1E] & 0x80) ? F(fn, 0x14C) : F(fn, 0x130);
			const u8v   kb   = static_cast<u8v>(static_cast<i32>(fade * 31.0f));  // cvttss2si (truncate) -> low byte
			if (U(S, 0x90) != 11u || U(S, 0x94) != static_cast<u32>(kb)) {
				DW |= 8;
				*reinterpret_cast<u32*>(S + 0x90) = 11;
				*reinterpret_cast<u32*>(S + 0x94) = kb;
			}
		}
	} else if ((tech & 0x20004000) != 0x4000) {
		// ======================= MATERIAL / EFFECT PATH =======================
		if (tech & 0x100000) {
			v88 = PP(shaderProperty, 0x68);            // effectData
			u8v* blockOut = v88 ? PP(v88, 0x20) : nullptr;
			if (v88 && blockOut) {
				u8v* rd = PP(blockOut, 0x48);
				std::uintptr_t srv = rd ? *reinterpret_cast<std::uintptr_t*>(rd + 0x10) : 0;  // texture SRV
				if (*reinterpret_cast<std::uintptr_t*>(S + 0x140) != srv) {   // qword_143027FF0
					*reinterpret_cast<u32*>(S + 4) |= 1;                      // unk_143027EB4
					*reinterpret_cast<std::uintptr_t*>(S + 0x140) = srv;
				}
				// sampler/addressing state (S+8 accumulator; S+0xBC,0xFC address modes)
				u32 eb8 = U(S, 8);                                            // unk_143027EB8
				if (U(S, 0xBC) != 3) { eb8 |= 1; *reinterpret_cast<u32*>(S + 0xBC) = 3; *reinterpret_cast<u32*>(S + 8) = eb8; }
				if (U(S, 0xFC) != 3) { eb8 |= 1; *reinterpret_cast<u32*>(S + 0xFC) = 3; *reinterpret_cast<u32*>(S + 8) = eb8; }
				if (U(S, 0xA0) != 1) { DW |= 0x40; *reinterpret_cast<u32*>(S + 0xA0) = 1; }   // dword_143027F50
				if (U(S, 0x88) != 3) {                                        // unk_143027F38 (+ cmov on S+0x8C)
					*reinterpret_cast<u32*>(S + 0x88) = 3;
					const u32 with4 = DW | 4, without4 = DW & ~4u;
					DW = (U(S, 0x8C) != 3) ? with4 : without4;
				}
				const u32 tok = U(S, 0xB0);                                   // unk_143027F60
				WsShadowToken() = tok;                                        // dword_141E10660 (worker-private cache)
				if (tok) { DW |= 0x80; *reinterpret_cast<u32*>(S + 0xB0) = 0; }
			}
		}

		if ((tech & 0x1E00100) == 0x100) {                                   // world matrix -> VS CB[VS+0x50]
			alignas(16) float m44b[16];
			EngineCall<void>(reinterpret_cast<void*>(engine::SG_BuildMatrix.address()), static_cast<void*>(m44b), static_cast<const void*>(geom + 0x7C));  // FUN_1412c3440
			EngineCall<void>(reinterpret_cast<void*>(engine::SG_MatrixTranspose.address()), static_cast<void*>(vcb(0x50)), static_cast<const void*>(m44b));  // transpose
		}

		if ((tech & 0x1200) == 0x1200) {
			// stencil sub-branch. NOTE: FUN_140d6fcf0 MUTATES the renderer scissor global (0x143028490).
			if (U(S, 0x90) != 1u || U(S, 0x94) != 0xFFu) { DW |= 8; *reinterpret_cast<u32*>(S + 0x90) = 1; *reinterpret_cast<u32*>(S + 0x94) = 0xFF; }
			if (U(S, 0xB0)) { DW |= 0x80; *reinterpret_cast<u32*>(S + 0xB0) = 0; }
			if (U(S, 0x88) != 0) {
				*reinterpret_cast<u32*>(S + 0x88) = 0;
				const u32 with4 = DW | 4, without4 = DW & ~4u;
				DW = (U(S, 0x8C) != 0) ? with4 : without4;
			}
			*reinterpret_cast<u32*>(vcb(0x57)) = *reinterpret_cast<u32*>(engine::SG_stencilVal014.address());  // dword_141E0E014 -> VS CB[VS+0x57]
			EngineCall<void>(reinterpret_cast<void*>(engine::SG_ScissorApply.address()),
				reinterpret_cast<void*>(engine::g_renderer.address()), static_cast<void*>(PS));  // FUN_140d6fcf0(scissor, PS)
		} else {
			if (tech & 0x200) {
				u8v* d52 = vcb(0x52);
				if (tech & 0x400) {
					alignas(16) float mvw[16];
					EngineCall<void>(reinterpret_cast<void*>(engine::SG_WorldToView.address()), static_cast<void*>(geom + 0x7C), 0, static_cast<void*>(mvw));  // FUN_140d42c50
					u8v* accum = reinterpret_cast<u8v*>(EngineCall<void*>(reinterpret_cast<void*>(engine::SG_GetAccumulator.address())));                       // GetCurrentAccumulator
					EngineCall<void>(reinterpret_cast<void*>(engine::SG_Vec3TransformCoord.address()), static_cast<void*>(d52), static_cast<const void*>(accum + 0x16C), static_cast<const void*>(mvw));  // D3DXVec3TransformCoord
				}
				*reinterpret_cast<u32*>(d52 + 0xC) = U(shaderProperty, 0x104);
			}

			if (PS) {
				if (tech & 0x20000) {
					// S+0xB0 conditional clear (spatial-fade gate)
					if (*reinterpret_cast<u8v*>(engine::SG_flagDE4C.address()) &&
						static_cast<u32>(*reinterpret_cast<u32*>(engine::SG_mode1D0E28.address()) - 16u) <= 1u &&
						U(S, 0xB0)) {
						DW |= 0x80; *reinterpret_cast<u32*>(S + 0xB0) = 0;
					}
					float extraParam = static_cast<float>(static_cast<u32>(pass[0x1D]));
					if (*reinterpret_cast<i32*>(engine::SG_modeDF94.address()) == 10) {   // decal-alpha hash lookup
						u8v* fadeNode = PP(shaderProperty, 0x60);
						u8v* accum    = reinterpret_cast<u8v*>(EngineCall<void*>(reinterpret_cast<void*>(engine::SG_GetAccumulator.address())));
						u8v* map      = accum + 0xD0;
						const u32 idx = EngineCallV<1, u32>(map, static_cast<void*>(fadeNode));         // (map)->vtbl[1](map, fadeNode)
						u8v* node     = *reinterpret_cast<u8v**>(*reinterpret_cast<std::uintptr_t*>(map + 0x10) + 8ull * idx);
						i32  found    = 0;
						while (node) {
							if (EngineCallV<2, bool>(map, static_cast<void*>(fadeNode), *reinterpret_cast<void**>(node + 8))) {  // (map)->vtbl[2](map, fadeNode, key)
								found = *reinterpret_cast<i32*>(node + 0x10);
								break;
							}
							node = *reinterpret_cast<u8v**>(node);
						}
						extraParam = (static_cast<float>(static_cast<u32>(found)) * 255.0f) / static_cast<float>(U(accum, 0xF0));
					}
					const float B = 0.0078125f;                          // 1/128
					float v61 = (128.0f - extraParam) * B; v61 = (v61 <= 1.0f) ? ((v61 < 0.0f) ? 0.0f : v61) : 1.0f;
					const float v62 = extraParam - 128.0f;
					float v63 = (128.0f - std::fabs(v62)) * B; v63 = (v63 <= 1.0f) ? ((v63 < 0.0f) ? 0.0f : v63) : 1.0f;
					float v64 = v62 * RELf(engine::SG_recip127.address()); v64 = (v64 <= 1.0f) ? ((v64 < 0.0f) ? 0.0f : v64) : 1.0f;
					u8v* d42 = pcb(0x42);
					*reinterpret_cast<float*>(d42 + 0)   = v64;
					*reinterpret_cast<float*>(d42 + 4)   = v61;
					*reinterpret_cast<float*>(d42 + 8)   = v63;
					*reinterpret_cast<u32*>(d42 + 0xC)   = 0x3F800000u;   // 1.0
				} else if (tech & 0x80000) {
					u8v* accum = reinterpret_cast<u8v*>(EngineCall<void*>(reinterpret_cast<void*>(engine::SG_GetAccumulator.address())));
					u8v* d42 = pcb(0x42);
					*reinterpret_cast<u32*>(d42 + 0xC) = 0x3F800000u;     // 1.0
					*reinterpret_cast<u32*>(d42 + 0)   = U(accum, 0x118); // color.r
					*reinterpret_cast<u32*>(d42 + 8)   = U(accum, 0x120); // color.b
					*reinterpret_cast<u32*>(d42 + 4)   = U(accum, 0x11C); // color.g
				}
			}
		}
	}

	// =========================== wind branch (tech & 0x4000000) ===========================
	if (tech & 0x4000000) {
		u8v* d56 = vcb(0x56);
		u8v* fadeNode = PP(shaderProperty, 0x60);
		u8v* leaf = nullptr;
		float v74 = 0.0f;
		if (fadeNode != *reinterpret_cast<u8v**>(engine::SG_pFadeExclude.address()) && fadeNode) {
			leaf = reinterpret_cast<u8v*>(EngineCallV<63, void*>(fadeNode));  // AsLeafAnimNode (vfunc 63)
			if (leaf)
				v74 = F(leaf, 0x164) * 6.0f;
		}
		*reinterpret_cast<float*>(d56 + 0) = v74;
		*reinterpret_cast<u32*>(d56 + 4)   = U(*reinterpret_cast<u8v**>(engine::g_shadowSceneNode.address()), 0x304);  // windMagnitude
		float v75 = 0.0f;
		if (leaf) {
			float x = F(leaf, 0x158);
			i32   i = *reinterpret_cast<i32*>(&x);
			i = 0x5F3759DF - (i >> 1);
			float y = *reinterpret_cast<float*>(&i);
			v75 = (1.5f - (x * 0.5f) * y * y) * y * x;   // x * rsqrt(x)
		}
		const float wmin = RELf(engine::SG_windFadeMin.address());
		const float wmax = RELf(engine::SG_windFadeMax.address());
		float v76 = leaf ? F(leaf, 0x15C) : 1.0f;
		const float t = (v75 - wmin) / (wmax - wmin);
		const float w = (1.0f - t) * v76;
		const float v27 = (w >= 0.0f) ? w : 0.0f;
		if (!(v76 < v27))
			v76 = v27;
		*reinterpret_cast<float*>(d56 + 8) = v76;
		*reinterpret_cast<u32*>(d56 + 0xC) = leaf ? U(leaf, 0x160) : 0x3F800000u;  // 1.0 default
	}

	// ================= alpha / fade PS-CB fill + unmap + bind =================
	u8v* niProp = reinterpret_cast<u8v*>(EngineCall<RE::NiAlphaProperty*>(reinterpret_cast<void*>(engine::GetNiProperty.address()), a_pass));  // BSRenderPass::GetNiProperty

	if (PS) {
		bool doFill;
		if (niProp && (*reinterpret_cast<u8v*>(niProp + 0x30) & 1))     doFill = true;
		else if (tech & 0x80)                                          doFill = true;
		else if ((tech & 0x14000) != 0x10000)                          doFill = false;  // -> straight to bind
		else                                                           doFill = true;

		if (doFill) {
			u8v* d40 = pcb(0x40);
			if (tech & 0x20000000) {                                    // shaderProperty color -> PS CB[PS+0x44]
				u8v* col = PP(shaderProperty, 0x88);
				u8v* d44 = pcb(0x44);
				if (col) {
					*reinterpret_cast<u32*>(d44 + 0) = U(col, 0);
					*reinterpret_cast<u32*>(d44 + 4) = U(col, 4);
					*reinterpret_cast<u32*>(d44 + 8) = U(col, 8);
				} else {
					*reinterpret_cast<u32*>(d44 + 0) = 0x3F800000u;
					*reinterpret_cast<u32*>(d44 + 4) = 0x3F800000u;
					*reinterpret_cast<u32*>(d44 + 8) = 0x3F800000u;
				}
				*reinterpret_cast<u32*>(d44 + 0xC) = U(shaderProperty, 0x30);  // alpha
			}
			// alpha-test ref -> PS CB[PS+0x40].x
			if (v88) {
				*reinterpret_cast<float*>(d40) = static_cast<float>(static_cast<u32>(v88[0x7C])) * RELf(engine::SG_recip255.address());
			} else if (niProp) {
				if (*reinterpret_cast<u8v*>(niProp + 0x30) & 1) {
					*reinterpret_cast<u32*>(d40) = 0x3F7EFEFFu;
				} else {
					const u8v mode = niProp[0x32];
					const float v85 = static_cast<float>(static_cast<u32>(mode)) * RELf(engine::SG_recip255.address()) + RELf(engine::SG_alphaBias.address());
					*reinterpret_cast<float*>(d40) = v85;
					if (mode == 4)
						*reinterpret_cast<float*>(d40) = v85 + RELf(engine::SG_recip255.address());
				}
			}
			// fade -> PS CB[PS+0x40].w
			if ((tech & 0x14000) == 0x10000) {
				const std::uint64_t impl = *reinterpret_cast<std::uint64_t*>(shaderProperty + 0x38);
				if ((impl & 0x4000ull) || (impl & 0x400000000000ull)) {
					*reinterpret_cast<u32*>(d40 + 0xC) = U(shaderProperty, 0x108);
				} else {
					u8v* fn = PP(shaderProperty, 0x60);
					*reinterpret_cast<float*>(d40 + 0xC) = F(fn, 0x130) * F(shaderProperty, 0x30);  // currentFade * alpha
				}
				if (pass[0x1E] & 0x80) {
					u8v* fn = PP(shaderProperty, 0x60);
					*reinterpret_cast<float*>(d40 + 0xC) = F(fn, 0x14C) * F(d40, 0xC);
				}
			}
		}

		if (vsCB) EngineCallV<15, void>(ctx, reinterpret_cast<ID3D11Resource*>(vsCB), 0u);  // Unmap VS CB
		if (psCB) EngineCallV<15, void>(ctx, reinterpret_cast<ID3D11Resource*>(psCB), 0u);  // Unmap PS CB
		ID3D11Buffer* vb = vsCB; EngineCallV<7,  void>(ctx, 2u, 1u, &vb);   // VSSetConstantBuffers slot 2
		ID3D11Buffer* pb = psCB; EngineCallV<16, void>(ctx, 2u, 1u, &pb);   // PSSetConstantBuffers slot 2
		return;
	}

	// ---- no pixel shader: unmap + bind VS only ----
	if (vsCB) EngineCallV<15, void>(ctx, reinterpret_cast<ID3D11Resource*>(vsCB), 0u);  // Unmap VS CB
	{ ID3D11Buffer* vb = vsCB; EngineCallV<7, void>(ctx, 2u, 1u, &vb); }                 // VSSetConstantBuffers slot 2
}
}  // anonymous namespace (opened before CsFlushRequested; spans the three setup reimpls)

void UtilityPassReplica::DrawTriShapeReplica(void* a_rendererData, std::uint32_t a_startIndex, std::uint32_t a_triCount)
{
	// BSGraphics::Renderer::DrawTriShape (0x140D6BFE0), replicated exactly: vertex-desc
	// and topology change detection into the dirty word, state flush, then unconditional
	// IB/VB binds and the indexed draw (the engine does NOT cache the IB/VB binds here).
	auto* rd = static_cast<engine::TriShapeData*>(a_rendererData);
	// vertexDesc (S+0x340), state flags (S+0), topology (S+0x358) live in the worker's
	// render-state block; at N=1 WsBlock() is the engine global block.
	auto* const   S = WsBlock();
	auto&         vertexDesc = *reinterpret_cast<std::uint64_t*>(S + 0x340);
	auto&         stateFlags = *reinterpret_cast<std::uint32_t*>(S + 0);
	auto&         topology = *reinterpret_cast<std::uint32_t*>(S + 0x358);
	if (vertexDesc != rd->vertexDesc) {
		vertexDesc = rd->vertexDesc;
		stateFlags |= 0x400;  // DIRTY_VERTEX_DESC
	}
	if (topology != 4 /*D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST*/) {
		topology = 4;
		stateFlags |= 0x800;  // DIRTY_PRIMITIVE_TOPO
	}
	auto* ctx = WsCtx();
	if (CsFlushRequested())
		FlushDirtyStatesReplica(S, ctx, false);
	else
		EngineCall<void>(reinterpret_cast<void*>(engine::SetDirtyStates.address()), false);

	const UINT stride = static_cast<UINT>((4 * rd->vertexDesc) & 0x3C);
	const UINT offset = 0;
	EngineCallV<19, void>(ctx, rd->indexBuffer, DXGI_FORMAT_R16_UINT, 0u);   // IASetIndexBuffer
	EngineCallV<18, void>(ctx, 0u, 1u, &rd->vertexBuffer, &stride, &offset); // IASetVertexBuffers
	EngineCallV<12, void>(ctx, 3u * a_triCount, a_startIndex, 0);            // DrawIndexed
}

// ===================================================================================================
// Shadow instancing (the FPS lever): render a set of captured static shadow passes as one
// DrawIndexedInstanced per unique (mesh, technique) group instead of one DrawIndexed per object.
//
// Design (see docs/development/utility-pass-re.md + the shadow-full-ownership memo):
//  * Each object's World matrix is byte-identical to the b2 PerGeometry World the engine builds for
//    that object (SG_BuildMatrix on geom+0x7C, then D3DXMatrixTranspose). It is packed into a
//    per-instance vertex stream as four R16G16B16A16_FLOAT rows (32 bytes) -- exactly the engine's
//    grass-instance layout (VA_INSTANCEDATA, attr 9) that the INSTANCED Utility VS consumes at
//    TEXCOORD4-7 (slot 1, PER_INSTANCE).
//  * The instanced input layout is created by the engine's own ILCreate, keyed by
//    (renderStateVertexDesc & VS.vertexDesc): setting renderStateVertexDesc = meshDesc | VA_INSTANCEDATA
//    and binding the INSTANCED VS (whose vertexDesc carries VA_INSTANCEDATA with 0xF wildcard offset
//    nibbles) yields POSITION at slot 0 (mesh's real offset) + the 4 instance rows at slot 1.
//  * Per-group state reuse: run BeginTechnique (FlushSetupTechniqueReplica) so the engine binds the
//    depth PS + blend/depth/raster + PerTechnique CB, then OVERRIDE only the bound VS with the
//    INSTANCED permutation. b2 (PerGeometry) is intentionally NOT filled -- World comes from the
//    instance stream. Only whole-TRISHAPE (geom+0x150==3), non-skinned, non-alpha-test passes are
//    instanced here; everything else stays engine-rendered inline.
//
// MUST be called on the render thread while the shadow map's RT/DSV/viewport are still bound. On exit
// the technique/material caches are invalidated so the engine re-establishes its own VS on the next
// pass (otherwise a cache hit would draw an engine pass through the still-bound INSTANCED VS).

// COMMAND-VALIDATION state (always-on, negligible cost): pass-conservation invariants checked per map,
// a ring of the most recent instanced draw commands (devbench-readable), and the throttled F16C-vs-
// engine-reference pack compare. A nonzero invariantViolations/packMismatches = the instanced submission
// no longer covers exactly the claimed pass set.
namespace
{
	struct InstValState
	{
		std::atomic<std::uint32_t> invariantViolations{ 0 };
		std::atomic<std::uint32_t> packChecks{ 0 };
		std::atomic<std::uint32_t> packMismatches{ 0 };
		std::atomic<std::uint32_t> mapsValidated{ 0 };

		struct Cmd
		{
			void*         rd;
			std::uint32_t tri, n, baseInst, tech;
			std::uint8_t  fade;
		};
		static constexpr std::uint32_t kRing = 64;
		Cmd                            ring[kRing]{};
		std::atomic<std::uint32_t>     ringHead{ 0 };

		void Record(void* rd, std::uint32_t tri, std::uint32_t n, std::uint32_t baseInst, std::uint32_t tech, std::uint8_t fade)
		{
			const auto h = ringHead.fetch_add(1, std::memory_order_relaxed) % kRing;
			ring[h] = Cmd{ rd, tri, n, baseInst, tech, fade };
		}
	};
	InstValState g_instVal;
}

std::array<std::uint32_t, 4> UtilityPassReplica::InstValReport() const
{
	return { g_instVal.mapsValidated.load(), g_instVal.invariantViolations.load(),
		g_instVal.packChecks.load(), g_instVal.packMismatches.load() };
}

void UtilityPassReplica::RenderShadowInstanced(RE::BSRenderPass* const* a_passes, const std::uint32_t* a_techniques,
	std::uint32_t a_count, std::uint32_t a_renderFlags)
{
	(void)a_renderFlags;
	if (a_count == 0)
		return;
	auto* ctx = globals::d3d::context;  // render thread -> immediate context
	auto* S = reinterpret_cast<std::uint8_t*>(engine::S_base.address());

	// NOTE: the engine's SetupGeometry builds b2 World = transpose(SG_BuildMatrix(geom+0x7C)) with NO
	// CameraPosAdjust subtraction (UtilityPassReplica.cpp ~2338-2342); our instance pack does the same, so
	// instWorld already equals b2 byte-exact. The shadow VS uses absolute World + the absolute per-light
	// CameraViewProj -- do NOT make instWorld light-relative (that breaks the match). The over-bright bug is
	// elsewhere; the b12@DRAW probe below reads b12 at ACTUAL draw time to check for mid-loop clobber.

	// DIAGNOSTIC (per-map projection probe): classify the map (directional vs local) and log the projection
	// b12 will carry (m_ViewProjMat source @ camera block 0x1430282E0) + the bound VS permutation id. Row 3
	// distinguishes ortho (0,0,0,1) / perspective (.,.,1,0) / scale-bias placeholder; vsId tells us whether
	// the per-map permutation selection is working (directional vs spot/point should differ).
	{
		static std::atomic<int> s_mapDbg{ 0 };
		if (s_mapDbg.load() < 16) {
			s_mapDbg.fetch_add(1);
			auto*               boundVS = *reinterpret_cast<std::uint8_t**>(S + 0x348);
			const std::uint32_t vsId = boundVS ? *reinterpret_cast<std::uint32_t*>(boundVS) : 0u;
			const auto*         vp = reinterpret_cast<const float*>(REL::Offset(0x30282E0).address());
			logger::info("[MapProj] tech={:08X} dir={} count={} vsId={:08X} vpR0=({:.4f},{:.4f},{:.4f},{:.4f}) vpR3=({:.3f},{:.3f},{:.3f},{:.3f})",
				a_techniques[0], (a_techniques[0] & 0x200000u) != 0u, a_count, vsId,
				vp[0], vp[1], vp[2], vp[3], vp[12], vp[13], vp[14], vp[15]);
		}
	}

	// DEFINITIVE diagnostic: read b12's ACTUAL GPU CameraViewProj. Call at the TOP (engine-left state) AND
	// right before DrawIndexedInstanced (post FlushSetupGeometry/SetDirtyStates). If TOP is the per-map light
	// projection but DRAW is the MAIN camera, an engine call in the group loop clobbers b12 -> the instanced
	// draws project with the main camera (matches "inst looks main-camera-like / over-bright"). c8=float[32].
	auto readB12 = [&](const char* tag) {
		static std::atomic<int> s_b12Dbg{ 0 };
		if (s_b12Dbg.load() >= 24)
			return;
		s_b12Dbg.fetch_add(1);
		auto* b12 = *reinterpret_cast<ID3D11Buffer**>(REL::Offset(0x3027E88).address());
		if (!b12 || !globals::d3d::device)
			return;
		D3D11_BUFFER_DESC bd{};
		b12->GetDesc(&bd);
		static winrt::com_ptr<ID3D11Buffer> s_stg;
		static UINT                         s_stgSize = 0;
		if (!s_stg || s_stgSize < bd.ByteWidth) {
			s_stg = nullptr;
			D3D11_BUFFER_DESC sd = bd;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.BindFlags = 0;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			sd.MiscFlags = 0;
			sd.StructureByteStride = 0;
			if (SUCCEEDED(globals::d3d::device->CreateBuffer(&sd, nullptr, s_stg.put())))
				s_stgSize = bd.ByteWidth;
		}
		if (!s_stg)
			return;
		ctx->CopyResource(s_stg.get(), b12);
		D3D11_MAPPED_SUBRESOURCE m{};
		if (SUCCEEDED(ctx->Map(s_stg.get(), 0, D3D11_MAP_READ, 0, &m)) && m.pData) {
			const float* f = reinterpret_cast<const float*>(m.pData);  // c8=float[32], c40=float[160]
			logger::info("[b12@{}] tech={:08X} VP_R0=({:.4f},{:.4f},{:.4f},{:.4f}) VP_R3=({:.2f},{:.2f},{:.2f},{:.2f}) PosAdj=({:.1f},{:.1f},{:.1f})",
				tag, a_techniques[0], f[32], f[33], f[34], f[35], f[44], f[45], f[46], f[47], f[160], f[161], f[162]);
			ctx->Unmap(s_stg.get(), 0);
		}
	};
	readB12("TOP");

	// DIAGNOSTIC accounting: where do the claimed passes go? (capture showed our instanced draws never
	// reach the GPU -- ~2/3 of shadow geometry missing). Reset per map call; logged at the end.
	static struct
	{
		std::uint32_t claimed, groupedPasses, groups, drawn, instSum, fbGroups, fbPasses,
			dropType, dropRd, dropTech, dropMap, nullIL, logIL;
	} s_acct{};
	s_acct.claimed = a_count;
	s_acct.groupedPasses = s_acct.groups = s_acct.drawn = s_acct.instSum = 0;
	s_acct.fbGroups = s_acct.fbPasses = s_acct.dropType = s_acct.dropRd = 0;
	s_acct.dropTech = s_acct.dropMap = s_acct.nullIL = 0;

	// ---- group by (rendererData, technique): same mesh + technique => one instanced draw ----
	struct Group
	{
		engine::TriShapeData*            rd;
		std::uint32_t                    tech;
		std::uint8_t                     fade;      // group fade key (uniform within the group)
		std::uint32_t                    baseInst;  // start slot in the shared instance VB
		std::vector<RE::BSRenderPass*>   passes;
	};
	static std::vector<Group>                             groups;    // persistent: reuse pass-vector allocations
	static std::unordered_map<std::uint64_t, std::size_t> index;
	for (auto& g : groups)
		g.passes.clear();
	std::size_t liveGroups = 0;  // groups[0..liveGroups) are this map's
	index.clear();
	if (groups.capacity() < a_count)
		groups.reserve(a_count);
	index.reserve(a_count);
	for (std::uint32_t i = 0; i < a_count; ++i) {
		auto* geom = reinterpret_cast<std::uint8_t*>(a_passes[i]->geometry);
		if (geom[0x150] != 3) {  // whole-TRISHAPE only (sub-index / skinned excluded)
			++s_acct.dropType;
			continue;
		}
		auto* rd = *reinterpret_cast<engine::TriShapeData**>(geom + 0x138);
		if (!rd) {
			++s_acct.dropRd;
			continue;
		}
		++s_acct.groupedPasses;

		// Per-object shadow FADE (distance crossfade): SetupGeometry, for accumulationHint==10 casters,
		// selects depth-stencil variant 11 (a stencil-dither fade state) with stencil-ref = fade*31. That
		// ref is per-object and lives in the pipeline state, so it CANNOT vary within one DrawIndexedInstanced
		// -- every instance inherits passes[0]'s fade. Mixing fade levels in a group would dither-drop the
		// whole batch's depth (ref<31 => sparse depth => under-occlusion => interiors flood with light).
		// Fold the fade byte into the group key so each instanced draw is uniform-fade (fade==31 solids --
		// the vast majority -- still batch together; the thin fading shell forms its own small groups).
		std::uint8_t fadeKey = 0xFFu;  // sentinel: non-fading caster (accumulationHint != 10 => solid, ref 255)
		{
			auto* pb = reinterpret_cast<std::uint8_t*>(a_passes[i]);
			if (pb[0x1C] == 10u) {  // accumulationHint
				auto* sp = *reinterpret_cast<std::uint8_t**>(pb + 0x08);  // shaderProperty
				auto* fn = sp ? *reinterpret_cast<std::uint8_t**>(sp + 0x60) : nullptr;
				if (fn) {
					const float fade = (pb[0x1E] & 0x80) ? *reinterpret_cast<float*>(fn + 0x14C) :
					                                       *reinterpret_cast<float*>(fn + 0x130);
					fadeKey = static_cast<std::uint8_t>(static_cast<int>(fade * 31.0f));
				}
			}
		}
		const std::uint64_t k = reinterpret_cast<std::uint64_t>(rd) ^
		                        (static_cast<std::uint64_t>(a_techniques[i]) << 1) ^
		                        (static_cast<std::uint64_t>(fadeKey) << 40);
		auto                it = index.find(k);
		std::size_t         gi;
		if (it == index.end()) {
			gi = liveGroups++;
			index.emplace(k, gi);
			if (gi < groups.size()) {  // recycle slot (passes already cleared; inner vector capacity kept)
				groups[gi].rd = rd;
				groups[gi].tech = a_techniques[i];
				groups[gi].fade = fadeKey;
			} else {
				groups.push_back(Group{ rd, a_techniques[i], fadeKey, 0, {} });
			}
		} else {
			gi = it->second;
		}
		groups[gi].passes.push_back(a_passes[i]);
	}
	if (liveGroups == 0)
		return;

	// Sort (indices, not Groups -- keeps recycled inner vectors in place) by (tech, fade) so the draw loop
	// runs technique setup ONCE per run instead of per group (was ~1.7k FlushSetupTechniqueReplica/frame).
	static std::vector<std::uint32_t> order;
	order.resize(liveGroups);
	for (std::uint32_t i = 0; i < liveGroups; ++i)
		order[i] = i;
	std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
		if (groups[a].tech != groups[b].tech)
			return groups[a].tech < groups[b].tech;
		return groups[a].fade < groups[b].fade;
	});

	// ---- instance VB: one dynamic buffer sized for ALL of this map's instances (32 bytes each), filled
	// with ONE Map(DISCARD); each group draws from its baseInst via DrawIndexedInstanced's
	// StartInstanceLocation (was one Map per group = ~1.7k DISCARD slices/frame of pure overhead) ----
	static winrt::com_ptr<ID3D11Buffer> s_instVB;
	static std::uint32_t                s_instCap = 0;
	std::uint32_t                       totalInst = 0;
	for (std::uint32_t oi = 0; oi < liveGroups; ++oi) {
		groups[order[oi]].baseInst = totalInst;
		totalInst += static_cast<std::uint32_t>(groups[order[oi]].passes.size());
	}
	if (totalInst > s_instCap) {
		std::uint32_t newCap = s_instCap ? s_instCap : 4096u;
		while (newCap < totalInst)
			newCap *= 2u;
		s_instVB = nullptr;
		D3D11_BUFFER_DESC bd{};
		bd.ByteWidth = newCap * 32u;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(globals::d3d::device->CreateBuffer(&bd, nullptr, s_instVB.put())))
			return;
		s_instCap = newCap;
		Util::SetResourceName(s_instVB.get(), "ShadowInstance::InstanceVB");
	}

	// Fill the whole map's instance stream in one Map(DISCARD). Per instance: engine SG_BuildMatrix (exact
	// world build incl. quirks), then SSE transpose + F16C pack (_mm_cvtps_ph, round-to-nearest-even ==
	// XMConvertFloatToHalf) -- replaces the SG_MatrixTranspose engine call + 16 scalar half-converts per
	// instance (~100k software conversions/frame, the dominant CPU overhead of the old fill).
	{
		D3D11_MAPPED_SUBRESOURCE mappedAll{};
		if (FAILED(ctx->Map(s_instVB.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedAll))) {
			++s_acct.dropMap;
			return;
		}
		auto* outBase = reinterpret_cast<std::uint8_t*>(mappedAll.pData);
		for (std::uint32_t oi = 0; oi < liveGroups; ++oi) {
			auto&      g = groups[order[oi]];
			auto*      out = outBase + static_cast<std::size_t>(g.baseInst) * 32u;
			const auto n = static_cast<std::uint32_t>(g.passes.size());
			for (std::uint32_t i = 0; i < n; ++i, out += 32) {
				auto*             geom = reinterpret_cast<std::uint8_t*>(g.passes[i]->geometry);
				alignas(16) float m44[16];
				if (EngineCallV<16, void*>(reinterpret_cast<RE::BSGeometry*>(g.passes[i]->geometry))) {
					// AsParticlesGeom: translate = world.translate + scale*(rot*modelBound.center).
					alignas(16) float mt[16];
					auto*             gw = geom + 0x7C;
					float             r[9];
					std::memcpy(r, gw, 36);
					std::memcpy(mt, r, 36);
					const float sc = *reinterpret_cast<float*>(gw + 0x30);
					const float cx = *reinterpret_cast<float*>(geom + 0x110);
					const float cy = *reinterpret_cast<float*>(geom + 0x114);
					const float cz = *reinterpret_cast<float*>(geom + 0x118);
					mt[9] = *reinterpret_cast<float*>(gw + 0x24) + sc * (r[0] * cx + r[1] * cy + r[2] * cz);
					mt[10] = *reinterpret_cast<float*>(gw + 0x28) + sc * (r[3] * cx + r[4] * cy + r[5] * cz);
					mt[11] = *reinterpret_cast<float*>(gw + 0x2C) + sc * (r[6] * cx + r[7] * cy + r[8] * cz);
					mt[12] = sc;
					EngineCall<void>(reinterpret_cast<void*>(engine::SG_BuildMatrix.address()), static_cast<void*>(m44), static_cast<const void*>(mt));
				} else {
					EngineCall<void>(reinterpret_cast<void*>(engine::SG_BuildMatrix.address()), static_cast<void*>(m44), static_cast<const void*>(geom + 0x7C));
				}
				__m128 r0 = _mm_load_ps(m44 + 0), r1 = _mm_load_ps(m44 + 4), r2 = _mm_load_ps(m44 + 8), r3 = _mm_load_ps(m44 + 12);
				_MM_TRANSPOSE4_PS(r0, r1, r2, r3);  // == SG_MatrixTranspose
				_mm_storel_epi64(reinterpret_cast<__m128i*>(out + 0), _mm_cvtps_ph(r0, _MM_FROUND_TO_NEAREST_INT));
				_mm_storel_epi64(reinterpret_cast<__m128i*>(out + 8), _mm_cvtps_ph(r1, _MM_FROUND_TO_NEAREST_INT));
				_mm_storel_epi64(reinterpret_cast<__m128i*>(out + 16), _mm_cvtps_ph(r2, _MM_FROUND_TO_NEAREST_INT));
				_mm_storel_epi64(reinterpret_cast<__m128i*>(out + 24), _mm_cvtps_ph(r3, _MM_FROUND_TO_NEAREST_INT));
				// COMMAND-VALIDATION reference check (throttled, first instance of the first 64 groups per
				// session): recompute this record via the ENGINE scalar path (SG_MatrixTranspose + scalar
				// round-to-nearest-even halves) and byte-compare -- pins the F16C fast pack to the engine
				// reference forever (a divergence here = misplaced instanced casters).
				if (i == 0) {
					static std::atomic<int> s_refChecks{ 64 };
					if (s_refChecks.fetch_sub(1, std::memory_order_relaxed) > 0) {
						alignas(16) float           wref[16];
						DirectX::PackedVector::HALF href[16];
						EngineCall<void>(reinterpret_cast<void*>(engine::SG_MatrixTranspose.address()), static_cast<void*>(wref), static_cast<const void*>(m44));
						for (int j = 0; j < 16; ++j)
							href[j] = DirectX::PackedVector::XMConvertFloatToHalf(wref[j]);
						if (std::memcmp(href, out, 32) != 0) {
							g_instVal.packMismatches.fetch_add(1, std::memory_order_relaxed);
							static std::atomic<bool> s_logged{ false };
							if (!s_logged.exchange(true))
								logger::warn("[InstVal] F16C instance pack DIVERGES from engine-reference scalar path (geom {:p})", static_cast<void*>(geom));
						} else {
							g_instVal.packChecks.fetch_add(1, std::memory_order_relaxed);
						}
					}
				}
			}
		}
		ctx->Unmap(s_instVB.get(), 0);
	}

	// The INSTANCED Utility VS is fetched PER GROUP below (each group's exact permutation, not a hardcoded
	// one): the engine's shadow VS for a group can be plain RENDER_SHADOWMAP, the parabolic point-light
	// variant (RenderShadowmapPb), the clamped variant, or carry vertex-format / tree-wind-animation flags.
	// Instancing all of them with one plain VS drops/mis-projects the non-plain passes (point lights, etc.).

	// VA_INSTANCEDATA (attr 9) SLOT-1 presence bit ONLY. The IL key = (S+0x340) & instVS.vertexDesc feeds
	// engine ILCreate (0x140D70F90), whose VA_INSTANCEDATA branch decodes bit(44+9)=bit53 (slot-0 presence)
	// BEFORE bit(54+9)=bit63 (slot-1 presence): if bit53 is set it pins the 4 R16G16B16A16 instance elements
	// to InputSlot 0 (the MESH VB) with AlignedByteOffset = ((key>>38)&0x3C), so the 0xF offset nibble makes
	// them 60/68/76/84 -- garbage World read from the wrong buffer, plus an illegal per-vertex(POSITION) +
	// per-instance(TEXCOORD4-7) mix on one slot. Setting ONLY bit63 (no bit53, no offset nibble) gives
	// InputSlot 1 (matching s_instVB bound at slot 1) at offsets 0/8/16/24 -- exactly the 32B FP16 stream.
	// (Do NOT use the full ShaderCache::AddAttribute encoding here; that is right for a mesh-owned attribute
	// but wrong for an engine-injected per-instance slot-1 stream.)
	constexpr std::uint64_t kInstBits = (1ull << (54 + 9));

	// INSTANCED flag bit (ShaderCache::UtilityShaderFlags::Instanced). ORed onto each group's real descriptor.
	constexpr std::uint32_t kInstancedFlag = 1u << 30;

	using DirectX::PackedVector::HALF;
	using DirectX::PackedVector::XMConvertFloatToHalf;

	// A/B gate: CS_INST_FALLBACK=1 renders each claimed pass NON-instanced via ReplicaRenderPassImmediately
	// (per-pass, immediate ctx) instead of the instanced draw -- isolates the instanced DRAW from the shared
	// immediate-ctx path. If this matches vanilla but the instanced path does not, the bug is the instanced draw.
	static const bool s_forceFallback = [] { char b[8] = {}; return GetEnvironmentVariableA("CS_INST_FALLBACK", b, sizeof(b)) && b[0] && b[0] != '0'; }();
	// Default = FlushSetupGeometryReplica (last structurally-OK state). CS_INST_SETUP_ENGINE=1 swaps in the
	// real engine SetupGeometry per group for A/B (tested 2026-07-13: made mode 9 WORSE -- streaking).
	static const bool s_setupReplica = [] { char b[8] = {}; return !(GetEnvironmentVariableA("CS_INST_SETUP_ENGINE", b, sizeof(b)) && b[0] && b[0] != '0'); }();
	// CS_INST_ALPHA=1 enables the per-group SetupGeometryAlphaBlending mirror (A/B; no measured effect).
	static const bool s_alphaSetup = [] { char b[8] = {}; return GetEnvironmentVariableA("CS_INST_ALPHA", b, sizeof(b)) && b[0] && b[0] != '0'; }();

	// Draw loop over the (tech, fade)-sorted groups. Per-(tech,fade) RUN setup is hoisted -- BeginTechnique,
	// instanced-VS fetch, alpha/geometry setup, VS override happen ONCE per run (was per group: ~1.7k
	// FlushSetupTechniqueReplica + VSSetShader + SetupGeometry per frame). Groups inside a run issue only
	// their mesh binds + one DrawIndexedInstanced reading from their baseInst slot of the shared instance
	// stream. RestoreGeometry closes each run (resets the shadow-fade depth-stencil token, uniform per run
	// because fade is part of the sort key).
	std::uint32_t                 curTech = 0xFFFFFFFFu;
	std::uint8_t                  curFade = 0;
	bool                          runLive = false;      // hoisted state valid; instanced draws may proceed
	bool                          runFallback = false;  // this (tech,fade) run renders per-pass (VS compiling)
	RE::BSGraphics::VertexShader* instVS = nullptr;
	RE::BSShader*                 runShader = nullptr;
	RE::BSRenderPass*             runPass0 = nullptr;
	const auto closeRun = [&]() {
		if (runLive && runShader && runPass0)
			EngineCallV<7, void>(runShader, runPass0, a_renderFlags);  // RestoreGeometry (fade token reset)
		runLive = false;
	};
	for (std::uint32_t oi = 0; oi < liveGroups; ++oi) {
		auto&               g = groups[order[oi]];
		const std::uint32_t n = static_cast<std::uint32_t>(g.passes.size());
		if (n == 0)
			continue;
		auto*               shader = g.passes[0]->shader;
		auto*               geom0 = reinterpret_cast<std::uint8_t*>(g.passes[0]->geometry);
		auto*               rd = g.rd;
		const std::uint16_t triCount = *reinterpret_cast<const std::uint16_t*>(geom0 + 0x158);

		if (!runLive && !runFallback || g.tech != curTech || g.fade != curFade) {
			closeRun();
			curTech = g.tech;
			curFade = g.fade;
			runFallback = false;

			// 1. BeginTechnique: engine binds depth PS + blend/depth/raster + PerTechnique CB (and its VS,
			//    which we override below). Reset the caches so a fresh SetupTechnique runs.
			WsShader() = nullptr;
			WsTechnique() = 0;
			WsMaterial() = nullptr;
			if (!FlushSetupTechniqueReplica(ctx, nullptr, nullptr, S, shader, static_cast<std::int32_t>(g.tech))) {
				++s_acct.dropTech;
				runFallback = true;  // no valid technique state: render this run's groups per-pass
				instVS = nullptr;
			} else {
				// 2. Fetch the INSTANCED VS for THIS run's EXACT permutation (id | INSTANCED preserves the
				//    parabolic / clamped / wind / vertex-format variant). Lookup-first; a miss queues an async
				//    compile and the run renders per-pass until the instanced VS is ready.
				auto* boundVS = *reinterpret_cast<RE::BSGraphics::VertexShader**>(S + 0x348);
				instVS = boundVS ? ShaderCache::Instance().GetVertexShader(*shader, boundVS->id | kInstancedFlag) : nullptr;
				if (!instVS || !instVS->shader || s_forceFallback) {
					runFallback = true;
				} else {
					// Mirror ReplayOnePass' per-object alpha/depth setup (A/B-gated; no measured effect).
					if (s_alphaSetup && shader != *reinterpret_cast<RE::BSShader**>(engine::g_skyShaderInstance.address())) {
						if ((a_renderFlags & 4) && !EngineCall<bool>(reinterpret_cast<void*>(engine::IsGrassShadowBlacklist.address()), g.passes[0]->passEnum)) {
							const bool alphaTest0 = *engine::g_useEarlyZ != 0;
							auto*      ap = EngineCall<RE::NiAlphaProperty*>(reinterpret_cast<void*>(engine::GetNiProperty.address()), g.passes[0]);
							EngineCall<void>(reinterpret_cast<void*>(engine::SetupGeometryAlphaBlending.address()), shader, ap, g.passes[0]->shaderProperty, alphaTest0);
						}
					}

					// Per-geometry setup once per run, on the run's first pass, while the engine shadow VS is
					// bound: establishes (1) the shadow-FADE depth-stencil token (uniform per run -- fade is in
					// the sort key) and (2) the per-LIGHT scissor (SG_ScissorFromBBox; per light == per map, so
					// uniform across every group here). Also writes b2 World (IGNORED -- the instanced VS reads
					// World from the stream) and the ShadowFadeParam.z falloff (SHADOWMASK-only; harmless).
					if (s_setupReplica) {
						auto* vsSh = *reinterpret_cast<std::uint8_t**>(S + 0x348);
						auto* psSh = *reinterpret_cast<std::uint8_t**>(S + 0x350);
						FlushSetupGeometryReplica(ctx,
							vsSh ? *reinterpret_cast<ID3D11Buffer**>(vsSh + 0x38) : nullptr,
							psSh ? *reinterpret_cast<ID3D11Buffer**>(psSh + 0x30) : nullptr,
							S, shader, g.passes[0]);
					} else {
						EngineCallV<6, void>(shader, g.passes[0], a_renderFlags);  // engine SetupGeometry
					}

					// Override the bound VS with the INSTANCED permutation; the b2 CB stays bound at slot 2.
					EngineCallV<11, void>(ctx, reinterpret_cast<ID3D11VertexShader*>(instVS->shader), nullptr, 0u);  // VSSetShader
					*reinterpret_cast<void**>(S + 0x348) = instVS;

					runShader = shader;
					runPass0 = g.passes[0];
					runLive = true;
				}
			}
		}

		if (runFallback) {
			// Per-pass fallback (technique failed or instanced VS still compiling). ReplicaRenderPassImmediately
			// does its own full per-pass setup and clobbers the hoisted state, so the run stays non-live and any
			// following (tech,fade) change re-runs the full setup above.
			++s_acct.fbGroups;
			s_acct.fbPasses += n;
			for (std::uint32_t i = 0; i < n; ++i)
				ReplicaRenderPassImmediately(g.passes[i], g.tech, false, a_renderFlags);
			continue;
		}
		++s_acct.groups;

		// 3. renderStateVertexDesc = meshDesc | VA_INSTANCEDATA (slot-1 presence bit only); IL + topology dirty.
		auto& vertexDesc = *reinterpret_cast<std::uint64_t*>(S + 0x340);
		auto& stateFlags = *reinterpret_cast<std::uint32_t*>(S + 0);
		auto& topology = *reinterpret_cast<std::uint32_t*>(S + 0x358);
		vertexDesc = rd->vertexDesc | kInstBits;
		stateFlags |= 0x400;  // DIRTY_VERTEX_DESC -> ILCreate(meshDesc & instVS.vertexDesc)
		if (topology != 4) {
			topology = 4;  // TRIANGLELIST
			stateFlags |= 0x800;
		}

		// 4. Flush the input layout + pending render state (engine SetDirtyStates; N=1 == global block).
		//    The per-LIGHT scissor set by the run's SetupGeometry survives this -- do NOT widen it.
		EngineCall<void>(reinterpret_cast<void*>(engine::SetDirtyStates.address()), false);

		// 5. Bind mesh VB (slot 0) + shared instance VB (slot 1) and draw this group's slice of the
		//    instance stream via StartInstanceLocation (no per-group Map, no per-group VB re-fill).
		const UINT    meshStride = static_cast<UINT>((4 * rd->vertexDesc) & 0x3C);
		ID3D11Buffer* vbs[2] = { rd->vertexBuffer, s_instVB.get() };
		UINT          strides[2] = { meshStride, 32u };
		UINT          offsets[2] = { 0u, 0u };
		EngineCallV<19, void>(ctx, rd->indexBuffer, DXGI_FORMAT_R16_UINT, 0u);          // IASetIndexBuffer
		EngineCallV<18, void>(ctx, 0u, 2u, vbs, strides, offsets);                      // IASetVertexBuffers
		// DIAGNOSTIC (first draws only): null IL would silently drop the draw in D3D11/DXVK.
		if (s_acct.logIL < 4) {
			ID3D11InputLayout* il = nullptr;
			ctx->IAGetInputLayout(&il);
			if (!il)
				++s_acct.nullIL;
			else
				il->Release();
			++s_acct.logIL;
			logger::info("[InstIL] meshDesc|inst={:016X} vsDesc={:016X} key={:016X} il={} tri={} n={} base={}",
				vertexDesc, *reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const std::uint8_t*>(instVS) + 0x48),
				vertexDesc & *reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const std::uint8_t*>(instVS) + 0x48),
				il ? "OK" : "NULL", triCount, n, g.baseInst);
		}
		readB12("DRAW");  // throttled b12 probe
		EngineCallV<20, void>(ctx, 3u * triCount, n, 0u, 0, g.baseInst);                // DrawIndexedInstanced
		g_instVal.Record(rd, triCount, n, g.baseInst, g.tech, g.fade);
		++s_acct.drawn;
		s_acct.instSum += n;
	}
	closeRun();

	// COMMAND-VALIDATION invariants (pass conservation): every claimed pass must land in exactly one
	// bucket. A violation means the instanced submission dropped or duplicated shadow casters.
	//   grouped == instSum (instanced) + fbPasses (per-pass fallback)
	//   claimed == grouped + dropType + dropRd
	//   drawn   == groups (one DrawIndexedInstanced per surviving group)
	{
		g_instVal.mapsValidated.fetch_add(1, std::memory_order_relaxed);
		const bool ok = (s_acct.groupedPasses == s_acct.instSum + s_acct.fbPasses) &&
		                (s_acct.claimed == s_acct.groupedPasses + s_acct.dropType + s_acct.dropRd) &&
		                (s_acct.drawn == s_acct.groups);
		if (!ok) {
			g_instVal.invariantViolations.fetch_add(1, std::memory_order_relaxed);
			static std::atomic<bool> s_logged{ false };
			if (!s_logged.exchange(true))
				logger::warn("[InstVal] PASS-CONSERVATION VIOLATION: claimed={} grouped={} instSum={} fb={} dropType={} dropRd={} drawn={} groups={}",
					s_acct.claimed, s_acct.groupedPasses, s_acct.instSum, s_acct.fbPasses,
					s_acct.dropType, s_acct.dropRd, s_acct.drawn, s_acct.groups);
		}
	}

	// Invalidate the technique/material caches so the engine re-binds its OWN VS on the next pass
	// instead of hitting the cache and drawing through the still-bound INSTANCED VS.
	WsShader() = nullptr;
	WsTechnique() = 0;
	WsMaterial() = nullptr;

	// Full per-map accounting: every claimed pass must land in exactly one bucket (drawn instances +
	// fallback passes + drops). Any deficit = silently lost shadow casters (the under-occlusion bug).
	static std::atomic<int> s_acctLogs{ 48 };  // first 48 map calls of the session
	if (s_acctLogs.fetch_sub(1, std::memory_order_relaxed) > 0) {
		logger::info("[InstAcct] claimed={} grouped={} groups={} drawn={} instSum={} fb={}/{}p dropType={} dropRd={} dropTech={} dropMap={} nullIL={}",
			s_acct.claimed, s_acct.groupedPasses, s_acct.groups, s_acct.drawn, s_acct.instSum,
			s_acct.fbGroups, s_acct.fbPasses, s_acct.dropType, s_acct.dropRd, s_acct.dropTech,
			s_acct.dropMap, s_acct.nullIL);
	}
}

void UtilityPassReplica::ReplicaRenderSkinned(RE::BSRenderPass* a_pass, bool a_alphaTest, std::uint32_t a_renderFlags)
{
	// ---- skinned path (0x141308970), static skin-instance branch ----
	// CanReplicate already excluded the dynamic bone-setter branch (geometry vfunc 54).
	// Notable deltas vs the standard path: the per-thread dynamic accumulator at
	// TLS+10752 is zeroed on entry (FUN_14131f7c0), the alpha-test flag is used RAW
	// (no early-Z OR), and the draw goes through the skin instance's Render vfunc (37),
	// which loops partitions, uploads bone palettes via the shader's bone-setter
	// interface, and issues the per-partition draws -- all engine code, shared with the
	// passes the engine still renders itself.
	auto* shader = a_pass->shader;
	auto* geom = a_pass->geometry;
	auto* geomBytes = reinterpret_cast<std::uint8_t*>(geom);
	auto* skin = *reinterpret_cast<void**>(geomBytes + 0x130);

	*reinterpret_cast<std::uint32_t*>(engine::TlsBlock() + 10752) = 0;

	// ShaderSetup (0x141309F80) with the RAW alpha-test flag.
	if (shader != *reinterpret_cast<RE::BSShader**>(engine::g_skyShaderInstance.address())) {
		if ((a_renderFlags & 4) && !EngineCall<bool>(reinterpret_cast<void*>(engine::IsGrassShadowBlacklist.address()), a_pass->passEnum))
			EngineCall<void>(reinterpret_cast<void*>(engine::SetupGeometryAlphaBlending.address()), shader,
				EngineCall<RE::NiAlphaProperty*>(reinterpret_cast<void*>(engine::GetNiProperty.address()), a_pass),
				a_pass->shaderProperty, a_alphaTest);
		if (a_alphaTest) {
			if (auto* alphaProp = EngineCall<RE::NiAlphaProperty*>(reinterpret_cast<void*>(engine::GetNiProperty.address()), a_pass))
				EngineCall<void>(reinterpret_cast<void*>(engine::SetupAlphaTestRef.address()), shader, alphaProp, a_pass->shaderProperty);
		}
	}
	if (CsSetupMask() & 4) {
		auto* Sg = WsBlock();
		auto* vsSh = *reinterpret_cast<std::uint8_t**>(Sg + 0x348);
		auto* psSh = *reinterpret_cast<std::uint8_t**>(Sg + 0x350);
		FlushSetupGeometryReplica(WsCtx(),
			vsSh ? *reinterpret_cast<ID3D11Buffer**>(vsSh + 0x38) : nullptr,
			psSh ? *reinterpret_cast<ID3D11Buffer**>(psSh + 0x30) : nullptr,
			Sg, shader, a_pass);
	} else {
		EngineCallV<6, void>(shader, a_pass, a_renderFlags);  // SetupGeometry
	}

	// Draw-struct layout verified against the dispatcher disasm at 0x141308A05.
	const auto lodByte = reinterpret_cast<const std::uint8_t*>(a_pass)[0x1E];
	engine::SkinDrawStruct s{};
	s.boneSetter = shader ? reinterpret_cast<std::uint8_t*>(shader) + 0x10 : nullptr;
	s.geometry = geom;
	s.singleLevel = (lodByte >> 7) & 1;
	s.lodIndex = lodByte & 0x7F;
	s.dynOffset[0] = -1;

	// Dynamic-shape sub-block (skinned BSDynamicTriShape, e.g. faces): upload the
	// CPU-side dynamic vertex data into the shared dynamic ring; the ring offset lands
	// in s.dynOffset[0] and the partition draw consumes it.
	if (void* dyn = EngineCallV<12, void*>(geom)) {  // AsBSDynamicTriShape
		auto* dynBytes = reinterpret_cast<std::uint8_t*>(dyn);
		const auto size = *reinterpret_cast<const std::uint32_t*>(dynBytes + 0x170);
		void* dst = EngineCall<void*>(reinterpret_cast<void*>(engine::MapSkinDynamicData.address()),
			reinterpret_cast<void*>(engine::g_renderer.address()), size, static_cast<std::int32_t*>(s.dynOffset));
		if (dst) {
			const void* src = EngineCall<void*>(reinterpret_cast<void*>(engine::DynShapeLockData.address()), dyn);
			std::memcpy(dst, src, size);
			EngineCall<void>(reinterpret_cast<void*>(engine::UnmapSkinDynamicData.address()),
				reinterpret_cast<void*>(engine::g_renderer.address()), dst);
			EngineCall<void>(reinterpret_cast<void*>(engine::DynShapeUnlock.address()), dyn);
		}
	}

	EngineCallV<37, void>(skin, static_cast<void*>(&s));  // NiSkinInstance/BSDismember Render

	EngineCallV<7, void>(shader, a_pass, a_renderFlags);  // RestoreGeometry
}

void UtilityPassReplica::DiffWindows(RE::BSRenderPass* a_pass, std::uint32_t a_technique)
{
	// Validated state (village + Riverwood + Whiterun interior, in motion): 1.1M+ passes,
	// zero structural divergence across all three classes. The only observed diff class
	// is a one-shot CS shader-cache transition: an async compile finishing BETWEEN the
	// two windows flips the BeginTechnique PS thunk from its engine-fallback bind
	// (recorded) to CS substitution (filtered), so the engine window carries one extra
	// PSSetShader exactly once per compile completion. Benign and self-identifying.
	++passesCompared;
	// Coverage/di­vergence heartbeat so long runs report progress without a debugger.
	if ((passesCompared & 0x3FFF) == 0) {
		logger::info("[UtilityPassReplica] divergedByClass tri={} subIndex={} skinned={}",
			divergedByClass[0], divergedByClass[1], divergedByClass[2]);
		logger::info("[UtilityPassReplica] compared={} diverged={} unsupported={} (noGeom={} skin={} custom={} nonTri={} noRD={} stencil={})",
			passesCompared, passesDiverged, passesUnsupported,
			g_unsupReason[0].load(std::memory_order_relaxed), g_unsupReason[1].load(std::memory_order_relaxed),
			g_unsupReason[2].load(std::memory_order_relaxed), g_unsupReason[3].load(std::memory_order_relaxed),
			g_unsupReason[4].load(std::memory_order_relaxed), g_unsupReason[5].load(std::memory_order_relaxed));
		std::string detail = "uncoveredGeomTypes[";
		for (int i = 0; i < 16; ++i)
			if (const auto n = g_geomTypeHist[i].load(std::memory_order_relaxed))
				detail += fmt::format(" {}:{}", i, n);
		detail += " ]";
		logger::info("[UtilityPassReplica] {}", detail);
	}
	const bool sameSize = engineWindow.size() == replicaWindow.size();
	bool identical = sameSize;
	std::size_t firstDiff = 0;
	std::uint32_t firstField = 0;  // 0=kind 1=slot 2=a 3=b 4=c
	if (sameSize) {
		for (std::size_t i = 0; i < engineWindow.size(); ++i) {
			const auto& e = engineWindow[i];
			const auto& r = replicaWindow[i];
			if (e.kind != r.kind || e.slot != r.slot || e.a != r.a || e.b != r.b || e.c != r.c) {
				identical = false;
				firstDiff = i;
				firstField = e.kind != r.kind ? 0 : e.slot != r.slot ? 1 :
				             e.a != r.a          ? 2 :
				             e.b != r.b          ? 3 :
				                                   4;
				break;
			}
		}
	}
	if (identical)
		return;

	++passesDiverged;
	// Class the divergence so each coverage stage gets its own dump budget and count:
	// skinned (geom+0x130), sub-index (type 8), plain trishape.
	const auto* gb = reinterpret_cast<const std::uint8_t*>(a_pass->geometry);
	const int   cls = *reinterpret_cast<void* const*>(gb + 0x130) ? 2 : (gb[0x150] == 8 ? 1 : 0);
	++divergedByClass[cls];

	// Pinpoint the FIRST divergence since the last reset for the rerunnable parity gate
	// (the log dump budget can run out; this single record always survives to be queried).
	if (!firstDivergeCaptured) {
		firstDivergeCaptured = true;
		firstDivergeClass = static_cast<std::uint32_t>(cls);
		firstDivergeTechnique = a_technique;
		firstDivergeEngineCalls = engineWindow.size();
		firstDivergeReplicaCalls = replicaWindow.size();
		firstDivergeSizeMismatch = !sameSize;
		firstDivergeIndex = firstDiff;
		firstDivergeField = firstField;
	}
	if (dumpBudgetByClass[cls] == 0)
		return;
	--dumpBudgetByClass[cls];

	static constexpr const char* kClassNames[3] = { "trishape", "subindex", "skinned" };
	logger::warn("[UtilityPassReplica][DIFF] class={} pass={} technique=0x{:X} engineCalls={} replicaCalls={} firstDiff={}",
		kClassNames[cls], static_cast<const void*>(a_pass), a_technique, engineWindow.size(), replicaWindow.size(),
		sameSize ? std::to_string(firstDiff) : "size-mismatch");
	if (sameSize && firstDiff < engineWindow.size()) {
		const auto& ec = engineWindow[firstDiff];
		const auto& rc = replicaWindow[firstDiff];
		logger::warn("[UtilityPassReplica][DIFF-CALL] field={} E(kind={} slot={} a={:X} b={:X} c={:X}) R(kind={} slot={} a={:X} b={:X} c={:X})",
			firstField, static_cast<int>(ec.kind), ec.slot, ec.a, ec.b, ec.c,
			static_cast<int>(rc.kind), rc.slot, rc.a, rc.b, rc.c);
	}
	// If the first diverging call is a Map with snapshotted dwords (dump enabled), report the
	// exact diverging dword offsets and engine-vs-replica values -- the precise field to fix.
	if (sameSize && firstDiff < engineWindow.size()) {
		const auto& e = engineWindow[firstDiff];
		const auto& r = replicaWindow[firstDiff];
		if (e.kind == Kind::kMapDiscardData && e.mapData && r.mapData) {
			const auto& ev = *e.mapData;
			const auto& rv = *r.mapData;
			std::string s;
			for (std::size_t k = 0; k < std::max(ev.size(), rv.size()); ++k) {
				const std::uint32_t evk = k < ev.size() ? ev[k] : 0;
				const std::uint32_t rvk = k < rv.size() ? rv[k] : 0;
				if (evk != rvk)
					s += fmt::format(" dw{}: E={:08X} R={:08X};", k, evk, rvk);
			}
			logger::warn("[UtilityPassReplica][DIFF-DWORDS]{}", s);
		}
	}
	const std::size_t n = std::max(engineWindow.size(), replicaWindow.size());
	for (std::size_t i = 0; i < n && i < 64; ++i) {
		const auto* e = i < engineWindow.size() ? &engineWindow[i] : nullptr;
		const auto* r = i < replicaWindow.size() ? &replicaWindow[i] : nullptr;
		const bool  same = e && r && e->kind == r->kind && e->slot == r->slot && e->a == r->a && e->b == r->b && e->c == r->c;
		logger::warn("  [{:2}]{} E:{:<22} s={} a={:X} b={:X} c={:X} | R:{:<22} s={} a={:X} b={:X} c={:X}",
			i, same ? " " : "*",
			e ? KindName(e->kind) : "-", e ? e->slot : 0, e ? e->a : 0, e ? e->b : 0, e ? e->c : 0,
			r ? KindName(r->kind) : "-", r ? r->slot : 0, r ? r->a : 0, r ? r->b : 0, r ? r->c : 0);
	}
}

UtilityPassReplica::ValidationReport UtilityPassReplica::GetValidationReport() const
{
	ValidationReport rep;
	rep.compared = passesCompared;
	rep.diverged = passesDiverged;
	rep.divergedTrishape = divergedByClass[0];
	rep.divergedSubIndex = divergedByClass[1];
	rep.divergedSkinned = divergedByClass[2];
	rep.unsupported = passesUnsupported;
	rep.haveFirstDiverge = firstDivergeCaptured;
	rep.firstClass = firstDivergeClass;
	rep.firstTechnique = firstDivergeTechnique;
	rep.firstEngineCalls = firstDivergeEngineCalls;
	rep.firstReplicaCalls = firstDivergeReplicaCalls;
	rep.firstSizeMismatch = firstDivergeSizeMismatch;
	rep.firstDiffIndex = firstDivergeIndex;
	rep.firstDiffField = firstDivergeField;
	return rep;
}

void UtilityPassReplica::ResetValidation()
{
	passesCompared = 0;
	passesDiverged = 0;
	divergedByClass[0] = divergedByClass[1] = divergedByClass[2] = 0;
	dumpBudgetByClass[0] = 8;
	dumpBudgetByClass[1] = 24;
	dumpBudgetByClass[2] = 24;
	passesUnsupported = 0;
	firstDivergeCaptured = false;
	firstDivergeClass = 0;
	firstDivergeTechnique = 0;
	firstDivergeEngineCalls = 0;
	firstDivergeReplicaCalls = 0;
	firstDivergeSizeMismatch = false;
	firstDivergeIndex = 0;
	firstDivergeField = 0;
}

// ============================================================================================
// Concurrent shadow fan-out worker binding.
//
// A ShadowWorker owns a PRIVATE 0x5D8 render-state block + technique/material/shadow-token cache
// cells. When bound as the calling thread's t_worker, the byte-exact setup pipeline (forced to
// CS_UTIL_RE_CSSETUP=31 + flush reimpl via CsSetupMask/CsFlushRequested) reads/writes ONLY this
// worker's state and records onto its deferred context -- no shared mutable state across threads
// (skinned passes, whose bone-CB/dyn-VB rings are still global, stay on the serial remainder).
// ============================================================================================
#pragma warning(push)
#pragma warning(disable : 4324)  // structure padded due to alignas(16) on `block`
class UtilityPassReplica::ShadowWorker
{
public:
	ShadowWorkerState               state;
	alignas(16) std::uint8_t        block[engine::kSnapshotBytes]{};
	std::uint32_t                   technique = 0;
	RE::BSShader*                   shader = nullptr;
	void*                           material = nullptr;
	std::uint32_t                   shadowToken = 0;
	std::uint32_t                   techFlags = 0;
	std::uint32_t                   techSub = 0;
	std::uint8_t                    dsvDirty = 0;
};
#pragma warning(pop)

UtilityPassReplica::ShadowWorker* UtilityPassReplica::MakeShadowWorker(ID3D11DeviceContext* a_ctx, std::uint32_t a_initToken)
{
	auto* w = new ShadowWorker();
	w->shadowToken = a_initToken;
	w->state.block = w->block;
	w->state.ctx = a_ctx;
	w->state.technique = &w->technique;
	w->state.shader = &w->shader;
	w->state.material = &w->material;
	w->state.shadowToken = &w->shadowToken;
	w->state.techFlags = &w->techFlags;
	w->state.techSub = &w->techSub;
	w->state.dsvDirty = &w->dsvDirty;
	return w;
}

void UtilityPassReplica::FreeShadowWorker(ShadowWorker* a_worker)
{
	delete a_worker;
}

void UtilityPassReplica::WorkerSeedMap(ShadowWorker* a_worker, const std::uint8_t* a_mapBlock)
{
	// Seed the map's clean RT/viewport/depth block, then force a full re-bind on the first pass and
	// reset the technique/material change-detection caches so the first replayed pass runs a full
	// BeginPass/SetupTechnique (a stale cache-hit would skip setup and bind a stale VS/PS pointer).
	std::memcpy(a_worker->block, a_mapBlock, engine::kSnapshotBytes);
	*reinterpret_cast<std::uint32_t*>(a_worker->block) = 0xFFFFFFFFu;  // force all main-word dirty bits
	a_worker->technique = 0;
	a_worker->shader = nullptr;
	a_worker->material = nullptr;
	a_worker->techFlags = 0;
	a_worker->techSub = 0;
	a_worker->dsvDirty = 0;
}

void UtilityPassReplica::WorkerBeginScope(ShadowWorker* a_worker)
{
	t_worker = &a_worker->state;
}

void UtilityPassReplica::WorkerEndScope()
{
	t_worker = nullptr;
}

// ============================================================================================
// BSBatchRenderer::BeginPass (SE 1.5.97 0x141308030), reimplemented 1:1. Owns the per-group DX11
// state setup + the m_PassGroupNext pass loop + the RestoreTechnique cleanup, driving the passes
// through ReplicaRenderPassImmediately. Structural fields on the batch renderer (a1): hash table
// base @+0x48, capacity @+0x2C, sentinel @+0x38, pass-array base @+8, "release passes" flag @+0x6C.
// Group-state caches in the render-state block: main @S+0, unk_143027F4C @S+0x9C, F5C @S+0xAC,
// F64 @S+0xB4. v11 is the alpha-test flag threaded into RenderPassImmediately.
// ============================================================================================
// Hash-table lookup shared by BeginPassReplica and BeginPassCompare: resolve the batch-group id v6
// for a key. Table base @a1+0x48, capacity (pow2) @a1+0x2C, sentinel @a1+0x38; entries {key@0,val@4,
// next@8} stride 16. Returns 0 (the default group) on miss, matching the engine.
static std::uint32_t BeginPassGroupId(std::uint8_t* a1, std::uint32_t key)
{
	std::uint32_t v6 = 0;
	if (auto tbl = *reinterpret_cast<std::uintptr_t*>(a1 + 0x48)) {
		std::uintptr_t e = tbl + 16ull * (key & (*reinterpret_cast<std::uint32_t*>(a1 + 0x2C) - 1));
		if (*reinterpret_cast<std::uintptr_t*>(e + 8)) {
			const std::uintptr_t sentinel = *reinterpret_cast<std::uintptr_t*>(a1 + 0x38);
			bool                 hit = true;
			while (*reinterpret_cast<std::uint32_t*>(e) != key) {
				e = *reinterpret_cast<std::uintptr_t*>(e + 8);
				if (e == sentinel) { hit = false; break; }
			}
			if (hit)
				v6 = *reinterpret_cast<std::uint32_t*>(e + 4);
		}
	}
	return v6;
}

std::uint8_t UtilityPassReplica::BeginPassReplica(void* a_batchRenderer, void* a2, void* a3, void* a4, std::uint32_t a5)
{
	auto* const   a1 = reinterpret_cast<std::uint8_t*>(a_batchRenderer);
	const std::uint32_t key = *reinterpret_cast<std::uint32_t*>(a2);
	auto* const   groupPtr = reinterpret_cast<std::int32_t*>(a3);

	// --- 1. hash-table lookup: batch-group id v6 for this key ---
	const std::uint32_t v6 = BeginPassGroupId(a1, key);

	// --- 2. per-group DX11 state setup (block dirty bits + change caches) ---
	std::uint8_t* const S = t_worker ? t_worker->block : reinterpret_cast<std::uint8_t*>(engine::S_base.address());
	auto&               main = *reinterpret_cast<std::uint32_t*>(S + 0x00);
	auto&               f4C = *reinterpret_cast<std::uint32_t*>(S + 0x9C);
	auto&               f5C = *reinterpret_cast<std::uint32_t*>(S + 0xAC);
	auto&               f64 = *reinterpret_cast<std::uint32_t*>(S + 0xB4);
	const std::uint8_t  e5D = *engine::g_beginPassFlagE5D;
	const std::int32_t  grp = *groupPtr;
	const bool          noBlend = (a5 & 0x108) == 0;
	bool                v11 = false;  // alpha-test flag passed to RenderPassImmediately

	if (grp == 0) {
		if (noBlend && f4C != 1) { f4C = 1; main |= 0x20; }
		if (f64) { f64 = 0; main |= 0x100; }
		if (f5C) { f5C = 0; main |= 0x80; }
	} else if (grp == 2) {
		if (noBlend && f4C) { f4C = 0; main |= 0x20; }
		if (f64) { f64 = 0; main |= 0x100; }
		if (f5C) { f5C = 0; main |= 0x80; }
	} else if (grp == 3) {
		if (noBlend && f4C) { f4C = 0; main |= 0x20; }
		if (f64 != 1) { f64 = 1; main |= 0x100; }
		v11 = true;
		if (e5D && f5C != 1) { f5C = 1; main |= 0x80; }
	} else if (grp == 1) {
		if (noBlend && f4C != 1) { f4C = 1; main |= 0x20; }
		if (f64 != 1) { f64 = 1; main |= 0x100; }
		v11 = true;
		if (e5D && f5C != 1) { f5C = 1; main |= 0x80; }
	} else if (grp == 4) {
		if (noBlend && f4C != 1) { f4C = 1; main |= 0x20; }
		if (f64 != 1) { f64 = 1; main |= 0x100; }
		v11 = true;
		if (f5C) { f5C = 0; main |= 0x80; }
	}

	// --- 3. pass loop: head of (group, bucket) chain, walk m_PassGroupNext, render each ---
	auto* const passArrayBase = *reinterpret_cast<std::uint8_t**>(a1 + 8);
	auto*       pass = *reinterpret_cast<RE::BSRenderPass**>(passArrayBase + 8ull * (grp + 6ll * v6));
	while (pass) {
		// Same coverage split as mode-2's OnRenderPassImmediately: the replica owns the passes it can
		// reproduce byte-for-byte (CanReplicate) and hands everything else -- skinned-dynamic, stencil,
		// custom-render, non-trishape -- to the engine's RenderPassImmediately whole-pass. Both paths
		// share the g_currentShader/technique/material caches, so interleaving is state-consistent.
		if (t_bpSeqSink)  // verify capture: record the dispatch (see BeginPassCompare)
			t_bpSeqSink->push_back({ pass, key, static_cast<std::uint8_t>(v11), a5 });
		if (CanReplicate(pass))
			ReplicaRenderPassImmediately(pass, key, v11, a5);
		else
			RenderPassImmediately_Hook::Engine(pass, key, v11, a5);
		pass = pass->passGroupNext;  // 0x30
	}

	// --- 4. cleanup: release the group's pass list (when the renderer owns them), RestoreTechnique
	//        on the last shader, reset the current shader/technique/material caches. ---
	if (*reinterpret_cast<std::uint8_t*>(a1 + 0x6C)) {
		auto* const bucket = passArrayBase + 48ull * v6;
		*reinterpret_cast<std::uint32_t*>(bucket + 40) &= ~(1u << grp);
		*reinterpret_cast<std::uintptr_t*>(bucket + 8ull * grp) = 0;
	}
	if (auto* curShader = WsShader()) {
		RestoreTechniqueReplica(S, reinterpret_cast<std::uint8_t*>(curShader), WsTechnique());
	}
	WsShader() = nullptr;
	WsTechnique() = 0;
	WsMaterial() = nullptr;
	if (f5C) { f5C = 0; main |= 0x80; }

	// --- 5. advance the batch iterator (pure engine logic, no DX11) + return "more groups remain" ---
	++*groupPtr;
	return engine::BatchAdvance(a1, a2, a3, a4);
}

// ============================================================================================
// BeginPass-level orchestration compare (kOwnBeginPassVerify). NON-DESTRUCTIVE: the engine's BeginPass
// pops-and-FREES the group's list nodes on the release path (sub_141307E80 via BatchAdvance), so it
// must run exactly once -- a double execution would double-free and desync the caller's group loop.
// Instead we compute the replica's EXPECTED dispatch by reading the (still-intact) pass chain, then run
// the engine ONCE for real while recording its ACTUAL per-pass dispatch, and diff:
//   the ordered (pass, key=technique, alphaTest=v11, renderFlags) sequence.
// A match proves BeginPassReplica's hash lookup + m_PassGroupNext walk + v11 derivation select exactly
// the passes the engine does. The per-pass DX11 (already byte-exact via the RenderPassImmediately
// compare), the group-state block writes and the cleanup are transcribed 1:1 from the decompile and
// exercised under mode-5 rendering; this mode adds the deterministic enumeration proof on top.
// ============================================================================================
std::uint8_t UtilityPassReplica::BeginPassCompare(void* a1v, void* a2, void* a3, void* a4, std::uint32_t a5, BeginPassFn a_engine)
{
	auto* const         a1 = reinterpret_cast<std::uint8_t*>(a1v);
	const std::uint32_t key = *reinterpret_cast<std::uint32_t*>(a2);
	const std::int32_t  grp = *reinterpret_cast<std::int32_t*>(a3);

	// Replica's EXPECTED dispatch, computed by pure reads before the engine consumes the chain. v11
	// (the alpha-test flag threaded into RenderPassImmediately) depends only on the group index -- true
	// for groups 1/3/4 -- exactly as the group-state machine sets it.
	thread_local std::vector<VerifyPassRec> replicaSeq, engineSeq;
	replicaSeq.clear();
	const std::uint8_t  v11 = (grp == 1 || grp == 3 || grp == 4) ? 1u : 0u;
	const std::uint32_t v6 = BeginPassGroupId(a1, key);
	auto* const         passArrayBase = *reinterpret_cast<std::uint8_t**>(a1 + 8);
	for (auto* p = *reinterpret_cast<RE::BSRenderPass**>(passArrayBase + 8ull * (grp + 6ll * v6)); p; p = p->passGroupNext)
		replicaSeq.push_back({ p, key, v11, a5 });

	// Engine's ACTUAL dispatch: run it once (renders + advances + may free nodes). Each pass flows
	// through OnRenderPassImmediately, which appends to engineSeq while the sink is armed.
	engineSeq.clear();
	t_bpSeqSink = &engineSeq;
	const std::uint8_t ret = static_cast<std::uint8_t>(reinterpret_cast<std::uintptr_t>(a_engine(a1v, a2, a3, a4, a5)));
	t_bpSeqSink = nullptr;

	// Diff the two sequences element-by-element.
	bool        diverged = engineSeq.size() != replicaSeq.size();
	std::size_t firstDiff = SIZE_MAX;
	if (!diverged) {
		for (std::size_t i = 0; i < engineSeq.size(); ++i) {
			const auto& e = engineSeq[i];
			const auto& r = replicaSeq[i];
			if (e.pass != r.pass || e.tech != r.tech || e.alpha != r.alpha || e.flags != r.flags) {
				diverged = true;
				firstDiff = i;
				break;
			}
		}
	}

	++g_bpCompareGroups;
	if (diverged) {
		++g_bpCompareDiverged;
		if (g_bpCompareDiverged <= 32)  // cap log spam; first 32 divergences suffice to debug
			logger::warn("[BeginPassCompare] DIVERGE grp={} key={:#x} v11={} seq(engine={},replica={}) firstDiff={}",
				grp, key, v11, engineSeq.size(), replicaSeq.size(),
				firstDiff == SIZE_MAX ? -1 : static_cast<int>(firstDiff));
	}
	return ret;
}

void UtilityPassReplica::GetBeginPassCompareStats(std::uint64_t& a_groups, std::uint64_t& a_diverged) const
{
	a_groups = g_bpCompareGroups;
	a_diverged = g_bpCompareDiverged;
}

// ============================================================================================
// Regime-B MULTITHREAD draw-state gate. For one covered shadow pass, render it two ways from the SAME
// starting state and diff the per-draw effective-state fingerprints:
//   (E) the engine's RenderPassImmediately, and
//   (T) the WORKER path -- ReplicaRenderPassImmediately under a ShadowWorker with its PRIVATE 0x5D8
//       block + private caches + the full setup/flush reimplementation (CsSetupMask=31), exactly the
//       code the real N-thread MT runs, seeded (all-dirty) as on a fresh context.
// The worker renders on the IMMEDIATE context here (staging CB reads are legal), but the fingerprint is
// context-blind and the worker forces all-state-dirty regardless of context -- so T is bit-identical to
// what the worker would produce on its deferred context. E==T therefore proves the worker path binds the
// same effective state (used CB bytes + all pipeline objects) the engine does, per draw. This is the
// gate that lets the MT path be optimized freely (order/context/buffer-identity independent). Same frame,
// deterministic, no frozen scene. The double render is depth-idempotent.
// ============================================================================================
void UtilityPassReplica::VerifyPassDrawStateThreaded(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
{
	auto* const ds = vanilla::DrawStateValidator::GetSingleton();
	auto* const S = reinterpret_cast<std::uint8_t*>(engine::S_base.address());

	// A private worker on the immediate context, created once (draw-state-equivalent to a deferred worker).
	static ShadowWorker* s_verifyWorker = nullptr;
	if (!s_verifyWorker)
		s_verifyWorker = MakeShadowWorker(globals::d3d::context, 0);
	if (!s_verifyWorker)
		return;

	// Snapshot the PRE-PASS block: the map's state + the group state BeginPass just set. This seeds the
	// worker's private block (it must render from the SAME starting state as the engine). The engine
	// render is left to evolve S normally (its per-pass dirty tracking must carry into the next pass), so
	// we do NOT restore S -- only rewind the SHARED bone/dyn-VB ring cursors the worker's maps advance.
	static thread_local std::vector<std::uint8_t> snap;
	snap.assign(S, S + engine::kSnapshotBytes);

	thread_local std::vector<vanilla::DrawFingerprint> fpE, fpT;

	// (E) engine render on the immediate context -> fingerprint list E (this is the real inline render;
	// S evolves to its correct post-render state and is left there for the next pass).
	fpE.clear();
	ds->SetFingerprintSink(&fpE);
	RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
	ds->SetFingerprintSink(nullptr);
	const std::uint32_t postBone = *engine::g_boneCBRingCursor;
	const std::uint64_t postDyn = *engine::g_dynVBRingState;

	// (T) worker path: PRIVATE block seeded (all-dirty) from the pre-pass snapshot, rendered on the
	// immediate context via the exact MT code. The worker uses private caches (WsShader/WsTechFlags), so
	// it doesn't disturb the engine's global shader/technique/material caches or the shader +0x90 flags.
	WorkerSeedMap(s_verifyWorker, snap.data());
	fpT.clear();
	ds->SetFingerprintSink(&fpT);
	WorkerBeginScope(s_verifyWorker);
	ReplicaRenderPassImmediately(a_pass, a_technique, a_alphaTest, a_renderFlags);
	WorkerEndScope();
	ds->SetFingerprintSink(nullptr);

	// Undo only the worker's shared-ring advance (skinned casters map the bone/dyn-VB rings); everything
	// else the worker touched was private, and S/caches stay at the engine's post-render values.
	*engine::g_boneCBRingCursor = postBone;
	*engine::g_dynVBRingState = postDyn;

	ds->CompareFingerprints(fpE, fpT, a_pass, a_technique);
}
