#include "OcclusionCulling.h"

#include "MOC.h"

#include "EngineFixes/GrassCull.h"
#include "Globals.h"

#include <RE/B/BSCullingProcess.h>
#include <RE/B/BSMultiBound.h>
#include <RE/B/BSParabolicCullingProcess.h>
#include <RE/N/NiAVObject.h>
#include <RE/N/NiCamera.h>

#include <imgui.h>

namespace
{
	// BSCullingProcess::Process1 (NiCullingProcess vtable index 0x16): per-object
	// processing / recursion driver. If the object is provably occluded during the
	// main cull pass, skip the original call entirely so neither the object nor its
	// subtree is accumulated.
	// Mirror the ENGINE's own cull side effect when we skip an object: clear its
	// kAccumulated flag exactly like BSCullingProcess does on a frustum cull (gated on
	// recurseToGeometry + updateAccumulateFlag). Without this, downstream consumers can
	// read a STALE accumulated bit from a previous frame on an object we occluded.
	inline void MarkCulledLikeEngine(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object)
	{
		auto* bsp = static_cast<RE::BSCullingProcess*>(a_self);
		if (bsp->recurseToGeometry && a_self->updateAccumulateFlag)
			a_object->GetFlags().reset(RE::NiAVObject::Flag::kAccumulated);
	}

