#include "OcclusionCulling.h"

#include "MOC.h"

#include "Globals.h"

#include <RE/B/BSCullingProcess.h>
#include <RE/L/LoadingMenu.h>
#include <RE/U/UI.h>
#include <RE/B/BSMultiBound.h>
#include <RE/B/BSParabolicCullingProcess.h>
#include <RE/N/NiAVObject.h>
#include <RE/N/NiCamera.h>
#include <RE/S/State.h>

#include <atomic>

#include <imgui.h>

namespace
{
	// The culling-process instance currently running a MAIN-camera cull (nullptr when
	// none). Per-object Process1 calls are occlusion-tested only when their process
	// matches. Instance-keyed rather than a bool because culls run CONCURRENTLY (the
	// main scene cull executes inside a BuildSceneLists job while water/shadow culls
	// run on other threads, each on a different process object) -- a shared flag would
	// leak main-pass testing into unrelated passes.
	std::atomic<RE::NiCullingProcess*> g_activeCullProcess{ nullptr };

	// BSCullingProcess::Process1 (NiCullingProcess vtable index 0x16): per-object
	// processing / recursion driver. If the object is provably occluded during the
	// main cull pass, skip the original call entirely so neither the object nor its
	// subtree is accumulated.
	// Mirror the ENGINE's own cull side effect when we skip an object: clear its
	// kAccumulated flag exactly like BSCullingProcess does on a frustum cull (gated on
	// recurseToGeometry + updateAccumulateFlag). Without this, downstream consumers can
	// read a STALE accumulated bit from a previous frame on an object we occluded --
	// i.e. MOC-culling must be a strict superset of vanilla culling's effects.
	inline void MarkCulledLikeEngine(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object)
	{
		auto* bsp = static_cast<RE::BSCullingProcess*>(a_self);
		if (bsp->recurseToGeometry && a_self->updateAccumulateFlag)
			a_object->GetFlags().reset(RE::NiAVObject::Flag::kAccumulated);
	}

	// CS_MOC_VALIDATE=1: measure the user's acceptance criterion directly -- of the
	// objects the ENGINE culls (frustum + occlusion planes), what fraction would MOC
	// also have culled? Detected via the kAccumulated flag across the original call
	// (only meaningful when the process recurses + updates the flag).
	bool                       g_validateMode = false;
	std::atomic<std::uint64_t> g_engineCulled{ 0 };
	std::atomic<std::uint64_t> g_engineCulledMocAgrees{ 0 };
	std::atomic<std::uint64_t> g_enginePassed{ 0 };

	template <class HookT>
	void Process1_Impl(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object, std::int32_t a_arg2)
	{
		// Gate on the CAMERA, not the single-instance bracket marker: the main
		// view runs MULTIPLE concurrent cull processes (scene-list jobs + the
		// per-pass subtree walks), and only whichever instance held the marker
		// got MOC-tested -- so whether a given walk culled an object depended on
		// thread scheduling, and the depth walk could draw what the main walk
		// culled. That per-walk disagreement flickered REGARDLESS of buffer
		// correctness (the mechanism survived every buffer-side fix). The camera
		// check makes testing uniform across every main-view walk while still
		// excluding shadow/reflection/cubemap/first-person culls.
		// Scene-list (accumulation) walks only: they run on JOB threads with the
		// RENDER camera and decide the pass lists (= the draws). The frozen cull
		// camera's per-pass subtree walks run on the RENDER THREAD interleaved
		// with utility/lighting submission -- testing there billed MOC CPU to
		// those overlay windows for little extra culling.
		const bool bracketed = MOC::IsSceneListCamera(a_self->camera);
		// MAIN view: optional small-VISIBLE cull (pure size math, no raster wait) first,
		// then the MOC occlusion test. In async mode the test is a lock-free cache lookup
		// (verdict computed off-thread by the builder) instead of an inline TestRect, so it
		// never stalls the scene-list barrier. TestSmallVisible is a no-op unless opted in.
		if (bracketed && a_object &&
			(!MOC::TestSmallVisible(a_object) ||
				(MOC::AsyncOcclusionTest ? !MOC::TestObjectCached(a_object) : !MOC::TestObject(a_object)))) {
			MarkCulledLikeEngine(a_self, a_object);
			return;  // small or occluded -> do not accumulate / recurse
		}

		// SUN shadow-gather pass: small-caster SHADOW cull. A rejection here removes the
		// caster from every cascade -- its mesh still renders in the main view, only its
		// shadow is dropped. Pure size math; no MOC buffer is involved for shadows.
		if (!bracketed && a_object && MOC::IsSunGatherCamera(a_self->camera) && !MOC::TestShadowCasterSmall(a_object)) {
			MarkCulledLikeEngine(a_self, a_object);
			return;
		}


		if (g_validateMode && bracketed && a_object) {
			auto* bsp = static_cast<RE::BSCullingProcess*>(a_self);
			if (bsp->recurseToGeometry && a_self->updateAccumulateFlag) {
				HookT::func(a_self, a_object, a_arg2);
				const bool accumulated = a_object->GetFlags().any(RE::NiAVObject::Flag::kAccumulated);
				if (!accumulated) {
					g_engineCulled.fetch_add(1, std::memory_order_relaxed);
					// Would MOC have culled it too? (Counts frustum-outs as agreement:
					// TestRect returns VIEW_CULLED outside the frustum.)
					if (!MOC::TestObject(a_object))
						g_engineCulledMocAgrees.fetch_add(1, std::memory_order_relaxed);
				} else {
					g_enginePassed.fetch_add(1, std::memory_order_relaxed);
				}
				return;
			}
		}
		HookT::func(a_self, a_object, a_arg2);
	}

