#include "ShadowDeferred.h"

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

// STATUS (WIP): the mechanism runs -- the shadow loop records onto the deferred context,
// FinishCommandList + ExecuteCommandList replays it, no crash, and the private dynamic-VB
// ring keeps the shared ring uncorrupted. The missing-shadows cause was settled by a
// RenderDoc capture A/B (engine mode 0 vs deferred mode 1, frozen scene): the recorded
// draws ran with VS cb12 -- the per-frame camera constant buffer, bound once per frame
// OUTSIDE the dirty-state system -- unbound on the fresh deferred context, so every draw
// transformed garbage and wrote no depth. The scope now carries the frame-persistent
// bindings over (PersistentBindings; diagnostic capture of the live context -- the
// standalone end-state re-establishes them from RE'd engine knowledge instead), executes
// with RestoreContextState=FALSE per the DXVK deferred-context guidelines, and
// re-establishes the immediate context itself. Gated off by default (CS_SHADOW_DEFERRED
// unset).

namespace
{
	// ---------------------------------------------------------------------------------
	// 1.5.97 engine internals. "S" = RendererShadowState 0x143027EB0 (the dirty-state
	// mirror SetDirtyStates flushes). See the utility-pass RE dossier for provenance.
	// ---------------------------------------------------------------------------------
	namespace engine
	{
		// The single immediate-context global. Every engine D3D11 call re-reads it, so
		// swapping it for the shadow scope redirects the whole shadow command stream.
		inline REL::Relocation<ID3D11DeviceContext**> g_immediateContext{ REL::Offset(0x3027EA0) };

		// Render-state mirror + cross-pass caches to snapshot/restore so the immediate
		// context (returned to its pre-scope state by ExecuteCommandList RestoreContextState)
		// and this mirror stay consistent after the scope. Same span the compare harness uses.
		inline REL::Relocation<std::uint8_t*>  S_base{ REL::Offset(0x3027EB0) };
		constexpr std::uint32_t                kSnapshotBytes = 0x5D8;
		inline REL::Relocation<std::uint32_t*> g_currentTechnique{ REL::Offset(0x3283BA4) };
		inline REL::Relocation<void**>         g_currentShader{ REL::Offset(0x3283BA8) };
		inline REL::Relocation<void**>         g_currentMaterial{ REL::Offset(0x3490BB0) };
		inline REL::Relocation<std::uint32_t*> g_boneCBRingCursor{ REL::Offset(0x3027A00) };
		inline REL::Relocation<std::uint64_t*> g_dynVBRingState{ REL::Offset(0x3025F30) };
		inline REL::Relocation<std::uint32_t*> g_shadowGeomToken{ REL::Offset(0x1E10660) };

		// Dynamic-VB ring (skinned BSDynamicTriShape, e.g. faces): array base 0x143025F18,
		// three ID3D11Buffer* at [0]/[1]/[2], ring state qword at [3] (LO=current index,
		// HI=byte offset). The engine allocator FUN_140D6C8A0 (id 75484) maps with
		// WRITE_NO_OVERWRITE + a GPU query for recycling -- both illegal/unsupported on a
		// deferred context (the map returns a null base -> the caller memcpy's to offset,
		// AV at ~0x109xxx). The unmap FUN_140D6C9E0 (id 75485) already reads the redirected
		// context and unmaps buffers[currentIndex], so it needs no change.
		inline REL::Relocation<void* const*>   g_dynVBBuffers{ REL::Offset(0x3025F18) };  // [idx] = ID3D11Buffer*

		// Frame-persistent constant buffers the engine binds ONCE per frame outside the
		// per-pass dirty-state system (BSGraphics::Renderer::SetPerFrameBuffers 0x140D70290
		// + Renderer::Begin 0x140D6A0C0, both before the shadow loop) and assumes persist.
		// A fresh deferred context starts default-state, so we re-bind them ourselves from
		// the SAME engine globals the engine binds from -- a standalone re-establish, not a
		// read of the live immediate context (which would data-race once threaded).
		// RE (AddrLib 524768 / 524754, verified via SetPerFrameBuffers disasm):
		//   base[923] @ 0x143027E88 = the PerFrame camera CB (FrameBuffer.hlsli 'PerFrame',
		//     register b12) -> bound to VS b12 AND PS b12.
		//   base[783] @ 0x143027A28 = a 16-byte engine per-frame CB -> bound to PS b11 only
		//     (SetPerFrameBuffers is its ONLY binder; SetDirtyStates only updates content).
		inline REL::Relocation<ID3D11Buffer**> g_perFrameCameraCB{ REL::Offset(0x3027E88) };  // base[923], VS/PS b12
		inline REL::Relocation<ID3D11Buffer**> g_psB11CB{ REL::Offset(0x3027A28) };            // base[783], PS b11
	}

