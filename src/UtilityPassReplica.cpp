#include "UtilityPassReplica.h"

#include "Globals.h"
#include "State.h"

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

	// Set only while the ENGINE window records: drop D3D11 calls a CS feature injects on
	// the engine's draw leaf (return address inside our own module). The REPLICA window
	// is never filtered -- it hand-codes the draw leaf (so no CS injection) and reaches
	// the engine setup functions by direct call, whose tail-called binds legitimately
	// report a CS return address that must NOT be dropped.
	bool g_filterCs = false;

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
	// layout (Map=14/Unmap=15 already proven safe by ParallelShaderSetup's hooks).
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

	// Engine helpers still called in Stage A (replaced in later stages).
	using SetDirtyStates_t = void (*)(bool);
	inline REL::Relocation<SetDirtyStates_t> SetDirtyStates{ REL::Offset(0xD705B0) };
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
}

void UtilityPassReplica::EndWindow()
{
	g_sink = nullptr;
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

		// Ground truth first: the engine renders and we record its command window. Filter
		// on: CS features that hook the engine's draw leaf inject their own binds/CBs on the
		// real render; those are overlays, not the engine command stream we replicate, so a
		// call whose return address lands in our own module is dropped from this window.
		g_filterCs = true;
		BeginWindow(engineWindow);
		RenderPassImmediately_Hook::Engine(a_pass, a_technique, a_alphaTest, a_renderFlags);
		EndWindow();
		g_filterCs = false;

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

		// Replica window: NEVER filtered. It hand-codes the draw leaf (so no CS injection)
		// and calls the engine setup functions directly; their tail-called binds report a
		// CS return address that is legitimate and must be recorded.
		BeginWindow(replicaWindow);
		ReplicaRenderPassImmediately(a_pass, a_technique, a_alphaTest, a_renderFlags);
		EndWindow();

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
	if (!geom)
		return false;
	const auto* geomBytes = reinterpret_cast<const std::uint8_t*>(geom);
	if (*reinterpret_cast<void* const*>(geomBytes + 0x130))  // skin instance
		return false;
	if (geomBytes[0x109] & 8)  // needs-custom-render
		return false;
	if (geomBytes[0x150] != 3)  // GeometryType TRISHAPE
		return false;
	if (!*reinterpret_cast<void* const*>(geomBytes + 0x138))  // rendererData
		return false;
	// STENCIL_ABOVE_WATER releases the bound PS on first use -- running it twice in
	// compare mode would double-Release. Excluded until the replica owns that path.
	const std::uint32_t f = a_pass->passEnum - 0x2B;
	if ((f & 0x1200) == 0x1200)
		return false;
	return true;
}