	struct Process1_Hook
	{
		static void thunk(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object, std::int32_t a_arg2)
		{
			Process1_Impl<Process1_Hook>(a_self, a_object, a_arg2);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Same hook for BSParabolicCullingProcess, which OVERRIDES Process1/Process2 with its
	// own bodies. The main-scene subtree culls run on the global parabolic process --
	// hooking only BSCullingProcess never sees them. Separate hook structs keep each
	// body's original function pointer.
	struct PProcess1_Hook
	{
		static void thunk(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object, std::int32_t a_arg2)
		{
			Process1_Impl<PProcess1_Hook>(a_self, a_object, a_arg2);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Shared Process2 (top-level cull entry, vtable index 0x17) logic: decide whether the
	// nested Process1 calls of THIS cull should be occlusion-tested. BuildOccluders gates
	// on pointer identity with the engine's main world-render camera and returns true only
	// for main-scene culls with a freshly built (or reused same-frame) buffer. Synchronous
	// in V1: the buffer is ready before the nested Process1 calls.
	bool Process2_Begin(const RE::NiCamera* a_camera, RE::NiAVObject* a_scene)
	{
		auto* feature = OcclusionCulling::GetSingleton();

		bool active = false;
		if (feature->IsActive() && MOC::IsInitialized() && a_camera) {
			if (MOC::BuildOccluders(const_cast<RE::NiCamera*>(a_camera)))
				active = MOC::EnableOcclusionTesting;
		}
		if (g_validateMode && active) {
			if (auto* gs = RE::BSGraphics::State::GetSingleton(); gs && (gs->frameCount % 120u) == 0u) {
				logger::info("[MOC][validate] engineCulled={} mocAgrees={} ({:.1f}%) enginePassed={}",
					g_engineCulled.load(), g_engineCulledMocAgrees.load(),
					g_engineCulled.load() ? 100.0 * g_engineCulledMocAgrees.load() / g_engineCulled.load() : 0.0,
					g_enginePassed.load());
			}
		}
		(void)a_scene;
		return active;
	}

	// Mark a_self as the active main-cull process for the duration of the original call
	// (only when this cull is the main camera's). Non-main culls don't touch the marker,
	// so concurrent auxiliary passes can neither enable themselves nor disable a running
	// main cull. Restores the previous marker (not nullptr) because both Process2 bodies
	// are detoured and the parabolic override may chain into the base implementation --
	// a nested bracket must not strip the outer one.
	template <class HookT>
	void Process2_Bracketed(RE::NiCullingProcess* a_self, const RE::NiCamera* a_camera, RE::NiAVObject* a_scene, RE::NiVisibleArray* a_visibleSet)
	{
		const bool active = Process2_Begin(a_camera, a_scene);
		RE::NiCullingProcess* const prev = g_activeCullProcess.load(std::memory_order_relaxed);
		if (active)
			g_activeCullProcess.store(a_self, std::memory_order_relaxed);

		// MOC-EXCLUSIVE: neutralize the vanilla occlusion planes for this main cull by
		// zeroing the compound frustum's operator count (the engine's Process1 gate is
		// `compoundFrustum && freeOp && ...`). View-frustum culling is unaffected (the
		// plain m_kPlanes path still runs); MOC provides all occlusion. Restored after
		// the pass; auxiliary passes (their own process objects) never see this.
		auto*         cf = active && MOC::ExclusiveOcclusion ? static_cast<RE::BSCullingProcess*>(a_self)->compoundFrustum : nullptr;
		std::uint32_t savedOps = 0;
		if (cf) {
			savedOps = cf->freeOp;
			cf->freeOp = 0;
		}

		HookT::func(a_self, a_camera, a_scene, a_visibleSet);

		if (cf)
			cf->freeOp = savedOps;
		if (active)
			g_activeCullProcess.store(prev, std::memory_order_relaxed);
	}

	struct Process2_Hook
	{
		static void thunk(RE::NiCullingProcess* a_self, const RE::NiCamera* a_camera, RE::NiAVObject* a_scene, RE::NiVisibleArray* a_visibleSet)
		{
			Process2_Bracketed<Process2_Hook>(a_self, a_camera, a_scene, a_visibleSet);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PProcess2_Hook
	{
		static void thunk(RE::NiCullingProcess* a_self, const RE::NiCamera* a_camera, RE::NiAVObject* a_scene, RE::NiVisibleArray* a_visibleSet)
		{
			Process2_Bracketed<PProcess2_Hook>(a_self, a_camera, a_scene, a_visibleSet);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// TestBaseVisibility1(BSMultiBound&) -- the engine's CONTAINER visibility path (rooms,
	// cells, building shells). Multibound nodes never reach Process1, so this is where the
	// high-value tight-AABB occlusion tests belong: one occluded container prunes all its
	// contents. Engine verdict first; we only downgrade visible -> occluded.
	struct TestBaseVis1_Hook
	{
		static bool thunk(RE::BSCullingProcess* a_self, RE::BSMultiBound* a_bound)
		{
			const bool visible = func(a_self, a_bound);
			if (visible && a_bound &&
				MOC::IsSceneListCamera(static_cast<RE::NiCullingProcess*>(a_self)->camera) &&
				!MOC::TestMultiBound(a_bound))
				return false;
			return visible;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct PTestBaseVis1_Hook
	{
		static bool thunk(RE::BSCullingProcess* a_self, RE::BSMultiBound* a_bound)
		{
			const bool visible = func(a_self, a_bound);
			if (visible && a_bound &&
				MOC::IsSceneListCamera(static_cast<RE::NiCullingProcess*>(a_self)->camera) &&
				!MOC::TestMultiBound(a_bound))
				return false;
			return visible;
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Distant-tree LOD culling seam. BSMultiStreamInstanceTriShape carries a DUMMY
	// worldBound (radius 1.0) + kAlwaysDraw, so object-level tests never see it; its
	// real culling is per-instance-group inside OnVisible: the engine frustum-tests
	// each <=75-tree group's world AABB and writes a visible byte at group+0x50. This
	// post-hook ANDs a MOC occlusion verdict into groups the engine kept. Stateless
	// (the byte is rewritten by every walk), downgrade-only, and gated to the
	// bracketed main cull so shadow/reflection walks keep vanilla verdicts. Grass
	// shares this class -- distant-tree shapes are selected by their shader property
	// (BSDistantTreeShaderProperty), grass is deliberately left untouched. Unlike
	// Process1 this IS genuine virtual dispatch (called via vtable from the cull
	// walk), so a vtable patch sees every call.
	struct MSITS_OnVisible_Hook
	{
		static void thunk(RE::BSMultiStreamInstanceTriShape* a_this, RE::NiCullingProcess* a_process, std::int32_t a_alphaGroupIndex)
		{
			func(a_this, a_process, a_alphaGroupIndex);
			// Gate on the CAMERA, not the process-instance marker: the LODRoot subtree
			// (where LOD-tree shapes hang) is walked by concurrent scene-list job
			// processes, and the single-instance g_activeCullProcess marker only ever
			// matches one of them (measured: ~7 of ~340 visible groups/frame tested).
			// The dual-camera identity check is the same gate BuildOccluders uses, so
			// shadow/reflection/cubemap/first-person walks stay untouched.
			if (!a_process || !MOC::IsMainViewCamera(a_process->camera))
				return;
			if (!MOC::CullTreeLODGroups)
				return;
			auto* prop = a_this->GetGeometryRuntimeData().shaderProperty.get();
			if (!prop || !netimmerse_cast<RE::BSDistantTreeShaderProperty*>(prop))
				return;
			for (auto* group : a_this->GetMultiStreamTrishapeRuntimeData().unk160) {
				if (!group)
					continue;
				// InstanceGroup IS-A BSMultiBoundAABB (base at offset 0); the
				// engine's per-group visible byte lives at +0x50.
				auto* groupBytes = reinterpret_cast<std::uint8_t*>(group);
				if (!groupBytes[0x50])
					continue;
				if (!MOC::TestInstanceGroup(reinterpret_cast<RE::BSMultiBoundAABB*>(group)))
					groupBytes[0x50] = 0;
			}
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Main::DrawWorld_PreRender (REL::ID 35560, 1.5.97 0x1405B1860): the render
	// camera's frustum is FINAL at function entry (its first act is seeding the
	// cull processes with it), and the first cull jobs are queued only after
	// Main::Begin + water effects. Kicking the occluder raster here gives it
	// that whole window to complete, so the standard raster-before-test model
	// costs (almost) no test-side waiting. Redundant with the cull-time claim
	// (CAS per frame) -- whichever runs first wins.
	struct DrawWorldPreRender_Hook
	{
		static std::int64_t thunk(void* a_main, std::int64_t a_arg2, char a_isMainMenu)
		{
			MOC::KickBuild();
			return func(a_main, a_arg2, a_isMainMenu);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void OcclusionCulling::PostPostLoad()
{
	// Idempotent: this runs from XSEPlugin's direct call AND (once we set loaded=true)
	// from Feature::ForEachLoadedFeature, so guard against double vtable-hook install.
	static bool s_installed = false;
	if (s_installed)
		return;

	// CS_OCCLUSION=1 flips the master toggle ON at boot, =0 forces it OFF (for A/B perf runs);
	// the menu checkbox (settings.EnableOcclusionTesting) is the real gate otherwise.
	char buf[16] = {};
	if (GetEnvironmentVariableA("CS_OCCLUSION", buf, sizeof(buf)) && buf[0]) {
		envEnabled = (buf[0] == '1');
		settings.EnableOcclusionTesting = (buf[0] == '1');
		settings.EnableOccluderRendering = (buf[0] == '1');
	}
	// CS_MOC_MAX_OCCLUDERS=<n>: boot-time raster-budget override for automated A/B runs.
	if (GetEnvironmentVariableA("CS_MOC_MAX_OCCLUDERS", buf, sizeof(buf)) && buf[0]) {
		const int v = atoi(buf);
		if (v > 0)
			settings.MaxOccludersPerFrame = v;
	}
	// CS_MOC_SIMPLIFY_MODE=<0..3>: occluder simplification algorithm override (A/B).
	if (GetEnvironmentVariableA("CS_MOC_SIMPLIFY_MODE", buf, sizeof(buf)) && buf[0]) {
		const int v = atoi(buf);
		if (v >= 0 && v <= 3)
			settings.SimplifyMode = v;
	}
	// CS_MOC_RUNTIME_LODS=0/1: build+use distance LODs override (A/B).
	if (GetEnvironmentVariableA("CS_MOC_RUNTIME_LODS", buf, sizeof(buf)) && buf[0])
		settings.BuildRuntimeLODs = buf[0] == '1';
	// CS_MOC_FORCE_CULL=<pct>: DIAGNOSTIC -- force-cull a percentage of kept objects to
	// measure the fps-per-culled-object curve. Breaks the image; env-only, never persisted.
	if (GetEnvironmentVariableA("CS_MOC_FORCE_CULL", buf, sizeof(buf)) && buf[0]) {
		const int v = atoi(buf);
		if (v >= 0 && v <= 100)
			diagForceCullPercent = v;
	}
	// CS_MOC_EXCLUSIVE=1: MOC-exclusive occlusion (vanilla planes neutralized).
	if (GetEnvironmentVariableA("CS_MOC_EXCLUSIVE", buf, sizeof(buf)) && buf[0] == '1')
		settings.ExclusiveOcclusion = true;
	// CS_MOC_TREE_LOD=0/1: distant-tree LOD group culling override for A/B runs.
	if (GetEnvironmentVariableA("CS_MOC_TREE_LOD", buf, sizeof(buf)) && buf[0])
		settings.CullTreeLOD = buf[0] == '1';
	// CS_MOC_TREE_OCCLUDERS=0/1: opaque tree parts as occluders, override for A/B runs.
	if (GetEnvironmentVariableA("CS_MOC_TREE_OCCLUDERS", buf, sizeof(buf)) && buf[0])
		settings.TreeOccluders = buf[0] == '1';
	// CS_MOC_SMALL_VIS / CS_MOC_SMALL_SHADOW=0/1: small VISIBLE / SHADOW culls (A/B).
	if (GetEnvironmentVariableA("CS_MOC_SMALL_VIS", buf, sizeof(buf)) && buf[0])
		settings.CullSmallVisible = buf[0] == '1';
	if (GetEnvironmentVariableA("CS_MOC_SMALL_VIS_SLOPE", buf, sizeof(buf)) && buf[0])
		settings.SmallVisibleSlope = static_cast<float>(atof(buf));
	if (GetEnvironmentVariableA("CS_MOC_SMALL_SHADOW", buf, sizeof(buf)) && buf[0])
		settings.CullSmallShadows = buf[0] == '1';
	if (GetEnvironmentVariableA("CS_MOC_SMALL_SHADOW_SLOPE", buf, sizeof(buf)) && buf[0])
		settings.SmallShadowSlope = static_cast<float>(atof(buf));
	// CS_MOC_ALPHA_OCCLUDERS=0/1: alpha-TESTED geometry as solid occluders (A/B).
	if (GetEnvironmentVariableA("CS_MOC_ALPHA_OCCLUDERS", buf, sizeof(buf)) && buf[0])
		settings.AlphaTestedOccluders = buf[0] == '1';
	// CS_MOC_TERRAIN_LOD=0/1: distant terrain LOD as occluders (A/B).
	if (GetEnvironmentVariableA("CS_MOC_TERRAIN_LOD", buf, sizeof(buf)) && buf[0])
		settings.TerrainLODOccluders = buf[0] == '1';
	// CS_MOC_ASYNC=0/1: async (off-barrier) occlusion testing (A/B).
	if (GetEnvironmentVariableA("CS_MOC_ASYNC", buf, sizeof(buf)) && buf[0])
		settings.AsyncOcclusionTest = buf[0] == '1';
	// CS_MOC_VALIDATE=1: engine-cull agreement instrumentation (see Process1_Impl).
	if (GetEnvironmentVariableA("CS_MOC_VALIDATE", buf, sizeof(buf)) && buf[0] == '1')
		g_validateMode = true;
	// CS_MOC_MIN_TEST_RADIUS=<n>: per-object test gate override for A/B runs.
	if (GetEnvironmentVariableA("CS_MOC_MIN_TEST_RADIUS", buf, sizeof(buf)) && buf[0]) {
		const int v = atoi(buf);
		if (v >= 0)
			settings.OccluderTestMinRadius = static_cast<float>(v);
	}

	// SE 1.5.97 only: the address-library id and struct offsets used by the port are SE.
	if (!REL::Module::IsSE()) {
		logger::info("[OcclusionCulling] SE-only for now; not installing on this runtime");
		return;
	}

	// Sync BEFORE Init so boot-time settings (env overrides, defaults) reach the
	// pool creation and the occluder gather.
	SyncSettingsToMOC();
	MOC::Init();

	// CS_MOC_NO_HOOKS=1: TRUE-VANILLA baseline for benchmarks -- skip every
	// detour, vtable patch and the MOC builder entirely; the game runs with
	// zero occlusion-culling instrumentation (not even trampoline overhead).
	{
		char nhBuf[8] = {};
		if (GetEnvironmentVariableA("CS_MOC_NO_HOOKS", nhBuf, sizeof(nhBuf)) && nhBuf[0] == '1') {
			logger::warn("[OcclusionCulling] CS_MOC_NO_HOOKS=1: no hooks installed (pure vanilla baseline)");
			return;
		}
	}

	// Detour the Process1/Process2 FUNCTION BODIES of both culling-process classes
	// (base BSCullingProcess and the BSParabolicCullingProcess overrides the main world
	// cull runs on). Body detours are essential: the engine's main cull walk calls
	// Process1 DIRECTLY (devirtualized), so vtable patches never see it -- with vtable
	// hooks the buffer built correctly but tested stayed 0. This mirrors Nukem, who
	// detoured the Process function body (1.5.23 0xD50310). Virtual dispatch lands in
	// the same bodies, so these four detours cover every call path. Installed
	// unconditionally; runtime behavior is gated by IsActive() inside the thunks.
	const auto p1b = REL::ID(74804).address();
	const auto p2b = REL::ID(74805).address();
	const auto p1p = REL::ID(101597).address();
	const auto p2p = REL::ID(101598).address();
	stl::detour_thunk<Process1_Hook>(REL::RelocationID(74804, 74804));     // BSCullingProcess::Process1
	stl::detour_thunk<Process2_Hook>(REL::RelocationID(74805, 74805));     // BSCullingProcess::Process2
	stl::detour_thunk<PProcess1_Hook>(REL::RelocationID(101597, 101597));  // BSParabolicCullingProcess::Process1
	stl::detour_thunk<PProcess2_Hook>(REL::RelocationID(101598, 101598));  // BSParabolicCullingProcess::Process2
	stl::detour_thunk<TestBaseVis1_Hook>(REL::RelocationID(74816, 74816));      // BSCullingProcess::TestBaseVisibility1
	stl::detour_thunk<PTestBaseVis1_Hook>(REL::RelocationID(101605, 101605));   // BSParabolicCullingProcess::TestBaseVisibility1
	stl::write_vfunc<0x34, MSITS_OnVisible_Hook>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);  // distant-tree LOD group culling
	// Kick at DrawWorld_PreRender entry, NOT earlier: a Main::Swap-entry kick was
	// measured WORSE (raster 1.5->2.5ms, waits +25%) -- during render setup the
	// CPU is busy (renderer begin, UI pre-display), while during BuildSceneLists
	// the cull threads that wait on the raster YIELD their cores to its workers.
	stl::detour_thunk<DrawWorldPreRender_Hook>(REL::RelocationID(35560, 35560));  // frame-start raster kick
	// On success DetourAttach rewrites T::func to the trampoline (!= original address);
	// equal means the attach silently failed.
	logger::info("[OcclusionCulling] detours attached: P1base={} P2base={} P1para={} P2para={}",
		Process1_Hook::func.address() != p1b, Process2_Hook::func.address() != p2b,
		PProcess1_Hook::func.address() != p1p, PProcess2_Hook::func.address() != p2p);

	// Quiesce the builder on loading-screen open (scene teardown race).
	if (auto* ui = RE::UI::GetSingleton()) {
		static MenuEventSink s_menuSink;
		ui->AddEventSink<RE::MenuOpenCloseEvent>(&s_menuSink);
	}

	s_installed = true;
	// Mark loaded (+ a nominal version) so the feature appears as a normal entry in the
	// CS menu; it has no shader .ini so Feature::Load leaves it unloaded otherwise. The
	// disk-cache overrides above keep this safe.
	version = "1-0-0";
	loaded = true;

	logger::info("[OcclusionCulling] hooks installed (master={})", settings.EnableOcclusionTesting ? "on" : "off");
}

bool OcclusionCulling::IsActive() const
{
	// Menu-driven master gate (also flipped on at boot by CS_OCCLUSION=1).
	return settings.EnableOcclusionTesting;
}

void OcclusionCulling::SyncSettingsToMOC()
{
	MOC::EnableOcclusionTesting = settings.EnableOcclusionTesting;
	MOC::EnableOccluderRendering = settings.EnableOccluderRendering;
	MOC::OccluderMaxDistance = settings.OccluderMaxDistance;
	MOC::OccluderFirstLevelMinSize = settings.OccluderFirstLevelMinSize;
	// Clamp the EFFECTIVE budget: persisted settings from older sessions carry
	// 4096 and silently override the tuned default (measured: 1463 occluders ->
	// raster 6.5ms vs 384 -> 1.9ms). The slider can still lower it.
	MOC::MaxOccludersPerFrame = static_cast<std::uint32_t>(std::clamp(settings.MaxOccludersPerFrame, 1, 512));
	// A simplification change must re-run on already-cached meshes, so invalidate the
	// mesh cache when either simplify knob differs from what MOC currently holds. Guarded
	// on IsInitialized so the boot-time sync (cache empty) never triggers a rebuild.
	const bool simplifyChanged = MOC::IsInitialized() &&
	                             (MOC::SimplifyMode != settings.SimplifyMode ||
									 MOC::SimplifyStrength != settings.SimplifyStrength ||
									 MOC::SimplifyError != settings.SimplifyError ||
									 MOC::SimplifyOptions != settings.SimplifyOptions ||
									 MOC::BuildRuntimeLODs != settings.BuildRuntimeLODs);
	MOC::SimplifyMode = std::clamp(settings.SimplifyMode, 0, 3);
	MOC::SimplifyStrength = std::clamp(settings.SimplifyStrength, 0.02f, 1.0f);
	MOC::SimplifyError = std::max(settings.SimplifyError, 0.0f);
	MOC::SimplifyOptions = settings.SimplifyOptions;
	MOC::BuildRuntimeLODs = settings.BuildRuntimeLODs;
	if (simplifyChanged)
		MOC::RebuildOccluderMeshCache();
	MOC::OccluderTestMinRadius = settings.OccluderTestMinRadius;
	MOC::ExclusiveOcclusion = settings.ExclusiveOcclusion;
	MOC::OccluderMinLeafSize = settings.OccluderMinLeafSize;
	MOC::CullTreeLODGroups = settings.CullTreeLOD;
	MOC::TreeOccluders = settings.TreeOccluders;
	MOC::AlphaTestedOccluders = settings.AlphaTestedOccluders;
	MOC::TerrainLODOccluders = settings.TerrainLODOccluders;
	MOC::AsyncOcclusionTest = settings.AsyncOcclusionTest;
	MOC::CullSmallVisible = settings.CullSmallVisible;
	MOC::SmallVisibleMinSize = settings.SmallVisibleMinSize;
	MOC::SmallVisibleSlope = settings.SmallVisibleSlope;
	MOC::CullSmallShadows = settings.CullSmallShadows;
	MOC::SmallShadowMinSize = settings.SmallShadowMinSize;
	MOC::SmallShadowSlope = settings.SmallShadowSlope;
	MOC::DiagForceCullPercent = diagForceCullPercent;  // env-only diagnostic, not persisted
}

RE::BSEventNotifyControl OcclusionCulling::MenuEventSink::ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*)
{
	// A loading screen opening means a scene teardown is imminent; a builder gather in
	// flight would race freed nodes (crashed on worldspace transitions twice). Drain it
	// BEFORE the teardown starts. The claim-path guard then refuses new kicks until the
	// load finishes.
	if (a_event && a_event->opening && a_event->menuName == RE::LoadingMenu::MENU_NAME)
		MOC::QuiesceBuilder();
	return RE::BSEventNotifyControl::kContinue;
}

void OcclusionCulling::Prepass()
{
	MOC::DumpDebugImages();  // env-gated (CS_MOC_DUMP=1) + rate-limited; no-op otherwise
}

void OcclusionCulling::DrawSettings()
{
	if (!REL::Module::IsSE()) {
		ImGui::TextWrapped("%s", T("feature.occlusion_culling.se_only", "Occlusion Culling currently supports Skyrim SE 1.5.97 only."));
		return;
	}

	ImGui::TextWrapped("%s", T("feature.occlusion_culling.desc",
		"Software (CPU) occlusion culling: skips drawing scene objects fully hidden behind large static meshes. Experimental."));
	ImGui::Separator();

	bool changed = false;

	// Master on/off — this is the "toggle culling entirely" switch.
	changed |= ImGui::Checkbox(T("feature.occlusion_culling.enable_testing", "Enable Occlusion Culling"), &settings.EnableOcclusionTesting);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("feature.occlusion_culling.enable_testing_tooltip",
			"Master toggle. When on, large static occluders are rasterized each frame and objects hidden behind them are skipped."));

	ImGui::BeginDisabled(!settings.EnableOcclusionTesting);

	changed |= ImGui::Checkbox(T("feature.occlusion_culling.enable_rendering", "Render Occluders"), &settings.EnableOccluderRendering);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("feature.occlusion_culling.enable_rendering_tooltip",
			"Rasterize large static meshes into the CPU occlusion buffer each frame. Turn OFF to A/B the cost: the buffer stays empty so nothing is culled, but the traversal still runs."));

	changed |= ImGui::SliderFloat(T("feature.occlusion_culling.max_distance", "Occluder Max Distance"), &settings.OccluderMaxDistance, 1000.0f, 100000.0f, "%.0f");
	changed |= ImGui::SliderFloat(T("feature.occlusion_culling.first_level_min_size", "Occluder Min Size"), &settings.OccluderFirstLevelMinSize, 0.0f, 2000.0f, "%.0f");

	changed |= ImGui::SliderFloat(T("feature.occlusion_culling.min_leaf_size", "Min Occluder Mesh Size"), &settings.OccluderMinLeafSize, 0.0f, 500.0f, "%.0f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("feature.occlusion_culling.min_leaf_size_tooltip",
			"Meshes smaller than this are not rasterized into the occlusion buffer. Everything larger is rendered (no per-frame cap)."));

	changed |= ImGui::SliderInt(T("feature.occlusion_culling.max_occluders", "Max Occluders / Frame"), &settings.MaxOccludersPerFrame, 16, 4096, "%d", ImGuiSliderFlags_Logarithmic);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("feature.occlusion_culling.max_occluders_tooltip",
			"CPU raster budget per frame, selected by estimated screen coverage (large occluders like terrain always make the cut). Typical scenes offer 700-1600 candidates, so high values mean 'rasterize everything'."));

	changed |= ImGui::SliderFloat(T("feature.occlusion_culling.min_test_radius", "Min Tested Object Size"), &settings.OccluderTestMinRadius, 0.0f, 200.0f, "%.0f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("feature.occlusion_culling.min_test_radius_tooltip",
			"Objects smaller than this (world-bound radius) are never occlusion-tested. Lower = more draw calls saved but more CPU per frame."));

	changed |= ImGui::Checkbox(T("feature.occlusion_culling.exclusive", "Exclusive Occlusion (replace vanilla planes)"), &settings.ExclusiveOcclusion);
	changed |= ImGui::Checkbox(T("feature.occlusion_culling.tree_lod", "Cull Distant Tree LOD"), &settings.CullTreeLOD);
	changed |= ImGui::Checkbox(T("feature.occlusion_culling.tree_occluders", "Opaque Tree Parts as Occluders"), &settings.TreeOccluders);
	changed |= ImGui::Checkbox(T("feature.occlusion_culling.alpha_occluders", "Alpha-Tested Objects as Occluders"), &settings.AlphaTestedOccluders);
	changed |= ImGui::Checkbox(T("feature.occlusion_culling.terrain_lod_occluders", "Distant Terrain LOD as Occluders"), &settings.TerrainLODOccluders);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("feature.occlusion_culling.terrain_lod_occluders_tooltip",
			"Rasterize the game's coarse distant terrain LOD as occluders. Off by default: unlike the heightmap-built near terrain (which stays below the real surface), the LOD mesh can rise above it and wrongly occlude visible geometry. The loaded-grid heightmap terrain is always used either way."));
	changed |= ImGui::Checkbox(T("feature.occlusion_culling.async_test", "Async Occlusion Testing (experimental)"), &settings.AsyncOcclusionTest);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("feature.occlusion_culling.async_test_tooltip",
			"Move the per-object occlusion test off the scene-list cull walk: the builder pre-tests last frame's candidates against this frame's buffer and Process1 just reads the result. Keeps the test cost out of the Utility barrier (no spike even with heavy occluders). Slightly less culling on newly-appeared objects for one frame. Experimental."));

	ImGui::Spacing();
	ImGui::TextDisabled("%s", T("feature.occlusion_culling.small_header", "Small-Object Culling"));
	changed |= ImGui::Checkbox(T("feature.occlusion_culling.small_shadows", "Cull Small Object Shadows"), &settings.CullSmallShadows);
	if (auto* t = T("feature.occlusion_culling.small_shadows_tooltip", "Drops distant small objects from the SHADOW MAPS only -- their mesh still renders, just its shadow disappears. Cuts shadow-map draw calls (the bulk of the Utility shader time) with no visible change. Threshold grows with distance; actors are never culled. Safe to raise."); ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", t);
	if (settings.CullSmallShadows)
		changed |= ImGui::SliderFloat(T("feature.occlusion_culling.small_shadow_slope", "Shadow Cull Distance Growth"), &settings.SmallShadowSlope, 0.0f, 0.08f, "%.3f");

	changed |= ImGui::Checkbox(T("feature.occlusion_culling.small_visible", "Cull Small Visible Objects"), &settings.CullSmallVisible);
	if (auto* t = T("feature.occlusion_culling.small_visible_tooltip", "Drops distant small objects from the MAIN VIEW entirely (their mesh stops rendering). Saves the most but removes on-screen geometry, so it pops in motion -- tune conservatively and check while moving. Actors are never culled."); ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", t);
	if (settings.CullSmallVisible)
		changed |= ImGui::SliderFloat(T("feature.occlusion_culling.small_visible_slope", "Visible Cull Distance Growth"), &settings.SmallVisibleSlope, 0.0f, 0.08f, "%.3f");

	ImGui::Spacing();
	ImGui::TextDisabled("%s", T("feature.occlusion_culling.simplify_header", "Occluder Mesh Simplification"));
	// Every meshoptimizer simplifier, selectable live -- each change rebuilds the loaded
	// occluder geometry so the effect (raster cost, cull accuracy, buffer view) is visible
	// immediately.
	const char* const kSimplifyModes[] = {
		T("feature.occlusion_culling.simplify_off", "Off (full detail)"),
		T("feature.occlusion_culling.simplify_quality", "Quality (meshopt_simplify)"),
		T("feature.occlusion_culling.simplify_sloppy", "Sloppy (meshopt_simplifySloppy)"),
		T("feature.occlusion_culling.simplify_prune", "Prune (meshopt_simplifyPrune)")
	};
	changed |= ImGui::Combo(T("feature.occlusion_culling.simplify_mode", "Simplification Algorithm"), &settings.SimplifyMode, kSimplifyModes, IM_ARRAYSIZE(kSimplifyModes));
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("feature.occlusion_culling.simplify_mode_tooltip",
			"Which meshoptimizer algorithm simplifies the occluder meshes. Quality preserves topology; Sloppy is faster and coarser (the silhouette may grow); Prune only drops small disconnected pieces. Changing any of these rebuilds the loaded occluders immediately."));
	if (settings.SimplifyMode != 0) {
		if (settings.SimplifyMode != 3)  // Prune has no ratio target
			changed |= ImGui::SliderFloat(T("feature.occlusion_culling.simplify_strength", "Target Detail"), &settings.SimplifyStrength, 0.02f, 1.0f, "%.2f");
		changed |= ImGui::SliderFloat(T("feature.occlusion_culling.simplify_error", "Target Error"), &settings.SimplifyError, 0.0001f, 1.0f, "%.4f", ImGuiSliderFlags_Logarithmic);
		if (settings.SimplifyMode == 1) {  // option flags apply to Quality only
			changed |= ImGui::CheckboxFlags(T("feature.occlusion_culling.simplify_lockborder", "Lock Border"), &settings.SimplifyOptions, 1u << 0);
			changed |= ImGui::CheckboxFlags(T("feature.occlusion_culling.simplify_sparse", "Sparse"), &settings.SimplifyOptions, 1u << 1);
			changed |= ImGui::CheckboxFlags(T("feature.occlusion_culling.simplify_errorabs", "Absolute Error"), &settings.SimplifyOptions, 1u << 2);
			changed |= ImGui::CheckboxFlags(T("feature.occlusion_culling.simplify_pruneopt", "Prune Components"), &settings.SimplifyOptions, 1u << 3);
		}
	}
	changed |= ImGui::Checkbox(T("feature.occlusion_culling.runtime_lods", "Occluder Distance LODs"), &settings.BuildRuntimeLODs);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("feature.occlusion_culling.runtime_lods_tooltip",
			"Use progressively coarser occluder meshes at distance (two extra sloppy-simplified LODs picked per frame). Faster raster, but sloppy LODs are not conservative -- their silhouette can grow and over-occlude. Turn OFF to rasterize every occluder at its base mesh (fully conservative) if distant geometry looks wrongly culled. Rebuilds occluders immediately."));

	ImGui::EndDisabled();

	if (changed)
		SyncSettingsToMOC();

	// Debug: the software occlusion depth buffer (occluder silhouettes, near = bright).
	if (ImGui::CollapsingHeader(T("feature.occlusion_culling.debug_buffer", "Occlusion Depth Buffer"))) {
		MOC::UpdateDebugView();
		if (void* srv = MOC::GetDebugSRV()) {
			const float w = ImGui::GetContentRegionAvail().x;
			ImGui::Image(srv, ImVec2(w, w * (9.0f / 16.0f)));  // buffer is 16:9 (512x288)
			ImGui::TextWrapped("%s", T("feature.occlusion_culling.debug_buffer_hint",
				"Grayscale = rasterized static occluders (brighter = closer). Empty until CS_OCCLUSION=1 and occluders are rendered."));
		} else {
			ImGui::TextWrapped("%s", T("feature.occlusion_culling.debug_buffer_unavailable", "Depth buffer texture not available."));
		}
	}
}

void OcclusionCulling::LoadSettings(json& o_json)
{
	if (o_json["EnableOcclusionTesting"].is_boolean())
		settings.EnableOcclusionTesting = o_json["EnableOcclusionTesting"];
	if (o_json["EnableOccluderRendering"].is_boolean())
		settings.EnableOccluderRendering = o_json["EnableOccluderRendering"];
	if (o_json["OccluderMaxDistance"].is_number())
		settings.OccluderMaxDistance = o_json["OccluderMaxDistance"];
	if (o_json["OccluderFirstLevelMinSize"].is_number())
		settings.OccluderFirstLevelMinSize = o_json["OccluderFirstLevelMinSize"];
	if (o_json["MaxOccludersPerFrame"].is_number_integer())
		settings.MaxOccludersPerFrame = o_json["MaxOccludersPerFrame"];
	if (o_json["SimplifyMode"].is_number_integer())
		settings.SimplifyMode = o_json["SimplifyMode"];
	else if (o_json["SimplifyOccluders"].is_boolean())  // migrate the old bool
		settings.SimplifyMode = o_json["SimplifyOccluders"] ? 1 : 0;
	if (o_json["SimplifyStrength"].is_number())
		settings.SimplifyStrength = o_json["SimplifyStrength"];
	if (o_json["SimplifyError"].is_number())
		settings.SimplifyError = o_json["SimplifyError"];
	if (o_json["SimplifyOptions"].is_number_unsigned())
		settings.SimplifyOptions = o_json["SimplifyOptions"];
	if (o_json["BuildRuntimeLODs"].is_boolean())
		settings.BuildRuntimeLODs = o_json["BuildRuntimeLODs"];
	if (o_json["OccluderTestMinRadius"].is_number())
		settings.OccluderTestMinRadius = o_json["OccluderTestMinRadius"];
	if (o_json["ExclusiveOcclusion"].is_boolean())
		settings.ExclusiveOcclusion = o_json["ExclusiveOcclusion"];
	if (o_json["OccluderMinLeafSize"].is_number())
		settings.OccluderMinLeafSize = o_json["OccluderMinLeafSize"];
	if (o_json["CullTreeLOD"].is_boolean())
		settings.CullTreeLOD = o_json["CullTreeLOD"];
	if (o_json["TreeOccluders"].is_boolean())
		settings.TreeOccluders = o_json["TreeOccluders"];
	if (o_json["AlphaTestedOccluders"].is_boolean())
		settings.AlphaTestedOccluders = o_json["AlphaTestedOccluders"];
	if (o_json["TerrainLODOccluders"].is_boolean())
		settings.TerrainLODOccluders = o_json["TerrainLODOccluders"];
	if (o_json["AsyncOcclusionTest"].is_boolean())
		settings.AsyncOcclusionTest = o_json["AsyncOcclusionTest"];
	if (o_json["CullSmallVisible"].is_boolean())
		settings.CullSmallVisible = o_json["CullSmallVisible"];
	if (o_json["SmallVisibleMinSize"].is_number())
		settings.SmallVisibleMinSize = o_json["SmallVisibleMinSize"];
	if (o_json["SmallVisibleSlope"].is_number())
		settings.SmallVisibleSlope = o_json["SmallVisibleSlope"];
	if (o_json["CullSmallShadows"].is_boolean())
		settings.CullSmallShadows = o_json["CullSmallShadows"];
	if (o_json["SmallShadowMinSize"].is_number())
		settings.SmallShadowMinSize = o_json["SmallShadowMinSize"];
	if (o_json["SmallShadowSlope"].is_number())
		settings.SmallShadowSlope = o_json["SmallShadowSlope"];

	SyncSettingsToMOC();
}

void OcclusionCulling::SaveSettings(json& o_json)
{
	o_json["EnableOcclusionTesting"] = settings.EnableOcclusionTesting;
	o_json["EnableOccluderRendering"] = settings.EnableOccluderRendering;
	o_json["OccluderMaxDistance"] = settings.OccluderMaxDistance;
	o_json["OccluderFirstLevelMinSize"] = settings.OccluderFirstLevelMinSize;
	o_json["MaxOccludersPerFrame"] = settings.MaxOccludersPerFrame;
	o_json["SimplifyMode"] = settings.SimplifyMode;
	o_json["SimplifyStrength"] = settings.SimplifyStrength;
	o_json["SimplifyError"] = settings.SimplifyError;
	o_json["SimplifyOptions"] = settings.SimplifyOptions;
	o_json["BuildRuntimeLODs"] = settings.BuildRuntimeLODs;
	o_json["OccluderTestMinRadius"] = settings.OccluderTestMinRadius;
	o_json["ExclusiveOcclusion"] = settings.ExclusiveOcclusion;
	o_json["OccluderMinLeafSize"] = settings.OccluderMinLeafSize;
	o_json["CullTreeLOD"] = settings.CullTreeLOD;
	o_json["TreeOccluders"] = settings.TreeOccluders;
	o_json["AlphaTestedOccluders"] = settings.AlphaTestedOccluders;
	o_json["TerrainLODOccluders"] = settings.TerrainLODOccluders;
	o_json["AsyncOcclusionTest"] = settings.AsyncOcclusionTest;
	o_json["CullSmallVisible"] = settings.CullSmallVisible;
	o_json["SmallVisibleMinSize"] = settings.SmallVisibleMinSize;
	o_json["SmallVisibleSlope"] = settings.SmallVisibleSlope;
	o_json["CullSmallShadows"] = settings.CullSmallShadows;
	o_json["SmallShadowMinSize"] = settings.SmallShadowMinSize;
	o_json["SmallShadowSlope"] = settings.SmallShadowSlope;
}

void OcclusionCulling::RestoreDefaultSettings()
{
	settings = {};
	SyncSettingsToMOC();
}