	// --- diagnostic: count DrawIndexed calls recorded on the deferred context ---
	std::atomic<std::uint64_t> g_deferredDraws{ 0 };
	using DrawIndexed_t = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
	DrawIndexed_t g_origDeferredDrawIndexed = nullptr;
	void STDMETHODCALLTYPE DrawIndexedCounter(ID3D11DeviceContext* a_ctx, UINT a_n, UINT a_start, INT a_base)
	{
		g_deferredDraws.fetch_add(1, std::memory_order_relaxed);
		g_origDeferredDrawIndexed(a_ctx, a_n, a_start, a_base);
	}

	// Detour on DrawWorld::RenderShadowmaps 0x1412E3480 -- the once-per-frame shadow-map
	// render driver (its whole body is the shadow-caster-light loop). func is the
	// trampoline to the original after DetourAttach; the body lives in the member below.
	struct RenderShadowmapsHook
	{
		static void thunk() { ShadowDeferred::GetSingleton()->RenderShadowmapsDetour(&func); }
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Deferred-safe dynamic-VB ring allocator. Only diverts while a shadow scope records;
	// normal rendering keeps the engine's NO_OVERWRITE ring. DISCARD-maps the current ring
	// buffer at offset 0 (each skinned shape gets a fresh version -- DXVK renames per
	// DISCARD, so earlier shapes' recorded draws keep referencing their own version), which
	// is the deferred-context-legal path and needs no GPU query.
	struct DynVBAllocHook
	{
		static void* thunk(void* a_renderer, std::uint32_t a_size, std::int32_t* a_outOffset)
		{
			if (!ShadowDeferred::GetSingleton()->ShadowScopeActive())
				return func(a_renderer, a_size, a_outOffset);

			auto* ctx = *engine::g_immediateContext;  // redirected to the deferred context in-scope
			const auto idx = *reinterpret_cast<const std::uint32_t*>(engine::g_dynVBRingState.address());
			auto* buf = reinterpret_cast<ID3D11Buffer* const*>(engine::g_dynVBBuffers.address())[idx];
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (!buf || FAILED(ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) {
				if (a_outOffset)
					*a_outOffset = 0;
				return nullptr;
			}
			if (a_outOffset)
				*a_outOffset = 0;
			return mapped.pData;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// DIAGNOSTIC persistent-binding carrier -- not the final design. Captures the shader
	// bindings the engine established on the immediate context OUTSIDE the dirty-state
	// system (the RenderDoc capture A/B showed the deferred shadow draws ran with VS
	// cb12 -- the per-frame camera buffer -- unbound, so every draw transformed garbage
	// and wrote no depth). The captured set is applied to the deferred context before
	// recording, and re-applied to the immediate context after ExecuteCommandList with
	// RestoreContextState=FALSE (per the DXVK deferred-context guidelines: a full restore
	// is CPU-expensive and pessimizes resource lifetime tracking; we re-establish the
	// little that matters ourselves). The standalone system must eventually NOT depend on
	// reading a live context: each binding this discovers gets traced to the engine code
	// that creates/binds it and is re-established from that understanding (see the
	// one-shot inventory log), keeping the deferred path fully self-described for the
	// eventual multithreaded recording. Skyrim binds whole buffers (no 11.1
	// first-constant ranges), so plain Get/Set pairs reproduce the bindings exactly.
	struct PersistentBindings
	{
		static constexpr UINT kCBSlots = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT;
		static constexpr UINT kSamplerSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
		static constexpr UINT kStages = 6;  // VS HS DS GS PS CS

		ID3D11Buffer*       constantBuffers[kStages][kCBSlots] = {};
		ID3D11SamplerState* samplers[kStages][kSamplerSlots] = {};

		// Get* AddRefs every returned pointer; Release() drops them once per capture.
		void Capture(ID3D11DeviceContext* a_src)
		{
			a_src->VSGetConstantBuffers(0, kCBSlots, constantBuffers[0]);
			a_src->HSGetConstantBuffers(0, kCBSlots, constantBuffers[1]);
			a_src->DSGetConstantBuffers(0, kCBSlots, constantBuffers[2]);
			a_src->GSGetConstantBuffers(0, kCBSlots, constantBuffers[3]);
			a_src->PSGetConstantBuffers(0, kCBSlots, constantBuffers[4]);
			a_src->CSGetConstantBuffers(0, kCBSlots, constantBuffers[5]);
			a_src->VSGetSamplers(0, kSamplerSlots, samplers[0]);
			a_src->HSGetSamplers(0, kSamplerSlots, samplers[1]);
			a_src->DSGetSamplers(0, kSamplerSlots, samplers[2]);
			a_src->GSGetSamplers(0, kSamplerSlots, samplers[3]);
			a_src->PSGetSamplers(0, kSamplerSlots, samplers[4]);
			a_src->CSGetSamplers(0, kSamplerSlots, samplers[5]);
		}

		void Apply(ID3D11DeviceContext* a_dst) const
		{
			a_dst->VSSetConstantBuffers(0, kCBSlots, const_cast<ID3D11Buffer* const*>(constantBuffers[0]));
			a_dst->HSSetConstantBuffers(0, kCBSlots, const_cast<ID3D11Buffer* const*>(constantBuffers[1]));
			a_dst->DSSetConstantBuffers(0, kCBSlots, const_cast<ID3D11Buffer* const*>(constantBuffers[2]));
			a_dst->GSSetConstantBuffers(0, kCBSlots, const_cast<ID3D11Buffer* const*>(constantBuffers[3]));
			a_dst->PSSetConstantBuffers(0, kCBSlots, const_cast<ID3D11Buffer* const*>(constantBuffers[4]));
			a_dst->CSSetConstantBuffers(0, kCBSlots, const_cast<ID3D11Buffer* const*>(constantBuffers[5]));
			a_dst->VSSetSamplers(0, kSamplerSlots, const_cast<ID3D11SamplerState* const*>(samplers[0]));
			a_dst->HSSetSamplers(0, kSamplerSlots, const_cast<ID3D11SamplerState* const*>(samplers[1]));
			a_dst->DSSetSamplers(0, kSamplerSlots, const_cast<ID3D11SamplerState* const*>(samplers[2]));
			a_dst->GSSetSamplers(0, kSamplerSlots, const_cast<ID3D11SamplerState* const*>(samplers[3]));
			a_dst->PSSetSamplers(0, kSamplerSlots, const_cast<ID3D11SamplerState* const*>(samplers[4]));
			a_dst->CSSetSamplers(0, kSamplerSlots, const_cast<ID3D11SamplerState* const*>(samplers[5]));
		}

		void Release()
		{
			for (auto& stage : constantBuffers)
				for (auto*& buffer : stage) {
					if (buffer)
						buffer->Release();
					buffer = nullptr;
				}
			for (auto& stage : samplers)
				for (auto*& sampler : stage) {
					if (sampler)
						sampler->Release();
					sampler = nullptr;
				}
		}

		// One-shot inventory of what the engine had bound pre-scope -- the discovery
		// output that drives the follow-up RE (trace each slot to the engine code that
		// binds it, then re-establish from that knowledge instead of capturing).
		void LogInventory() const
		{
			static constexpr const char* kStageNames[kStages] = { "VS", "HS", "DS", "GS", "PS", "CS" };
			for (UINT stage = 0; stage < kStages; ++stage) {
				std::string slots;
				for (UINT i = 0; i < kCBSlots; ++i)
					if (constantBuffers[stage][i])
						slots += std::format("{}b{}({:p})", slots.empty() ? "" : " ", i, static_cast<void*>(constantBuffers[stage][i]));
				if (!slots.empty())
					logger::info("[ShadowDeferred] pre-scope {} CBs: {}", kStageNames[stage], slots);
			}
			for (UINT stage = 0; stage < kStages; ++stage) {
				std::string slots;
				for (UINT i = 0; i < kSamplerSlots; ++i)
					if (samplers[stage][i])
						slots += std::format("{}s{}", slots.empty() ? "" : " ", i);
				if (!slots.empty())
					logger::info("[ShadowDeferred] pre-scope {} samplers: {}", kStageNames[stage], slots);
			}
		}
	};

	// STANDALONE re-establishment of the frame-persistent constant-buffer bindings, read
	// from the engine's own globals + Community Shaders' own buffers -- NOT from a live
	// context. This is the multithread-ready replacement for PersistentBindings (which
	// reads the live immediate context and would data-race once workers exist). The exact
	// set is RE-verified (see the g_perFrameCameraCB/g_psB11CB relocations and the
	// binding-inventory: VS b12; PS b4 b5 b6 b11 b12; CS b5 b6):
	//   VS b12       = engine PerFrame camera CB   (base[923])
	//   PS b11       = engine 16-byte per-frame CB (base[783])
	//   PS b12       = engine PerFrame camera CB   (base[923])
	//   PS b4/b5/b6  = CS permutation / shared / feature CB
	//   CS b5/b6     = CS shared / feature CB
	// The engine binds VS/PS b12 + PS b11 in SetPerFrameBuffers; CS binds b4/b5/b6 + CS
	// b5/b6 in Deferred::Renderer_ResetState -- both once per frame before the shadow loop.
	void BindPersistentStandalone(ID3D11DeviceContext* a_ctx)
	{
		// The relocations are always valid addresses post-load; the ID3D11Buffer* stored
		// there is created at renderer init, long before any shadow scope runs.
		ID3D11Buffer* const perFrame = *engine::g_perFrameCameraCB;
		ID3D11Buffer* const psB11 = *engine::g_psB11CB;

		auto* const state = globals::state;
		ID3D11Buffer* const permutation = (state && state->permutationCB) ? state->permutationCB->CB() : nullptr;
		ID3D11Buffer* const shared = (state && state->sharedDataCB) ? state->sharedDataCB->CB() : nullptr;
		ID3D11Buffer* const feature = (state && state->featureDataCB) ? state->featureDataCB->CB() : nullptr;

		// One-shot: log the pointers the standalone path resolved, so they can be diffed
		// against the mirror's pre-scope inventory (must be identical for provable parity).
		static bool s_logged = false;
		if (!s_logged) {
			s_logged = true;
			logger::info("[ShadowDeferred] standalone bind sources: VS/PS b12={:p} PS b11={:p} "
						 "PS/CS b4={:p} b5={:p} b6={:p}",
				static_cast<void*>(perFrame), static_cast<void*>(psB11),
				static_cast<void*>(permutation), static_cast<void*>(shared), static_cast<void*>(feature));
		}

		// VS b12
		a_ctx->VSSetConstantBuffers(12, 1, &perFrame);
		// PS b4/b5/b6 (contiguous), then b11, then b12
		ID3D11Buffer* psSharedRange[3] = { permutation, shared, feature };
		a_ctx->PSSetConstantBuffers(4, 3, psSharedRange);
		a_ctx->PSSetConstantBuffers(11, 1, &psB11);
		a_ctx->PSSetConstantBuffers(12, 1, &perFrame);
		// CS b5/b6 (contiguous)
		ID3D11Buffer* csSharedRange[2] = { shared, feature };
		a_ctx->CSSetConstantBuffers(5, 2, csSharedRange);
	}

	// A/B fallback: CS_SHADOW_MIRROR_BINDINGS=1 uses the old live-context mirror instead of
	// the standalone bind, so a scene that regresses can be diffed against the known-good
	// path. Standalone is the default (multithread-ready); remove the fallback once the
	// validation layer has confirmed parity across venues.
	bool MirrorBindingsRequested()
	{
		static const bool s_mirror = [] {
			char buf[8] = {};
			return GetEnvironmentVariableA("CS_SHADOW_MIRROR_BINDINGS", buf, sizeof(buf)) && buf[0] == '1';
		}();
		return s_mirror;
	}
}

void ShadowDeferred::RenderShadowmapsDetour(void* a_original)
{
	auto  callOriginal = *static_cast<void (**)()>(a_original);
	auto* deferred = deferredContext.get();
	auto* immediate = *engine::g_immediateContext;
	if (!IsActive() || !deferred || !immediate) {
		callOriginal();  // safety: never lose the frame's shadows
		return;
	}

	// Snapshot the render-state mirror + cross-pass caches. The engine's shadow code
	// mutates these to their post-shadow values while it records; restoring them (with
	// ExecuteCommandList RestoreContextState=TRUE returning the immediate context to its
	// pre-scope bindings) keeps the mirror and the real context consistent for the passes
	// the engine renders next on the immediate context.
	static std::vector<std::uint8_t> s_snapshot(engine::kSnapshotBytes);
	std::memcpy(s_snapshot.data(), reinterpret_cast<void*>(engine::S_base.address()), engine::kSnapshotBytes);
	const auto  savedTechnique = *engine::g_currentTechnique;
	auto* const savedShader = *engine::g_currentShader;
	auto* const savedMaterial = *engine::g_currentMaterial;
	const auto  savedBoneCursor = *engine::g_boneCBRingCursor;
	const auto  savedDynVB = *engine::g_dynVBRingState;
	const auto  savedShadowToken = *engine::g_shadowGeomToken;

	// Redirect BOTH context pointers to the deferred context: the engine's (0x143027EA0)
	// and CS's cached copy (globals::d3d::context). While the scope is active the CS
	// BeginTechnique substitution hooks bind engine-original shaders through the engine
	// path (see Hooks.cpp), and any stray CS D3D during a shadow pass also lands on the
	// deferred context rather than the live immediate one.
	// Swap the engine's dynamic-VB ring-buffer pointers to a private buffer for the scope,
	// so the shadow path's deferred DISCARD maps never corrupt the shared ring the
	// immediate main-view pass reuses. Lazily created from the engine buffer's own desc.
	auto** const  ringSlots = reinterpret_cast<ID3D11Buffer**>(engine::g_dynVBBuffers.address());
	ID3D11Buffer* savedRing[3] = { ringSlots[0], ringSlots[1], ringSlots[2] };
	if (!privateDynVB && savedRing[0]) {
		D3D11_BUFFER_DESC desc{};
		savedRing[0]->GetDesc(&desc);
		if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, privateDynVB.put())))
			privateDynVB = nullptr;
		else
			Util::SetResourceName(privateDynVB.get(), "ShadowDeferred::PrivateDynVB");
	}
	if (privateDynVB) {
		ringSlots[0] = ringSlots[1] = ringSlots[2] = privateDynVB.get();
	}