	template <class HookT>
	void Process1_Impl(RE::NiCullingProcess* a_self, RE::NiAVObject* a_object, std::int32_t a_arg2)
	{
		// Gate on the CAMERA: the main view runs MULTIPLE concurrent cull processes (scene-list
		// jobs + per-pass subtree walks). Testing every SCENE-LIST (accumulation) walk keeps
		// culling uniform across the main view while excluding shadow/reflection/cubemap/first-
		// person culls.
		const bool bracketed = MOC::IsSceneListCamera(a_self->camera);
		// MAIN view: the Hi-Z occlusion test. When the Hi-Z front snapshot isn't published yet
		// the test conservatively keeps the object.
		if (bracketed && a_object && !MOC::TestObject(a_object)) {
			MarkCulledLikeEngine(a_self, a_object);
			return;  // occluded -> do not accumulate / recurse
		}

		// SUN shadow-gather pass: small-caster SHADOW cull. A rejection here removes the
		// caster from every cascade -- its mesh still renders in the main view, only its
		// shadow is dropped. Pure size math; no depth buffer is involved for shadows.
		if (!bracketed && a_object && MOC::IsSunGatherCamera(a_self->camera) && !MOC::TestShadowCasterSmall(a_object)) {
			MarkCulledLikeEngine(a_self, a_object);
			return;
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
	void Process2_Begin(const RE::NiCamera* a_camera)
	{
		auto* feature = OcclusionCulling::GetSingleton();
		if (feature->IsActive() && MOC::IsInitialized() && a_camera)
			MOC::BuildOccluders(const_cast<RE::NiCamera*>(a_camera));
	}

	// Run the once-per-frame Hi-Z matrix load (Process2_Begin) around the original cull. Occlusion
	// testing is gated per-object on the scene-list camera identity inside Process1, so this only
	// needs to drive the buffer build.
	template <class HookT>
	void Process2_Bracketed(RE::NiCullingProcess* a_self, const RE::NiCamera* a_camera, RE::NiAVObject* a_scene, RE::NiVisibleArray* a_visibleSet)
	{
		Process2_Begin(a_camera);
		HookT::func(a_self, a_camera, a_scene, a_visibleSet);
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

}

void OcclusionCulling::PostPostLoad()
{
	// Idempotent: this runs from XSEPlugin's direct call AND (once we set loaded=true)
	// from Feature::ForEachLoadedFeature, so guard against double vtable-hook install.
	static bool s_installed = false;
	if (s_installed)
		return;

	// SE 1.5.97 only: the address-library id and struct offsets used by the port are SE.
	if (!REL::Module::IsSE()) {
		logger::info("[OcclusionCulling] SE-only for now; not installing on this runtime");
		return;
	}

	// Sync BEFORE Init so the default settings reach the Hi-Z runtime.
	SyncSettingsToMOC();
	MOC::Init();

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
	// On success DetourAttach rewrites T::func to the trampoline (!= original address);
	// equal means the attach silently failed.
	logger::info("[OcclusionCulling] detours attached: P1base={} P2base={} P1para={} P2para={}",
		Process1_Hook::func.address() != p1b, Process2_Hook::func.address() != p2b,
		PProcess1_Hook::func.address() != p1p, PProcess2_Hook::func.address() != p2p);

	s_installed = true;
	// Mark loaded (+ a nominal version) so the feature appears as a normal entry in the
	// CS menu; it has no shader .ini so Feature::Load leaves it unloaded otherwise. The
	// disk-cache overrides keep this safe.
	version = "1-0-0";
	loaded = true;

	logger::info("[OcclusionCulling] hooks installed (master={})", settings.EnableOcclusionTesting ? "on" : "off");
}

bool OcclusionCulling::IsActive() const
{
	// Menu-driven master gate.
	return settings.EnableOcclusionTesting;
}

void OcclusionCulling::SyncSettingsToMOC()
{
	MOC::EnableOcclusionTesting = settings.EnableOcclusionTesting;
	MOC::OccluderTestMinRadius = settings.OccluderTestMinRadius;
	MOC::CullSmallShadows = settings.CullSmallShadows;
	MOC::SmallShadowMinSize = settings.SmallShadowMinSize;
	MOC::SmallShadowSlope = settings.SmallShadowSlope;

	// The GPU grass cull is a separate EngineFix with no menu of its own -- push its settings from here too.
	GrassCull::SetSettings({
		.cull = settings.GrassCulling,
		.occlusion = settings.GrassOcclusion,
		.thinAmount = settings.GrassThinning,
		.thinCurve = settings.GrassThinningCurve,
		.thinScale = settings.GrassThinningScale,
	});
}

void OcclusionCulling::DrawSettings()
{
	if (!REL::Module::IsSE()) {
		ImGui::TextWrapped("%s", T("feature.occlusion_culling.se_only", "Occlusion Culling currently supports Skyrim SE 1.5.97 only."));
		return;
	}

	ImGui::TextWrapped("%s", T("feature.occlusion_culling.desc",
		"GPU Hi-Z occlusion culling: skips drawing scene objects fully hidden behind nearer geometry, using the previous frame's reduced depth."));
	ImGui::Separator();

	bool changed = false;

	// Master on/off — this is the "toggle culling entirely" switch.
	changed |= ImGui::Checkbox(T("feature.occlusion_culling.enable_testing", "Enable Occlusion Culling"), &settings.EnableOcclusionTesting);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("feature.occlusion_culling.enable_testing_tooltip",
			"Master toggle. When on, scene objects hidden behind nearer geometry are skipped using the GPU Hi-Z depth."));

	ImGui::BeginDisabled(!settings.EnableOcclusionTesting);

	changed |= ImGui::SliderFloat(T("feature.occlusion_culling.min_test_radius", "Min Tested Object Size"), &settings.OccluderTestMinRadius, 0.0f, 200.0f, "%.0f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("feature.occlusion_culling.min_test_radius_tooltip",
			"Objects smaller than this (world-bound radius) are never occlusion-tested. Lower = more draw calls saved but more CPU per frame."));

	ImGui::Spacing();
	ImGui::TextDisabled("%s", T("feature.occlusion_culling.small_header", "Small-Object Culling"));
	changed |= ImGui::Checkbox(T("feature.occlusion_culling.small_shadows", "Cull Small Object Shadows"), &settings.CullSmallShadows);
	if (auto* t = T("feature.occlusion_culling.small_shadows_tooltip", "Drops distant small objects from the SHADOW MAPS only -- their mesh still renders, just its shadow disappears. Cuts shadow-map draw calls (the bulk of the Utility shader time) with no visible change. Threshold grows with distance; actors are never culled. Safe to raise."); ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", t);
	if (settings.CullSmallShadows)
		changed |= ImGui::SliderFloat(T("feature.occlusion_culling.small_shadow_slope", "Shadow Cull Distance Growth"), &settings.SmallShadowSlope, 0.0f, 0.08f, "%.3f");

	ImGui::EndDisabled();

	// --- GPU grass culling (a separate EngineFix that shares this menu; independent of the object master above) ---
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::TextDisabled("%s", T("feature.occlusion_culling.grass_header", "Grass Culling"));
	ImGui::Spacing();

	changed |= ImGui::Checkbox(T("feature.occlusion_culling.grass_enable", "Enable Grass Culling"), &settings.GrassCulling);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("feature.occlusion_culling.grass_enable_tooltip",
			"GPU-culls grass clumps that are off-screen or past the fade-out distance before they are drawn. Cheap and artifact-free."));

	ImGui::BeginDisabled(!settings.GrassCulling);

	changed |= ImGui::Checkbox(T("feature.occlusion_culling.grass_occlusion", "Grass Occlusion Culling"), &settings.GrassOcclusion);
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("feature.occlusion_culling.grass_occlusion_tooltip",
			"Also drops grass fully hidden behind terrain, rocks, and buildings (GPU Hi-Z). This is the main frame-rate win in occluded views."));

