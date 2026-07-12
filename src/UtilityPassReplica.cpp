#include "UtilityPassReplica.h"

#include "Globals.h"
#include "State.h"

#include <intrin.h>
#include <winrt/base.h>

#include <RE/B/BSRenderPass.h>
#include <RE/B/BSShader.h>
#include <RE/B/BSUtilityShader.h>

namespace
{
	using Recorded = UtilityPassReplica::RecordedCall;
	using Kind = UtilityPassReplica::RecordedCall::Kind;

	// Active recording sink (render thread only; null = recorder disarmed and every
	// hook below is a single-branch passthrough).
	std::vector<Recorded>* g_sink = nullptr;

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
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
				Record(Kind::kVSSetConstantBuffers, static_cast<std::uint16_t>(slot), n, HashPointers(bufs, n));
			func(ctx, slot, n, bufs);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PSSetShaderResources_Hook  // vfunc 8
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, UINT slot, UINT n, ID3D11ShaderResourceView* const* views)
		{
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
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
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
				Record(Kind::kPSSetShader, 0, reinterpret_cast<std::uint64_t>(ps));
			func(ctx, ps, inst, n);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PSSetSamplers_Hook  // vfunc 10
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, UINT slot, UINT n, ID3D11SamplerState* const* samplers)
		{
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
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
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
				Record(Kind::kVSSetShader, 0, reinterpret_cast<std::uint64_t>(vs));
			func(ctx, vs, inst, n);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct DrawIndexed_Hook  // vfunc 12
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex)
		{
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
				Record(Kind::kDrawIndexed, 0, indexCount, startIndex, static_cast<std::uint64_t>(static_cast<std::int64_t>(baseVertex)));
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
				if (!g_filterCs || EngineCaller(_ReturnAddress())) {
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
						if (!g_filterCs || EngineCaller(_ReturnAddress()))
							Record(Kind::kMapDiscardData, 0, reinterpret_cast<std::uint64_t>(res), n, HashBytes(slotEntry.data, n));
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
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
				Record(Kind::kPSSetConstantBuffers, static_cast<std::uint16_t>(slot), n, HashPointers(bufs, n));
			func(ctx, slot, n, bufs);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct IASetInputLayout_Hook  // vfunc 17
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, ID3D11InputLayout* layout)
		{
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
				Record(Kind::kIASetInputLayout, 0, reinterpret_cast<std::uint64_t>(layout));
			func(ctx, layout);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct IASetVertexBuffers_Hook  // vfunc 18
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, UINT slot, UINT n, ID3D11Buffer* const* bufs, const UINT* strides, const UINT* offsets)
		{
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
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
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
				Record(Kind::kIASetIndexBuffer, 0, reinterpret_cast<std::uint64_t>(buf), fmt, offset);
			func(ctx, buf, fmt, offset);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct IASetPrimitiveTopology_Hook  // vfunc 24
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, D3D11_PRIMITIVE_TOPOLOGY topo)
		{
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
				Record(Kind::kIASetPrimitiveTopology, 0, topo);
			func(ctx, topo);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct VSSetShaderResources_Hook  // vfunc 25
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, UINT slot, UINT n, ID3D11ShaderResourceView* const* views)
		{
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
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
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
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
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
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
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
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
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
				Record(Kind::kOMSetDepthStencilState, 0, reinterpret_cast<std::uint64_t>(state), stencilRef);
			func(ctx, state, stencilRef);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct RSSetState_Hook  // vfunc 43
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, ID3D11RasterizerState* state)
		{
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
				Record(Kind::kRSSetState, 0, reinterpret_cast<std::uint64_t>(state));
			func(ctx, state);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct RSSetViewports_Hook  // vfunc 44
	{
		static void __stdcall thunk(ID3D11DeviceContext* ctx, UINT n, const D3D11_VIEWPORT* viewports)
		{
			if (!g_filterCs || EngineCaller(_ReturnAddress()))
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
	inline REL::Relocation<std::uint8_t*>  g_focusShadowEnable{ REL::Offset(0x1E0DE43) };
	inline REL::Relocation<std::uint32_t*> g_focusShadowCount{ REL::Offset(0x1D0FB8) };
	inline REL::Relocation<std::uint8_t**> g_viewCamera{ REL::Offset(0x1D0E68) };
	inline REL::Relocation<std::uint8_t**> g_shadowSceneNode{ REL::Offset(0x1E0DED0) };
	inline REL::Relocation<std::uint32_t*> g_shadowFixedCount{ REL::Offset(0x1867188) };
	inline REL::Relocation<std::uint8_t*>  g_copySplitToVS{ REL::Offset(0x1E0DE4C) };
	inline REL::Relocation<std::uint32_t*> g_shadowMode{ REL::Offset(0x1E0DE34) };
	inline REL::Relocation<std::uint32_t*> g_poissonDenom{ REL::Offset(0x3283B90) };
	inline REL::Relocation<float*>         g_poissonRadiusScale{ REL::Offset(0x1E10670) };
	inline REL::Relocation<float*>         g_fixedSplit{ REL::Offset(0x3283B78) };
	inline REL::Relocation<float*>         g_maxFocusDist{ REL::Offset(0x1E106B8) };
	inline REL::Relocation<float**>        g_focusShadowData{ REL::Offset(0x1D0FA8) };
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
	inline REL::Relocation<std::uintptr_t> SG_pCamNode{ REL::Offset(0x1D0F88) };
	inline REL::Relocation<std::uintptr_t> SG_pViewFrustumObj{ REL::Offset(0x1D0E68) };
	inline REL::Relocation<std::uintptr_t> SG_pFadeExclude{ REL::Offset(0x1D0DA8) };
	inline REL::Relocation<std::uintptr_t> SG_mode1D0E28{ REL::Offset(0x1D0E28) };
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
	};
	thread_local ShadowWorkerState* t_worker = nullptr;

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
}

void UtilityPassReplica::EndWindow()
{
	g_sink = nullptr;
	g_filterCs = false;
}

void UtilityPassReplica::OnRenderPassImmediately(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
{
	// Only utility passes are in scope; everything else is always the engine's.
	const bool isUtility = a_pass && a_pass->shader &&
	                       a_pass->shader->shaderType.get() == RE::BSShader::Type::Utility;
	if (!isUtility || GetMode() == Mode::kOff) {
		RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
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

		// Ground truth first: the engine renders and we record its command window
		// (BeginWindow arms the symmetric CS-injection filter).
		BeginWindow(engineWindow);
		RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
		EndWindow();

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
		g_stubActive = true;
		BeginWindow(replicaWindow);
		ReplicaRenderPassImmediately(a_pass, a_technique, a_alphaTest, a_renderFlags);
		EndWindow();
		g_stubActive = false;

		DiffWindows(a_pass, a_technique);
		return;
	}

	// Mode::kReplace: the engine is switched off for this pass.
	ReplicaRenderPassImmediately(a_pass, a_technique, a_alphaTest, a_renderFlags);
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
		if (auto* prev = WsShader())
			EngineCallV<3, void>(prev, WsTechnique());  // RestoreTechnique
		WsShader() = nullptr;
		WsTechnique() = 0;
		WsMaterial() = nullptr;
		if (!EngineCallV<2, bool>(shader, a_technique))  // SetupTechnique
			return;  // engine bails the whole pass on setup failure
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

	// ShaderSetup (0x141309F80): alpha-blend + alpha-test-ref setup, then SetupGeometry.
	if (shader != *reinterpret_cast<RE::BSShader**>(engine::g_skyShaderInstance.address())) {
		if ((a_renderFlags & 4) && !EngineCall<bool>(reinterpret_cast<void*>(engine::IsGrassShadowBlacklist.address()), a_pass->passEnum))
			EngineCall<void>(reinterpret_cast<void*>(engine::SetupGeometryAlphaBlending.address()), shader,
				EngineCall<RE::NiAlphaProperty*>(reinterpret_cast<void*>(engine::GetNiProperty.address()), a_pass),
				a_pass->shaderProperty, alphaTest);
		if (alphaTest) {
			if (auto* alphaProp = EngineCall<RE::NiAlphaProperty*>(reinterpret_cast<void*>(engine::GetNiProperty.address()), a_pass))
				EngineCall<void>(reinterpret_cast<void*>(engine::SetupAlphaTestRef.address()), shader, alphaProp, a_pass->shaderProperty);
		}
	}
	EngineCallV<6, void>(shader, a_pass, a_renderFlags);  // SetupGeometry

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
		return s_on;
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
		return s_mask;
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

		const std::uint32_t flags = *reinterpret_cast<std::uint32_t*>(shader + 0x90);
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
}

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
	EngineCallV<6, void>(shader, a_pass, a_renderFlags);  // SetupGeometry

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