	auto* const savedCsContext = globals::d3d::context;
	*engine::g_immediateContext = deferred;
	globals::d3d::context = deferred;
	scopeActive = true;

	// Force the main pipeline state dirty so the FIRST shadow pass fully re-establishes it
	// on the fresh deferred context. The engine's SetDirtyStates only re-binds CHANGED
	// state, assuming the rest persisted from earlier immediate-context passes this frame
	// -- which a deferred context (default-state at creation and after each
	// ExecuteCommandList) does NOT have, so without this the shadow draws render with a
	// stale/default render target + depth-stencil/raster/viewport and write empty depth (no
	// shadows). Only the main word 0x143027EB0 (RT/depth/raster/viewport/blend/IA/topology,
	// all of which persist across passes within a shadow map) is forced: SetDirtyStates
	// processes just its known bits from valid caches and clears the word, and the shadow
	// passes re-bind their own shaders/CBs/SRVs/samplers per pass. The per-stage SRV/sampler
	// and CS-UAV dirty words are NOT forced -- their caches hold null/garbage for stages the
	// shadow path doesn't use, which crashes the bind. The end-of-scope restore undoes this.
	// A fresh deferred context starts with a 0x0 viewport AND an empty scissor rect. Skyrim
	// uses scissor rects with scissor-test enabled, so without a scissor set every shadow
	// draw is clipped away (writing no depth) while ClearDepthStencilView -- which ignores
	// viewport/scissor -- still clears the map, exactly matching the observed "shadows
	// cleared but never drawn". Seed a max-size viewport + scissor; the engine's own
	// UpdateViewPort overrides the viewport per shadow map, and the scissor stays wide open.
	// Capture the immediate context's ACTUAL pre-scope viewport/scissor (also reused to
	// re-establish the immediate context after the state-clearing execute below) -- the
	// engine enters its shadow rendering with whatever these were left at, and per-map
	// code overrides the viewport through the forced-dirty path. The wide-open seed is
	// the fallback for the edge case where nothing was set: a fresh deferred context
	// starts with a 0x0 viewport and an empty scissor, which clips every draw away.
	UINT           savedViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_VIEWPORT savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	immediate->RSGetViewports(&savedViewportCount, savedViewports);
	UINT       savedScissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_RECT savedScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	immediate->RSGetScissorRects(&savedScissorCount, savedScissors);
	{
		const D3D11_VIEWPORT vp{ 0.0f, 0.0f, 16384.0f, 16384.0f, 0.0f, 1.0f };
		const D3D11_RECT     sc{ 0, 0, 16384, 16384 };
		deferred->RSSetViewports(1, &vp);
		deferred->RSSetScissorRects(1, &sc);
		if (savedViewportCount)
			deferred->RSSetViewports(savedViewportCount, savedViewports);
		if (savedScissorCount)
			deferred->RSSetScissorRects(savedScissorCount, savedScissors);
	}