	changed |= ImGui::SliderFloat(T("feature.occlusion_culling.grass_thinning", "Grass Thinning"), &settings.GrassThinning, 0.0f, 1.0f, "%.2f");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("%s", T("feature.occlusion_culling.grass_thinning_tooltip",
			"Culls grass across the vanilla fade-out range (fade-start to fade-end) instead of just fading it invisible like vanilla, matching that density falloff. This amount is the fraction culled at the fade-out end; 1.0 fully matches the fade. 0 = off."));

	if (settings.GrassThinning > 0.0f) {
		changed |= ImGui::SliderFloat(T("feature.occlusion_culling.grass_thinning_curve", "Thinning Curve"), &settings.GrassThinningCurve, 0.25f, 8.0f, "%.2f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", T("feature.occlusion_culling.grass_thinning_curve_tooltip",
				"Shapes where thinning bites. Below 1 thins aggressively even up close; above 1 keeps near grass dense and pushes the thinning out toward the fade edge."));

		changed |= ImGui::SliderFloat(T("feature.occlusion_culling.grass_thinning_scale", "Thinning Scale-Up"), &settings.GrassThinningScale, 0.0f, 3.0f, "%.2f");
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", T("feature.occlusion_culling.grass_thinning_scale_tooltip",
				"Enlarges the surviving clumps to compensate for the thinned-out density, so the field still looks full. 0 = no compensation."));
	}

	ImGui::EndDisabled();

	if (changed)
		SyncSettingsToMOC();
}

void OcclusionCulling::Load(json& o_json)
{
	// No shader .ini -> base Feature::Load returns at its rc<0 guard before reaching LoadSettings, so persisted
	// settings would never be restored. Mirror the success branch directly (settings are keyed by GetName()).
	if (o_json[GetName()].is_structured()) {
		try {
			LoadSettings(o_json[GetName()]);
		} catch (...) {
			logger::warn("Invalid settings for {}, using default.", GetName());
			RestoreDefaultSettings();
		}
	} else {
		RestoreDefaultSettings();
	}
}

void OcclusionCulling::LoadSettings(json& o_json)
{
	if (o_json["EnableOcclusionTesting"].is_boolean())
		settings.EnableOcclusionTesting = o_json["EnableOcclusionTesting"];
	if (o_json["OccluderTestMinRadius"].is_number())
		settings.OccluderTestMinRadius = o_json["OccluderTestMinRadius"];
	if (o_json["CullSmallShadows"].is_boolean())
		settings.CullSmallShadows = o_json["CullSmallShadows"];
	if (o_json["SmallShadowMinSize"].is_number())
		settings.SmallShadowMinSize = o_json["SmallShadowMinSize"];
	if (o_json["SmallShadowSlope"].is_number())
		settings.SmallShadowSlope = o_json["SmallShadowSlope"];

	if (o_json["GrassCulling"].is_boolean())
		settings.GrassCulling = o_json["GrassCulling"];
	if (o_json["GrassOcclusion"].is_boolean())
		settings.GrassOcclusion = o_json["GrassOcclusion"];
	if (o_json["GrassThinning"].is_number())
		settings.GrassThinning = o_json["GrassThinning"];
	if (o_json["GrassThinningCurve"].is_number())
		settings.GrassThinningCurve = o_json["GrassThinningCurve"];
	if (o_json["GrassThinningScale"].is_number())
		settings.GrassThinningScale = o_json["GrassThinningScale"];

	SyncSettingsToMOC();
}

void OcclusionCulling::SaveSettings(json& o_json)
{
	o_json["EnableOcclusionTesting"] = settings.EnableOcclusionTesting;
	o_json["OccluderTestMinRadius"] = settings.OccluderTestMinRadius;
	o_json["CullSmallShadows"] = settings.CullSmallShadows;
	o_json["SmallShadowMinSize"] = settings.SmallShadowMinSize;
	o_json["SmallShadowSlope"] = settings.SmallShadowSlope;
	o_json["GrassCulling"] = settings.GrassCulling;
	o_json["GrassOcclusion"] = settings.GrassOcclusion;
	o_json["GrassThinning"] = settings.GrassThinning;
	o_json["GrassThinningCurve"] = settings.GrassThinningCurve;
	o_json["GrassThinningScale"] = settings.GrassThinningScale;
}

void OcclusionCulling::RestoreDefaultSettings()
{
	settings = {};
	SyncSettingsToMOC();
}
