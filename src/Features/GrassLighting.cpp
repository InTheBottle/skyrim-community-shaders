#include "GrassLighting.h"

#include "GrassOptimizations.h"
#include "I18n/I18n.h"

#define I18N_KEY_PREFIX "feature.grass_lighting."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GrassLighting::Settings,
	Glossiness,
	SpecularStrength,
	SubsurfaceScatteringAmount,
	OverrideComplexGrassSettings,
	BasicGrassBrightness,
	ComplexGrassThreshold,
	MidLODBrightness,
	FarLODBrightness,
	VertexAOStrength,
	SoftLighting,
	RootOcclusion,
	TipScattering,
	NormalStrength,
	SpecularAAStrength)

void GrassLighting::DrawSettings()
{
	if (ImGui::TreeNodeEx(T(TKEY("complex_grass"), "Complex Grass"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextWrapped("%s", T(TKEY("specular_desc"), "Specular highlights for complex grass"));
		ImGui::SliderFloat(T(TKEY("glossiness"), "Glossiness"), &settings.Glossiness, 1.0f, 100.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("glossiness_tooltip"), "Specular highlight glossiness."));
		}

		ImGui::SliderFloat(T(TKEY("specular_strength"), "Specular Strength"), &settings.SpecularStrength, 0.0f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("specular_strength_tooltip"), "Specular highlight strength."));
		}

		ImGui::SliderFloat(T(TKEY("normal_strength"), "Normal Strength"), &settings.NormalStrength, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("normal_strength_tooltip"), "Scales the tangent-space normal map. Below 1 flattens harsh normals, above 1 deepens them."));
		}

		ImGui::SliderFloat(T(TKEY("specular_aa"), "Specular Anti-Aliasing"), &settings.SpecularAAStrength, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("specular_aa_tooltip"), "Widens roughness where the normal varies quickly across a pixel, removing shimmering highlights on distant grass."));
		}

		ImGui::Spacing();
		ImGui::TextWrapped("%s", T(TKEY("detection_header"), "Complex Grass Detection"));
		ImGui::SliderFloat(T(TKEY("detection_threshold"), "Detection Threshold"), &settings.ComplexGrassThreshold, 0.001f, 0.1f, "%.3f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("detection_threshold_tooltip"),
								  "Threshold for detecting complex grass textures. Lower values are more strict."));
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("effects"), "Effects"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat(T(TKEY("sss_amount"), "SSS Amount"), &settings.SubsurfaceScatteringAmount, 0.0f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("sss_tooltip"),
								  "Subsurface Scattering (SSS) amount. "
								  "Soft lighting controls how evenly lit an object is. "
								  "Back lighting illuminates the back face of an object. "
								  "Combined to model the transport of light through the surface."));
		}

		ImGui::SliderFloat(T(TKEY("tip_scattering"), "Tip Scattering"), &settings.TipScattering, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("tip_scattering_tooltip"), "Biases subsurface scattering toward the thin blade tips and away from the thicker base."));
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("lighting"), "Lighting"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("override_complex"), "Override Complex Grass Lighting Settings"), (bool*)&settings.OverrideComplexGrassSettings);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("override_complex_tooltip"),
								  "Override the settings set by the grass mesh author. "
								  "Complex grass authors can define the brightness for their grass meshes. "
								  "However, some authors may not account for the extra lights available from Bottled Shaders. "
								  "This option will treat their grass settings like non-complex grass. "
								  "This was the default in Bottled Shaders < 0.7.0"));
		}

		ImGui::Spacing();
		ImGui::SliderFloat(T(TKEY("soft_lighting"), "Soft Lighting"), &settings.SoftLighting, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("soft_lighting_tooltip"), "Wraps direct light around the blade, strongest at the tips. Softens the hard terminator on flat grass cards."));
		}

		ImGui::SliderFloat(T(TKEY("root_occlusion"), "Root Occlusion"), &settings.RootOcclusion, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("root_occlusion_tooltip"), "Darkens ambient light toward the base of each blade, grounding grass against the terrain."));
		}

		ImGui::SliderFloat(T(TKEY("vertex_occlusion"), "Vertex Occlusion"), &settings.VertexAOStrength, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("vertex_occlusion_tooltip"), "How much of the mesh vertex colour darkening is applied as ambient occlusion."));
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TextWrapped("%s", T(TKEY("basic_grass"), "Basic Grass"));
		ImGui::SliderFloat(T(TKEY("brightness"), "Brightness"), &settings.BasicGrassBrightness, 0.0f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("brightness_tooltip"), "Darkens the grass textures to look better with the new lighting"));
		}

		if (globals::features::grassOptimizations.loaded) {
			ImGui::Spacing();
			ImGui::TextWrapped("%s", T(TKEY("mid_lod_grass"), "Middle LOD Grass"));
			ImGui::PushID("midlod");
			ImGui::SliderFloat(T(TKEY("lod_brightness"), "Brightness"), &settings.MidLODBrightness, 0.0f, 2.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("mid_lod_brightness_tooltip"),
									  "Raise or lower until middle-distance LOD grass matches the full-detail grass around it. Requires Grass Optimizations with Middle LOD enabled."));
			}
			ImGui::PopID();

			ImGui::Spacing();
			ImGui::TextWrapped("%s", T(TKEY("far_lod_grass"), "Far LOD Grass"));
			ImGui::PushID("farlod");
			ImGui::SliderFloat(T(TKEY("lod_brightness"), "Brightness"), &settings.FarLODBrightness, 0.0f, 2.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("far_lod_brightness_tooltip"),
									  "Raise or lower until the most distant LOD grass matches the middle LOD grass in front of it. Requires Grass Optimizations with Far LOD enabled."));
			}
			ImGui::PopID();
		}

		ImGui::TreePop();
	}
}

#undef I18N_KEY_PREFIX

void GrassLighting::LoadSettings(json& o_json)
{
	settings = o_json;
}

void GrassLighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void GrassLighting::RestoreDefaultSettings()
{
	settings = {};
}