	// Establish the frame-persistent constant-buffer bindings (per-frame camera CB in VS
	// b12 et al.) that the engine binds on the immediate context before this scope and
	// never re-binds per pass. Default path is STANDALONE (read from the engine/CS globals,
	// no live-context read -- multithread-ready). CS_SHADOW_MIRROR_BINDINGS=1 falls back to
	// the diagnostic live-context mirror for A/B. The mirror also captured the one-shot
	// inventory that drove this RE; keep it on frame 0 for continued verification.
	PersistentBindings persistentBindings;
	const bool          mirrorBindings = MirrorBindingsRequested();
	if (mirrorBindings || framesDeferred == 0) {
		persistentBindings.Capture(immediate);
		if (framesDeferred == 0)
			persistentBindings.LogInventory();
	}
	if (mirrorBindings)
		persistentBindings.Apply(deferred);
	else
		BindPersistentStandalone(deferred);

	// Retarget the perf-annotation interface at the deferred context for the scope so the
	// engine's frame-annotation hooks ("Directional Light Shadowmaps" etc.) record their
	// labels INSIDE the command list -- on the immediate context they would bracket
	// nothing and the executed draws would be unlabeled. Restored after the scope.
	globals::state->SetPerfAnnotationContext(deferred);
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("ShadowDeferred: deferred shadow-map recording");

