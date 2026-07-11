#include "ShadowDeferred.h"

#include "Globals.h"

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
	}
	if (privateDynVB) {
		ringSlots[0] = ringSlots[1] = ringSlots[2] = privateDynVB.get();
	}

	auto* const savedCsContext = globals::d3d::context;
	*engine::g_immediateContext = deferred;
	globals::d3d::context = deferred;
	scopeActive = true;

	callOriginal();

	scopeActive = false;
	globals::d3d::context = savedCsContext;
	*engine::g_immediateContext = immediate;

	ringSlots[0] = savedRing[0];
	ringSlots[1] = savedRing[1];
	ringSlots[2] = savedRing[2];

	// Close the recording and replay it in place. RestoreContextState=TRUE so the
	// immediate context is left exactly as the engine expected before the shadow scope.
	winrt::com_ptr<ID3D11CommandList> commandList;
	if (SUCCEEDED(deferred->FinishCommandList(FALSE, commandList.put())) && commandList) {
		immediate->ExecuteCommandList(commandList.get(), TRUE);
		++framesDeferred;
	} else {
		logger::error("[ShadowDeferred] FinishCommandList failed; shadows this frame are lost");
	}

	// Restore the render-state mirror + caches to their pre-scope values.
	std::memcpy(reinterpret_cast<void*>(engine::S_base.address()), s_snapshot.data(), engine::kSnapshotBytes);
	*engine::g_currentTechnique = savedTechnique;
	*engine::g_currentShader = savedShader;
	*engine::g_currentMaterial = savedMaterial;
	*engine::g_boneCBRingCursor = savedBoneCursor;
	*engine::g_dynVBRingState = savedDynVB;
	*engine::g_shadowGeomToken = savedShadowToken;
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