void UtilityPassReplica::ReplicaRenderPassImmediately(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
{
	// ---- RenderPassImmediately body (1.5.97 0x141308440), replicated ----
	auto* shader = a_pass->shader;
	auto* geom = a_pass->geometry;

	// Technique cache: BeginPass only when the technique or shader changed. 0x5C006076
	// never caches (the engine forces re-setup for that sentinel technique).
	const bool cached = *engine::g_currentTechnique == a_technique &&
	                    a_technique != 0x5C006076 &&
	                    shader == *engine::g_currentShader;
	if (!cached) {
		*engine::g_debugTechnique = a_technique;
		// BeginPass (0x1413086C0): RestoreTechnique on the outgoing shader, clear the
		// caches, SetupTechnique on the incoming one, then re-stamp the caches.
		if (auto* prev = *engine::g_currentShader)
			prev->RestoreTechnique(*engine::g_currentTechnique);
		*engine::g_currentShader = nullptr;
		*engine::g_currentTechnique = 0;
		*engine::g_currentMaterial = nullptr;
		if (!shader->SetupTechnique(a_technique))
			return;  // engine bails the whole pass on setup failure
		*engine::g_currentShader = shader;
		*engine::g_currentTechnique = a_technique;
	}

	// Material change detection (cache at 0x143490BB0).
	void* material = a_pass->shaderProperty ? *reinterpret_cast<void* const*>(
												  reinterpret_cast<const std::uint8_t*>(a_pass->shaderProperty) + 0x78) :
	                                          nullptr;
	if (material != *engine::g_currentMaterial) {
		if (material)
			shader->SetupMaterial(static_cast<RE::BSShaderMaterial*>(material));
		*engine::g_currentMaterial = material;
	}


	// ucCurrentMeshLODLevel: the walk stamps the pass's LOD index onto the geometry.
	auto* geomBytes = reinterpret_cast<std::uint8_t*>(geom);
	geomBytes[0x108] = static_cast<std::uint8_t>(a_pass->LODMode.index);

	// ---- _Standard path (0x1413088C0): ShaderSetup -> Draw -> RestoreGeometry ----
	const bool alphaTest = a_alphaTest || *engine::g_useEarlyZ != 0;

	// ShaderSetup (0x141309F80): alpha-blend + alpha-test-ref setup, then SetupGeometry.
	if (shader != *reinterpret_cast<RE::BSShader**>(engine::g_skyShaderInstance.address())) {
		if ((a_renderFlags & 4) && !engine::IsGrassShadowBlacklist(a_pass->passEnum))
			engine::SetupGeometryAlphaBlending(shader, engine::GetNiProperty(a_pass), a_pass->shaderProperty, alphaTest);
		if (alphaTest) {
			if (auto* alphaProp = engine::GetNiProperty(a_pass))
				engine::SetupAlphaTestRef(shader, alphaProp, a_pass->shaderProperty);
		}
	}
	shader->SetupGeometry(a_pass, a_renderFlags);

	// ---- Draw, TRISHAPE leaf (0x141307160 case 2 -> DrawTriShape 0x140D6BFE0) ----
	{
		auto* rd = *reinterpret_cast<engine::TriShapeData**>(geomBytes + 0x138);
		const std::uint16_t triCount = *reinterpret_cast<const std::uint16_t*>(geomBytes + 0x158);

		if (*engine::S_vertexDesc != rd->vertexDesc) {
			*engine::S_vertexDesc = rd->vertexDesc;
			*engine::S_stateUpdateFlags |= 0x400;  // DIRTY_VERTEX_DESC
		}
		if (*engine::S_topology != 4 /*D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST*/) {
			*engine::S_topology = 4;
			*engine::S_stateUpdateFlags |= 0x800;  // DIRTY_PRIMITIVE_TOPO
		}
		engine::SetDirtyStates(false);

		auto*      ctx = globals::d3d::context;
		const UINT stride = static_cast<UINT>((4 * rd->vertexDesc) & 0x3C);
		const UINT offset = 0;
		ctx->IASetIndexBuffer(rd->indexBuffer, DXGI_FORMAT_R16_UINT, 0);
		ctx->IASetVertexBuffers(0, 1, &rd->vertexBuffer, &stride, &offset);
		ctx->DrawIndexed(3u * triCount, 0, 0);
	}

	shader->RestoreGeometry(a_pass, a_renderFlags);
}

void UtilityPassReplica::DiffWindows(RE::BSRenderPass* a_pass, std::uint32_t a_technique)
{
	// KNOWN RESIDUAL (~5% of passes, all in the force-all-dirty state where S->flags,
	// EB4 and EB8 read 0xFFFFFFFF/0xFFFF at the pass): the replica emits one extra
	// OMSetBlendState the engine window omits. Verified (topology reads 4, so the address
	// base is right) that S->flags stays 0xFFFFFFFF through the replica's entire setup --
	// no engine setup call clears the alpha-blend dirty bit (0x80). The engine window
	// reaches DrawTriShape with 0x80 already clear, which only a CS feature hooking the
	// engine's draw leaf (the same one injecting the filtered 64B CB) can be doing -- it
	// touches blend state on the real render, and the hand-coded DrawTriShape bypasses it.
	// The replica therefore reproduces the PURE engine command stream; the "extra" call is
	// a correct, redundant re-application of the already-correct blend state. Left as-is:
	// closing it means replicating the CS draw-leaf hook, which belongs to REPLACE mode.
	++passesCompared;
	// Coverage/di­vergence heartbeat so long runs report progress without a debugger.
	if ((passesCompared & 0x3FFF) == 0)
		logger::info("[UtilityPassReplica] compared={} diverged={} unsupported={}",
			passesCompared, passesDiverged, passesUnsupported);
	const bool sameSize = engineWindow.size() == replicaWindow.size();
	bool identical = sameSize;
	std::size_t firstDiff = 0;
	if (sameSize) {
		for (std::size_t i = 0; i < engineWindow.size(); ++i) {
			const auto& e = engineWindow[i];
			const auto& r = replicaWindow[i];
			if (e.kind != r.kind || e.slot != r.slot || e.a != r.a || e.b != r.b || e.c != r.c) {
				identical = false;
				firstDiff = i;
				break;
			}
		}
	}
	if (identical)
		return;

	++passesDiverged;
	if (divergenceLogBudget == 0)
		return;
	--divergenceLogBudget;

	logger::warn("[UtilityPassReplica][DIFF] pass={} technique=0x{:X} engineCalls={} replicaCalls={} firstDiff={}",
		static_cast<const void*>(a_pass), a_technique, engineWindow.size(), replicaWindow.size(),
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