	auto* const sflags = reinterpret_cast<std::uint32_t*>(engine::S_base.address());
	sflags[0] = 0xFFFFFFFFu;  // 0x143027EB0 main state flags. The RenderDoc deferred-vs-engine
	                          // capture A/B settled the earlier "state or draws?" question: the
	                          // draws ran with VS cb12 (per-frame camera CB, bound outside the
	                          // dirty system) missing -- persistent bindings are mirrored above,
	                          // while this force covers only the dirty-tracked state.

	callOriginal();

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
	// Rebind annotations to the immediate context before it resumes the frame.
	globals::state->SetPerfAnnotationContext(immediate);

	scopeActive = false;
	globals::d3d::context = savedCsContext;
	*engine::g_immediateContext = immediate;

	ringSlots[0] = savedRing[0];
	ringSlots[1] = savedRing[1];
	ringSlots[2] = savedRing[2];

	// Close the recording and replay it in place. RestoreContextState=FALSE on BOTH calls
	// per the DXVK deferred-context guidelines (a full restore is CPU-expensive and
	// pessimizes resource lifetime tracking). Execute-with-FALSE leaves the immediate
	// context CLEARED, so re-establish what the engine assumes persists ourselves:
	// the frame-persistent bindings captured at scope entry, the pre-scope viewport /
	// scissor, and -- after the mirror snapshot restore below -- a forced-dirty main
	// state word so the engine's next SetDirtyStates re-binds everything it tracks from
	// the (restored) mirror caches. The command list is released immediately after
	// execution (com_ptr scope) so its resources drop early, also per the guidelines.
	const auto drawsThisScope = g_deferredDraws.exchange(0, std::memory_order_relaxed);
	{
		winrt::com_ptr<ID3D11CommandList> commandList;
		if (SUCCEEDED(deferred->FinishCommandList(FALSE, commandList.put())) && commandList) {
			immediate->ExecuteCommandList(commandList.get(), FALSE);
			++framesDeferred;
			if ((framesDeferred & 0x3F) == 1)
				logger::info("[ShadowDeferred] frame {}: recorded {} DrawIndexed on the deferred context", framesDeferred, drawsThisScope);
		} else {
			logger::error("[ShadowDeferred] FinishCommandList failed; shadows this frame are lost");
		}
	}

	// Re-establish the immediate context after the state-clearing execute: the
	// frame-persistent bindings and the pre-scope viewport/scissor captured at entry.
	if (mirrorBindings)
		persistentBindings.Apply(immediate);
	else
		BindPersistentStandalone(immediate);
	persistentBindings.Release();  // no-op when nothing was captured
	if (savedViewportCount)
		immediate->RSSetViewports(savedViewportCount, savedViewports);
	if (savedScissorCount)
		immediate->RSSetScissorRects(savedScissorCount, savedScissors);

	// Restore the render-state mirror + caches to their pre-scope values, then force the
	// main dirty word so the engine's next SetDirtyStates re-binds the dirty-tracked
	// state (render targets, raster, viewport, IA) onto the cleared immediate context.
	std::memcpy(reinterpret_cast<void*>(engine::S_base.address()), s_snapshot.data(), engine::kSnapshotBytes);
	*engine::g_currentTechnique = savedTechnique;
	*engine::g_currentShader = savedShader;
	*engine::g_currentMaterial = savedMaterial;
	*engine::g_boneCBRingCursor = savedBoneCursor;
	*engine::g_dynVBRingState = savedDynVB;
	*engine::g_shadowGeomToken = savedShadowToken;
	sflags[0] = 0xFFFFFFFFu;
}

void ShadowDeferred::Setup()
{
	char buf[8] = {};
	if (GetEnvironmentVariableA("CS_SHADOW_DEFERRED", buf, sizeof(buf)) && buf[0]) {
		const int v = atoi(buf);
		if (v >= 0 && v <= 1)
			mode.store(static_cast<Mode>(v), std::memory_order_relaxed);
	}
	if (!IsActive())
		return;

	// The engine addresses below are 1.5.97 (SE) offsets; the shadow-loop layout and
	// globals differ on AE. Restrict to SE until AE addresses are mapped.
	if (REL::Module::IsAE()) {
		logger::warn("[ShadowDeferred] AE runtime detected; SE-only for now, disabling");
		mode.store(Mode::kOff, std::memory_order_relaxed);
		return;
	}

	auto* device = globals::d3d::device;
	if (!device) {
		logger::error("[ShadowDeferred] no D3D11 device; disabling");
		mode.store(Mode::kOff, std::memory_order_relaxed);
		return;
	}

	// Native D3D11 supports deferred contexts + command lists directly. A deferred context
	// starts with default state, so the shadow recording sets every state it needs
	// explicitly -- exactly what a privately-stated, eventually-threaded path must do.
	const HRESULT hr = device->CreateDeferredContext(0, deferredContext.put());
	if (FAILED(hr) || !deferredContext) {
		logger::error("[ShadowDeferred] CreateDeferredContext failed (0x{:08X}); disabling", static_cast<std::uint32_t>(hr));
		mode.store(Mode::kOff, std::memory_order_relaxed);
		return;
	}
	Util::SetResourceName(deferredContext.get(), "ShadowDeferred::DeferredContext");

	// Diagnostic: patch the deferred context's own vtable DrawIndexed (index 12) to count
	// recorded draws. The deferred context is a distinct DXVK class from the immediate one,
	// so this touches only our context.
	{
		auto** vtbl = *reinterpret_cast<void***>(deferredContext.get());
		DWORD  oldProtect = 0;
		if (VirtualProtect(&vtbl[12], sizeof(void*), PAGE_READWRITE, &oldProtect)) {
			g_origDeferredDrawIndexed = reinterpret_cast<DrawIndexed_t>(vtbl[12]);
			vtbl[12] = reinterpret_cast<void*>(&DrawIndexedCounter);
			VirtualProtect(&vtbl[12], sizeof(void*), oldProtect, &oldProtect);
		}
	}

	InstallHooks();
	logger::info("[ShadowDeferred] active, mode={} (deferred context ready)", static_cast<std::uint32_t>(GetMode()));
}

void ShadowDeferred::InstallHooks()
{
	if (hooksInstalled)
		return;
	hooksInstalled = true;

	// Detour the shadow-map render driver DrawWorld::RenderShadowmaps (AddressLibrary id
	// 100420 = 0x1412E3480, verified in offsets-1-5-97-0.csv and the RE dossier Part III).
	stl::detour_thunk<RenderShadowmapsHook>(REL::RelocationID(100420, 0));
	// Deferred-safe dynamic-VB ring allocator (id 75484); inert outside the shadow scope.
	stl::detour_thunk<DynVBAllocHook>(REL::RelocationID(75484, 0));
	logger::info("[ShadowDeferred] detoured RenderShadowmaps @ 0x{:X}", RenderShadowmapsHook::func.address());
}
