#include "SnowCover.h"

#include "ShaderCache.h"
#include "Util.h"
#include "Utils/FileSystem.h"
#include <DDSTextureLoader.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SnowCover::UserSettings,
	EnableExpensiveFoliage,
	AffectHavok,
	AffectFloraTint,
	SnowHeightOffset)

void copyString(const std::string& input, char* dst, size_t dst_size)
{
	strncpy(dst, input.c_str(), dst_size - 1);
	dst[dst_size - 1] = '\0';
}

void SnowCover::DrawSettings()
{
	ImGui::Checkbox("Enable Nicer Foliage", (bool*)&settings.EnableExpensiveFoliage);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Uses one more texture sample to put snow on edges of tree lods and grass.");
	}
	ImGui::Checkbox("Snow on Mobile Objects", (bool*)&settings.AffectHavok);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Movable clutter (Havok rigidbodies) such as barrels, crates and dropped items will receive snow.\n"
			"Snow stays on them while they move instead of vanishing. It is projected from world space,\n"
			"so it drifts across the surface while an object is tumbling. Actors are never affected.");
	}
	ImGui::Checkbox("Tint Harvestable Plants", (bool*)&settings.AffectFloraTint);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Lets the seasonal tree tint recolour harvestable flora such as mountain flowers,\n"
			"lavender and deathbell. Their petals are not green, so the shift towards autumn\n"
			"hues sends them to the wrong colour. Off by default; snow still settles on them.");
	}
	ImGui::SliderFloat("Snow Offset", &settings.SnowHeightOffset, -20000.0f, 20000.0f);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Moves the altitude that snow appears at. For testing purposes.");
	}
	ImGui::Checkbox("Melt Snow Near Fire", &fireMeltSettings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Clears snow around placed fires (campfires, braziers, hearths).\n"
			"Spells, projectiles and impacts are ignored. The %u nearest fires are used.",
			MAX_FIRE_MELT_SOURCES);
	}
	if (fireMeltSettings.Enabled) {
		ImGui::SliderFloat("Fire Melt Radius", &fireMeltSettings.RadiusScale, 1.0f, 16.0f, "%.1fx");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Melt radius as a multiple of the fire's size.");
		}
		ImGui::SliderFloat("Fire Melt Strength", &fireMeltSettings.Strength, 0.0f, 1.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("How much snow is removed at the fire. 1 = bare ground.");
		}
		ImGui::Text("Tracked fires: %u", static_cast<uint32_t>(trackedFires.size()));
	}
	ImGui::Separator();
	ImGui::Text("Each config applies to one worldspace or interior cell.");
	ImGui::Text("Saved config will be applied when you enter the worldspace.");

	if (ImGui::TreeNodeEx("Worldspace Config", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text("Current worldspace/cell: %s", last_worldspace.c_str());
		ImGui::Text("Config status: %s", status.c_str());

		ImGui::SameLine();
		if (ImGui::Button("Reload")) {
			last_worldspace = "";
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Reloads the config for current worldspace from file."
				"Reload is required to load new texture paths.");
		}
		ImGui::SameLine();
		if (ImGui::Button("Save")) {
			SaveConfig();
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Saves the current config to a file.");
		}
		ImGui::SameLine();
		if (ImGui::Button("Defaults")) {
			const uint enabled = wsettings.EnableSnowCover;
			ResetWorldConfig();
			wsettings.EnableSnowCover = enabled;
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Resets the current config to default values.");
		}
		ImGui::Separator();
		ImGui::Checkbox("Enable Snow Cover", (bool*)&wsettings.EnableSnowCover);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Enables the feature. This is set automatically when a config is found.");
		}
		if (wsettings.EnableSnowCover) {
			ImGui::Checkbox("Affect Grass Tint", (bool*)&wsettings.AffectGrassTint);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Should grass turn yellow?");
			}
			ImGui::Checkbox("Affect Tree Tint", (bool*)&wsettings.AffectTreeTint);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Should trees turn yellow?");
			}
			ImGui::SliderFloat("Foliage Color Height Offset", &wsettings.FoliageHeightOffset, -2000.0f, 2000.0f);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How far below/above the snow line should the foliage color start changing?");
			}
			ImGui::SliderFloat("Blend smoothness", &wsettings.BlendSmoothness, 1.f, 10000.0f);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How gradual the snow transition is.");
			}
			if (ImGui::TreeNodeEx("Distance")) {
				ImGui::SliderFloat("Weather Fade Start", &wsettings.WeatherFadeStart, 0.0f, 200000.0f, "%.0f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text(
						"Camera distance at which weather-driven snow starts fading out.\n"
						"Keep it above ~10000 or the fade lands on the terrain LOD and the snow front crawls with the camera.\n"
						"Seasonal and altitude snow is never distance-faded.");
				}
				ImGui::SliderFloat("Weather Fade End", &wsettings.WeatherFadeEnd, 0.0f, 300000.0f, "%.0f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Camera distance at which weather-driven snow is gone entirely.");
				}
				ImGui::SliderFloat("Object Fade Start", &wsettings.ObjectFadeStart, 0.0f, 40000.0f, "%.0f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Camera distance at which snow on statics and trees starts weakening.");
				}
				ImGui::SliderFloat("Object Fade End", &wsettings.ObjectFadeEnd, 0.0f, 40000.0f, "%.0f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Camera distance at which that falloff is complete.");
				}
				ImGui::SliderFloat("Object Fade Amount", &wsettings.ObjectFadeAmount, 0.0f, 1.0f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text(
						"Fraction of snow removed from distant statics and trees.\n"
						"Set to 0 to disable the falloff entirely. Keeps DynDOLOD ultra-tree billboards from reading as white slabs.");
				}
				ImGui::TreePop();
			}
			if (ImGui::TreeNodeEx("Weather and Seasons")) {
				ImGui::SliderInt("Maximum Summer Month", (int*)&MaxSummerMonth, 0, 11);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("In which month is the snow line highest?");
				}
				ImGui::SliderInt("Maximum Winter Month", (int*)&MaxWinterMonth, 0, 11);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("In which month is the snow line lowest?");
				}
				ImGui::SliderFloat("Summer Height Offset", &SummerHeightOffset, -20000.0f, 20000.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("What is the snow line altitude in summer?");
				}
				ImGui::SliderFloat("Winter Height Offset", &WinterHeightOffset, -20000.0f, 20000.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("What is the snow line altitude in winter?");
				}
				ImGui::SliderFloat("Snowing speed", &snowing_speed, 0.01f, 10.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("How fast snowy weather accumulates snow. Also depends on the weather settings.");
				}
				ImGui::SliderFloat("Melting speed", &melting_speed, 0.01f, 10.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("How fast snow (under snow line) disappears when it is not snowing.");
				}
				ImGui::TreePop();
			}

			if (ImGui::TreeNodeEx("Snow Map")) {
				ImGui::InputText("Map texture", mapbuf, 128);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Path to the map texture relative to Data folder. Interpreted as grayscale.");
				}
				bool boundsChanged = ImGui::InputFloat("Min X", &mapMin.x, 0.0f, 10.0f);
				boundsChanged |= ImGui::InputFloat("Min Y", &mapMin.y, 0.0f, 10.0f);
				boundsChanged |= ImGui::InputFloat("Max X", &mapMax.x, 0.0f, 10.0f);
				boundsChanged |= ImGui::InputFloat("Max Y", &mapMax.y, 0.0f, 10.0f);
				if (boundsChanged)
					UpdateMapTransform();

				map_tex = std::filesystem::path(mapbuf);
				ImGui::SliderFloat("Snow Map Z Scale", &wsettings.mapZscale, 0.1f, 10000.0f);
				ImGui::TreePop();
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("A grayscale map of the worldspace that offsets the altitude snow appears at. Relative to game Data folder, without '.dds' ");
			}

			if (ImGui::TreeNodeEx("Material")) {
				ImGui::SliderFloat("Min Angle", &wsettings.MinAngle, 0.0f, 1.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("The angle snow starts appearing at. 0 = horizontal, 1 = vertical.");
				}
				ImGui::SliderFloat("Max Angle", &wsettings.MaxAngle, 0.0f, 1.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("The angle snow is fully visible at. 0 = horizontal, 1 = vertical.");
				}
				ImGui::InputText("Main texture", tbuf, 128);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Path to the main texture relative to Data folder, without '.dds'. Needs to be a PBR texture with a diffuse, _n, and _rmaos.");
				}
				main_tex = std::string(tbuf);
				ImGui::InputText("Alt texture", altbuf, 128);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Optional path to the alternative texture relative to Data folder, without '.dds'. Needs to be a PBR texture with a diffuse, _n, and _rmaos.");
				}
				alt_tex = std::string(altbuf);
				ImGui::ColorEdit4("Main Tint", &wsettings.MainTint.x);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Tint for the main texture. Alpha only affects the color but not roughness etc.");
				}
				ImGui::InputFloat("Main Specular", &wsettings.MainSpec, 0.0f, 1.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("The main specular multiplier. Most materials = 0.04, snow = 0.02");
				}
				ImGui::ColorEdit4("Alt Tint", &wsettings.AltTint.x);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Tint for the alternative texture. Alpha only affects the color but not roughness etc.");
				}
				ImGui::InputFloat("Alt Specular", &wsettings.AltSpec, 0.0f, 1.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("The alternative specular multiplier. Most materials = 0.04, snow = 0.02");
				}
				ImGui::SliderFloat("Peak Main Angle", &wsettings.PeakMainAngle, 0.0f, 1.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("The angle at which the main texture is the strongest. The stronger wins. 0 = horizontal, 1 = vertical.");
				}
				ImGui::SliderFloat("Peak Alt Angle", &wsettings.PeakAltAngle, 0.0f, 1.0f);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("The angle at which the alternative texture is the strongest. The stronger wins. 0 = horizontal, 1 = vertical.");
				}
				ImGui::SliderFloat("UV Scale", &wsettings.UVScale, 0.1f, 10.f, "%.1f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("The UV scale of both textures.");
				}
				ImGui::Text("Glint");
				ImGui::SliderFloat("Screenspace Scale", &wsettings.ScreenSpaceScale, 0.f, 3.f, "%.3f");
				ImGui::SliderFloat("Log Microfacet Density", &wsettings.LogMicrofacetDensity, 0.f, 40.f, "%.3f");
				ImGui::SliderFloat("Microfacet Roughness", &wsettings.MicrofacetRoughness, 0.f, 1.f, "%.3f");
				ImGui::SliderFloat("Density Randomization", &wsettings.DensityRandomization, 0.f, 5.f, "%.3f");
				ImGui::TreePop();
			}
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Debug Info")) {
		ImGui::Text("Month: %.3f", perFrame.Month);
		ImGui::Text("TimeSnowing: %.3f", perFrame.TimeSnowing);
		ImGui::Text("SnowingDensity: %.3f", perFrame.SnowingDensity);
		if (debug_text != nullptr)
			ImGui::Text("Debug text: %s", debug_text);

		ImGui::TreePop();
	}

	ImGui::Spacing();
	ImGui::Spacing();
}

SnowCover::PerFrame SnowCover::GetCommonBufferData()
{
	Reload();

	bool snowing = false;
	bool raining = false;
	if (wsettings.EnableSnowCover) {
		snowingDensity = 0;
		if (auto sky = RE::Sky::GetSingleton()) {
			if (auto currentWeather = sky->currentWeather) {
				if (currentWeather->precipitationData) {
					float particleDensity = currentWeather->precipitationData->GetSettingValue(RE::BGSShaderParticleGeometryData::DataID::kParticleDensity).f;
					float particleGravity = currentWeather->precipitationData->GetSettingValue(RE::BGSShaderParticleGeometryData::DataID::kGravityVelocity).f;
					snowingDensity = particleDensity * particleGravity;
				}
				if (currentWeather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow)) {
					snowing = true;
				} else if (currentWeather->data.flags.any(RE::TESWeather::WeatherDataFlag::kRainy)) {
					raining = true;
				}
			}
		}
	} else {
		snowingDensity = 0;
	}
	if (auto calendar = RE::Calendar::GetSingleton()) {
		auto h = calendar->GetHour();
		auto diff = h < lastHour ? h + 24 - lastHour : h - lastHour;
		if (snowing)
			timeSnowing += diff * snowing_speed;
		else if (raining) {
			timeSnowing -= 2 * diff * melting_speed;
		} else {
			if (timeSnowing > 0) {
				timeSnowing = std::max(timeSnowing - diff * melting_speed, 0.0f);
			} else if (timeSnowing < 0) {
				timeSnowing = std::min(timeSnowing + diff * melting_speed, 0.0f);
			}
		}

		timeSnowing = std::clamp(timeSnowing, -2.0f, 2.0f);
		lastHour = h;

		auto time = calendar->GetTime();
		perFrame.Month = static_cast<float>(time.tm_mon + (time.tm_mday + (time.tm_hour + (time.tm_min + time.tm_sec / 60.0) / 60.0) / 24.0) / 32.0);
	}
	perFrame.SnowingDensity = snowingDensity;
	perFrame.TimeSnowing = timeSnowing;
	perFrame.SeasonalAltitude = GetSeasonalAltitude();
	perFrame.settings = settings;
	perFrame.wsettings = wsettings;

	if (fireMeltFrame.IsNewFrame())
		UpdateFireMelt();

	return perFrame;
}

void SnowCover::SetupResources()
{
	Reload();
	if (auto calendar = RE::Calendar::GetSingleton())
		lastHour = calendar->GetHour();
}

const char* GetWorldspace()
{
	auto curr_worldspace = "none";
	auto tes = RE::TES::GetSingleton();
	if (tes) {
		auto worldspace = tes->GetRuntimeData2().worldSpace;
		if (tes->interiorCell) {
			curr_worldspace = tes->interiorCell->GetFormEditorID();
		} else if (worldspace) {
			curr_worldspace = worldspace->GetFormEditorID();
		}
	}
	return (curr_worldspace && *curr_worldspace) ? curr_worldspace : "none";
}

void SnowCover::ResetWorldConfig()
{
	wsettings = WorldSettings{};
	MaxSummerMonth = DEFAULT_PEAK_SUMMER_MONTH;
	MaxWinterMonth = DEFAULT_PEAK_WINTER_MONTH;
	SummerHeightOffset = DEFAULT_SUMMER_HEIGHT_OFFSET;
	WinterHeightOffset = DEFAULT_WINTER_HEIGHT_OFFSET;
	snowing_speed = 1.0f;
	melting_speed = 1.0f;
	mapMin = DEFAULT_MAP_MIN;
	mapMax = DEFAULT_MAP_MAX;
	UpdateMapTransform();
	map_tex.clear();
	main_tex.clear();
	alt_tex.clear();
	mapbuf[0] = '\0';
	tbuf[0] = '\0';
	altbuf[0] = '\0';
}

void SnowCover::UpdateMapTransform()
{
	float2 extent = mapMax - mapMin;
	if (extent.x == 0.0f)
		extent.x = DEFAULT_MAP_MAX.x - DEFAULT_MAP_MIN.x;
	if (extent.y == 0.0f)
		extent.y = DEFAULT_MAP_MAX.y - DEFAULT_MAP_MIN.y;
	wsettings.mapScale = float2(1.0) / extent;
	wsettings.mapOffset = -mapMin * wsettings.mapScale;
}

void SnowCover::SaveConfig()
{
	if (last_worldspace.empty() || last_worldspace == "none") {
		status = "Not in a named worldspace or cell, nothing to save to.";
		return;
	}
	map_tex = std::filesystem::path(mapbuf);
	main_tex = std::filesystem::path(tbuf);
	alt_tex = std::filesystem::path(altbuf);
	json config = {
		{ "AffectGrassTint", wsettings.AffectGrassTint },
		{ "AffectTreeTint", wsettings.AffectTreeTint },
		{ "FoliageHeightOffset", wsettings.FoliageHeightOffset },
		{ "UVScale", wsettings.UVScale },
		{ "MaxSummerMonth", MaxSummerMonth },
		{ "MaxWinterMonth", MaxWinterMonth },
		{ "SummerHeightOffset", SummerHeightOffset },
		{ "WinterHeightOffset", WinterHeightOffset },
		{ "MapTexture", map_tex.generic_string() },
		{ "MapZscale", wsettings.mapZscale },
		{ "BlendSmoothness", wsettings.BlendSmoothness },
		{ "WeatherFadeStart", wsettings.WeatherFadeStart },
		{ "WeatherFadeEnd", wsettings.WeatherFadeEnd },
		{ "ObjectFadeStart", wsettings.ObjectFadeStart },
		{ "ObjectFadeEnd", wsettings.ObjectFadeEnd },
		{ "ObjectFadeAmount", wsettings.ObjectFadeAmount },
		{ "ScreenSpaceScale", wsettings.ScreenSpaceScale },
		{ "LogMicrofacetDensity", wsettings.LogMicrofacetDensity },
		{ "MicrofacetRoughness", wsettings.MicrofacetRoughness },
		{ "DensityRandomization", wsettings.DensityRandomization },
		{ "MapMin", json::array({ mapMin.x, mapMin.y }) },
		{ "MapMax", json::array({ mapMax.x, mapMax.y }) },
		{ "MainTexture", main_tex.generic_string() },
		{ "AltTexture", alt_tex.generic_string() },
		{ "MainTint", json::array({ wsettings.MainTint.x,
						  wsettings.MainTint.y,
						  wsettings.MainTint.z,
						  wsettings.MainTint.w }) },
		{ "AltTint", json::array({ wsettings.AltTint.x,
						 wsettings.AltTint.y,
						 wsettings.AltTint.z,
						 wsettings.AltTint.w }) },
		{ "SnowingSpeed", snowing_speed },
		{ "MeltingSpeed", melting_speed },
		{ "PeakMainAngle", wsettings.PeakMainAngle },
		{ "PeakAltAngle", wsettings.PeakAltAngle },
		{ "MinAngle", wsettings.MinAngle },
		{ "MaxAngle", wsettings.MaxAngle },
		{ "MainSpec", wsettings.MainSpec },
		{ "AltSpec", wsettings.AltSpec },
	};

	auto path = (Util::PathHelpers::GetShadersPath() / "SnowCover" / last_worldspace).replace_extension(std::filesystem::path(".json"));
	try {
		std::ofstream file(path);
		file << config.dump(4);
		file.close();
		if (!file) {
			status = std::format("Failed to save {}", path.generic_string());
			logger::error("[Snow Cover] Failed to write {}", path.generic_string());
			return;
		}
	} catch (const std::exception& e) {
		status = std::format("Failed to save {}", path.generic_string());
		logger::error("[Snow Cover] Error saving {}: {}", path.generic_string(), e.what());
		return;
	}
	status = std::format("Saved {}", path.generic_string());
}

void SnowCover::Reload()
{
	std::ifstream fileStream;
	std::filesystem::path path;
	try {
		std::string curr_worldspace = GetWorldspace();
		if (curr_worldspace == last_worldspace)
			return;
		last_worldspace = curr_worldspace;
		ResetWorldConfig();
		path = (Util::PathHelpers::GetShadersPath() / "SnowCover" / curr_worldspace).replace_extension(std::filesystem::path(".json"));
		if (!std::filesystem::exists(path)) {
			status = std::format("Config doesn't exist {}", path.generic_string());
			wsettings.EnableSnowCover = false;
			return;
		}
		fileStream = std::ifstream(path);
		if (!fileStream.is_open()) {
			status = std::string("Cannot open config.");
			wsettings.EnableSnowCover = false;
			logger::error("[Snow Cover] Cannot open config at {}", path.generic_string());
			return;
		}
	} catch (const std::system_error& e) {
		logger::error("[Snow Cover] Error opening file: {}", e.what());
	}

	auto whitelist_path = Util::PathHelpers::GetShadersPath() / "SnowCover" / "whitelist.txt";
	auto blacklist_path = Util::PathHelpers::GetShadersPath() / "SnowCover" / "blacklist.txt";

	whitelist = FormIdParser::parseTriNameFile(whitelist_path);
	blacklist = FormIdParser::parseTriNameFile(blacklist_path);

	json config;
	try {
		fileStream >> config;
		wsettings.AffectGrassTint = config["AffectGrassTint"];
		wsettings.AffectTreeTint = config["AffectTreeTint"];
		wsettings.FoliageHeightOffset = config["FoliageHeightOffset"];
		wsettings.UVScale = config["UVScale"];
		MaxSummerMonth = config["MaxSummerMonth"];
		MaxWinterMonth = config["MaxWinterMonth"];
		SummerHeightOffset = config["SummerHeightOffset"];
		WinterHeightOffset = config["WinterHeightOffset"];
		mapMin = float2(config["MapMin"][0], config["MapMin"][1]);
		mapMax = float2(config["MapMax"][0], config["MapMax"][1]);
		UpdateMapTransform();
		wsettings.mapZscale = config["MapZscale"];
		wsettings.BlendSmoothness = config["BlendSmoothness"];
		// Shipped worldspace configs predate the distance knobs; operator[] would throw on them.
		wsettings.WeatherFadeStart = config.value("WeatherFadeStart", DEFAULT_WEATHER_FADE_START);
		wsettings.WeatherFadeEnd = config.value("WeatherFadeEnd", DEFAULT_WEATHER_FADE_END);
		wsettings.ObjectFadeStart = config.value("ObjectFadeStart", DEFAULT_OBJECT_FADE_START);
		wsettings.ObjectFadeEnd = config.value("ObjectFadeEnd", DEFAULT_OBJECT_FADE_END);
		wsettings.ObjectFadeAmount = config.value("ObjectFadeAmount", DEFAULT_OBJECT_FADE_AMOUNT);
		wsettings.ScreenSpaceScale = config["ScreenSpaceScale"];
		wsettings.LogMicrofacetDensity = config["LogMicrofacetDensity"];
		wsettings.MicrofacetRoughness = config["MicrofacetRoughness"];
		wsettings.DensityRandomization = config["DensityRandomization"];
		wsettings.MainTint = float4(config["MainTint"][0], config["MainTint"][1], config["MainTint"][2], config["MainTint"][3]);
		wsettings.AltTint = float4(config["AltTint"][0], config["AltTint"][1], config["AltTint"][2], config["AltTint"][3]);
		snowing_speed = config["SnowingSpeed"];
		melting_speed = config["MeltingSpeed"];
		wsettings.PeakMainAngle = config["PeakMainAngle"];
		wsettings.PeakAltAngle = config["PeakAltAngle"];
		wsettings.MinAngle = config["MinAngle"];
		wsettings.MaxAngle = config["MaxAngle"];
		wsettings.MainSpec = config["MainSpec"];
		wsettings.AltSpec = config["AltSpec"];
		main_tex = std::filesystem::path(config["MainTexture"].get<std::string>());
		copyString(main_tex.generic_string().c_str(), tbuf, 256);
		auto device = globals::d3d::device;
		auto context = globals::d3d::context;
		for (auto& view : views)
			view = nullptr;
		auto data_path = Util::PathHelpers::GetDataPath();
		auto loadDDS = [&](const std::filesystem::path& file) -> winrt::com_ptr<ID3D11ShaderResourceView> {
			winrt::com_ptr<ID3D11ShaderResourceView> view;
			if (FAILED(DirectX::CreateDDSTextureFromFile(device, context, file.native().c_str(), nullptr, view.put())))
				return nullptr;
			return view;
		};

		views[0] = loadDDS(std::filesystem::path(data_path / main_tex).replace_extension(".dds"));
		if (!views[0]) {
			logger::warn("Snow Cover: Error loading {}.dds texture", main_tex.generic_string());
			views[0] = loadDDS(Util::PathHelpers::GetShadersPath() / "SnowCover" / "default" / "main.dds");
		}
		views[1] = loadDDS(std::filesystem::path(data_path / main_tex).concat("_n.dds"));
		if (!views[1]) {
			logger::warn("Snow Cover: Error loading {}_n.dds texture", main_tex.generic_string());
			views[1] = loadDDS(Util::PathHelpers::GetShadersPath() / "SnowCover" / "default" / "main_n.dds");
		}
		views[2] = loadDDS(std::filesystem::path(data_path / main_tex).concat("_rmaos.dds"));
		if (!views[2]) {
			logger::warn("Snow Cover: Error loading {}_rmaos.dds texture", main_tex.generic_string());
			views[2] = loadDDS(Util::PathHelpers::GetShadersPath() / "SnowCover" / "default" / "main_rmaos.dds");
		}

		if (config.contains("AltTexture") && config["AltTexture"] != "") {
			alt_tex = std::filesystem::path(config["AltTexture"].get<std::string>());
			copyString(alt_tex.generic_string().c_str(), altbuf, 256);
			views[3] = loadDDS(std::filesystem::path(data_path / alt_tex).replace_extension(".dds"));
			if (!views[3]) {
				logger::warn("Snow Cover: Error loading {}.dds texture", alt_tex.generic_string());
				views[3] = views[0];
			}
			views[4] = loadDDS(std::filesystem::path(data_path / alt_tex).concat("_n.dds"));
			if (!views[4]) {
				logger::warn("Snow Cover: Error loading {}_n.dds texture", alt_tex.generic_string());
				views[4] = views[1];
			}
			views[5] = loadDDS(std::filesystem::path(data_path / alt_tex).concat("_rmaos.dds"));
			if (!views[5]) {
				logger::warn("Snow Cover: Error loading {}_rmaos.dds texture", alt_tex.generic_string());
				views[5] = views[2];
			}
		} else {
			views[3] = views[0];
			views[4] = views[1];
			views[5] = views[2];
		}

		map_tex = std::filesystem::path(config["MapTexture"].get<std::string>());
		copyString(map_tex.generic_string().c_str(), mapbuf, 256);
		views[6] = loadDDS(std::filesystem::path(data_path / map_tex).concat(".dds"));
		if (!views[6]) {
			logger::warn("Snow Cover: Error loading {}.dds texture", map_tex.generic_string());
			views[6] = loadDDS(data_path / "textures" / "gray.dds");
		}
		wsettings.EnableSnowCover = true;
	} catch (const nlohmann::json::parse_error& e) {
		logger::error("[Snow Cover] failed to parse {} : {}", path.generic_string(), e.what());
		status = e.what();
		ResetWorldConfig();
		return;
	} catch (const nlohmann::json::exception& e) {
		logger::error("[Snow Cover] failed to parse {} : {}", path.generic_string(), e.what());
		status = e.what();
		ResetWorldConfig();
		return;
	} catch (...) {
		logger::error("[Snow Cover] unknown error when loading a config: {}", path.generic_string());
		status = std::string("Unknown error.");
		return;
	}
	status = std::string("Loaded.");
}

void SnowCover::Prepass()
{
	if (wsettings.EnableSnowCover) {
		ID3D11ShaderResourceView* srvs[std::tuple_size_v<decltype(views)>]{};
		for (size_t i = 0; i < views.size(); ++i)
			srvs[i] = views[i].get();
		globals::d3d::context->PSSetShaderResources(FIRST_SRV_SLOT, (uint)views.size(), srvs);
	}
}

void SnowCover::LoadSettings(json& o_json)
{
	settings = o_json;
	const FireMeltSettings defaults{};
	fireMeltSettings.Enabled = o_json.value("FireMeltEnabled", defaults.Enabled);
	fireMeltSettings.RadiusScale = std::clamp(o_json.value("FireMeltRadiusScale", defaults.RadiusScale), 1.0f, 16.0f);
	fireMeltSettings.Strength = std::clamp(o_json.value("FireMeltStrength", defaults.Strength), 0.0f, 1.0f);
}

void SnowCover::SaveSettings(json& o_json)
{
	o_json = settings;
	o_json["FireMeltEnabled"] = fireMeltSettings.Enabled;
	o_json["FireMeltRadiusScale"] = fireMeltSettings.RadiusScale;
	o_json["FireMeltStrength"] = fireMeltSettings.Strength;
}

void SnowCover::RestoreDefaultSettings()
{
	settings = {};
	fireMeltSettings = {};
}

bool SnowCover::IsWorldFireSource(RE::TESBoundObject* a_base)
{
	if (!a_base)
		return false;
	switch (a_base->GetFormType()) {
	case RE::FormType::Static:
	case RE::FormType::MovableStatic:
	case RE::FormType::Light:
	case RE::FormType::Activator:
	case RE::FormType::TalkingActivator:
	case RE::FormType::Furniture:
		return true;
	default:
		return false;
	}
}

bool SnowCover::IsFireGeometry(RE::BSGeometry* a_geometry)
{
	auto& data = a_geometry->GetGeometryRuntimeData();
	auto* property = data.shaderProperty.get();
	auto* alpha = data.alphaProperty.get();
	if (!property || !alpha || property->GetRTTI() != globals::rtti::BSEffectShaderPropertyRTTI.get())
		return false;
	if ((alpha->alphaFlags & 0x1E0) != 0)
		return false;
	auto* material = static_cast<RE::BSEffectShaderMaterial*>(property->material);
	if (!material)
		return false;

	using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
	if (property->flags.any(Flag::kSoftEffect))
		return !material->greyscaleTexturePath.empty() && property->flags.all(Flag::kGrayscaleToPaletteColor, Flag::kGrayscaleToPaletteAlpha);

	static REL::Relocation<const RE::NiRTTI*> stripParticlesRTTI{ RE::NiRTTI_BSStripParticleSystem };
	auto* particles = a_geometry->AsParticlesGeom();
	if (!particles || a_geometry->GetRTTI() == stripParticlesRTTI.get() || material->sourceTexturePath.empty())
		return false;
	auto* particleData = particles->GetParticlesRuntimeData().particleData.get();
	return particleData && particleData->GetParticlesRuntimeData().subTextureOffsetsCount != 0;
}

void SnowCover::CollectFireGeometry(RE::NiAVObject* a_object, bool a_hidden, bool& a_hasFire, std::vector<RE::NiBound>& a_samples)
{
	if (!a_object)
		return;
	const bool hidden = a_hidden || a_object->GetAppCulled();
	if (auto* geometry = a_object->AsGeometry()) {
		if (!IsFireGeometry(geometry))
			return;
		a_hasFire = true;
		if (!hidden && geometry->worldBound.radius > 0.0f)
			a_samples.push_back(geometry->worldBound);
		return;
	}
	if (auto* node = a_object->AsNode()) {
		for (auto& child : node->GetChildren())
			CollectFireGeometry(child.get(), hidden, a_hasFire, a_samples);
	}
}

bool SnowCover::ScanFireSources()
{
	for (auto& fire : trackedFires)
		++fire.missedScans;

	auto* tes = RE::TES::GetSingleton();
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!tes || !player)
		return false;

	auto merge = [](const RE::NiBound& a_lhs, const RE::NiBound& a_rhs) {
		const RE::NiPoint3 delta = a_rhs.center - a_lhs.center;
		const float distance = delta.Length();
		if (distance + a_rhs.radius <= a_lhs.radius)
			return a_lhs;
		if (distance + a_lhs.radius <= a_rhs.radius)
			return a_rhs;
		RE::NiBound combined;
		combined.radius = 0.5f * (distance + a_lhs.radius + a_rhs.radius);
		combined.center = a_lhs.center + delta * ((combined.radius - a_lhs.radius) / distance);
		return combined;
	};

	const auto eye = Util::GetEyePosition();
	uint32_t classified = 0;
	fireClusters.clear();

	tes->ForEachReferenceInRange(player, FIRE_MELT_MAX_DISTANCE, [&](RE::TESObjectREFR* a_ref) {
		if (!a_ref || a_ref->IsDisabled() || a_ref->IsDeleted())
			return RE::BSContainer::ForEachResult::kContinue;
		auto* base = a_ref->GetBaseObject();
		if (!IsWorldFireSource(base))
			return RE::BSContainer::ForEachResult::kContinue;
		const auto cached = fireBaseCache.find(base->GetFormID());
		const bool known = cached != fireBaseCache.end();
		if ((known && !cached->second) || (!known && classified >= MAX_FIRE_BASE_CLASSIFICATIONS_PER_SCAN))
			return RE::BSContainer::ForEachResult::kContinue;
		auto* root = a_ref->Get3D();
		if (!root)
			return RE::BSContainer::ForEachResult::kContinue;

		bool hasFire = false;
		fireSamples.clear();
		CollectFireGeometry(root, false, hasFire, fireSamples);
		if (!known) {
			fireBaseCache.emplace(base->GetFormID(), hasFire);
			++classified;
		}

		const auto refPosition = a_ref->GetPosition();
		const size_t first = fireClusters.size();
		for (const auto& sample : fireSamples) {
			if (sample.center.GetSquaredDistance(refPosition) > FIRE_MELT_MAX_REF_OFFSET * FIRE_MELT_MAX_REF_OFFSET)
				continue;
			auto cluster = std::find_if(fireClusters.begin() + first, fireClusters.end(), [&](const FireCluster& a_cluster) {
				return a_cluster.bound.center.GetSquaredDistance(sample.center) <= FIRE_MELT_CLUSTER_DISTANCE * FIRE_MELT_CLUSTER_DISTANCE;
			});
			if (cluster != fireClusters.end())
				cluster->bound = merge(cluster->bound, sample);
			else
				fireClusters.push_back({ a_ref->GetFormID(), static_cast<uint32_t>(fireClusters.size() - first), sample, 0.0f });
		}
		return RE::BSContainer::ForEachResult::kContinue;
	});

	for (auto& cluster : fireClusters)
		cluster.distanceSq = eye.GetSquaredDistance(cluster.bound.center);
	const size_t keep = std::min<size_t>(fireClusters.size(), MAX_TRACKED_FIRES);
	std::partial_sort(fireClusters.begin(), fireClusters.begin() + keep, fireClusters.end(), [](const FireCluster& a, const FireCluster& b) { return a.distanceSq < b.distanceSq; });

	for (size_t i = 0; i < keep; ++i) {
		const auto& cluster = fireClusters[i];
		const float radius = std::clamp(cluster.bound.radius, FIRE_MELT_MIN_RADIUS, FIRE_MELT_MAX_RADIUS);
		auto fire = std::find_if(trackedFires.begin(), trackedFires.end(), [&](const TrackedFire& a_fire) {
			return a_fire.refID == cluster.refID && a_fire.cluster == cluster.cluster;
		});
		if (fire == trackedFires.end()) {
			trackedFires.push_back({ cluster.refID, cluster.cluster, cluster.bound.center, radius, cluster.bound.center, radius, fireMeltSnap ? 1.0f : 0.0f, 0 });
			continue;
		}
		fire->sampleCenter = cluster.bound.center;
		fire->sampleRadius = radius;
		fire->missedScans = 0;
	}
	return classified < MAX_FIRE_BASE_CLASSIFICATIONS_PER_SCAN;
}

void SnowCover::UpdateFireMelt()
{
	if (!wsettings.EnableSnowCover || !fireMeltSettings.Enabled || fireMeltSettings.Strength <= 0.0f || globals::state->isLoadingMenuOpen) {
		trackedFires.clear();
		fireScanTimer = FIRE_MELT_SCAN_INTERVAL;
		fireMeltSnap = true;
		UploadFireMelt();
		return;
	}

	const float dt = std::clamp(RE::GetSecondsSinceLastFrame(), 0.0f, 0.1f);
	fireScanTimer += dt;
	if (fireScanTimer >= FIRE_MELT_SCAN_INTERVAL) {
		fireScanTimer = 0.0f;
		if (ScanFireSources())
			fireMeltSnap = false;
	}

	const float centerBlend = 1.0f - std::exp(-dt / FIRE_MELT_CENTER_SMOOTHING);
	const float growBlend = 1.0f - std::exp(-dt / FIRE_MELT_GROW_SMOOTHING);
	const float shrinkBlend = 1.0f - std::exp(-dt / FIRE_MELT_SHRINK_SMOOTHING);
	const float fadeStep = dt / FIRE_MELT_FADE_TIME;
	for (auto& fire : trackedFires) {
		fire.center += (fire.sampleCenter - fire.center) * centerBlend;
		fire.radius += (fire.sampleRadius - fire.radius) * (fire.sampleRadius > fire.radius ? growBlend : shrinkBlend);
		fire.strength = std::clamp(fire.strength + (fire.missedScans < FIRE_MELT_GRACE_SCANS ? fadeStep : -fadeStep), 0.0f, 1.0f);
	}
	std::erase_if(trackedFires, [](const TrackedFire& a_fire) { return a_fire.missedScans >= FIRE_MELT_GRACE_SCANS && a_fire.strength <= 0.0f; });

	UploadFireMelt();
}

void SnowCover::UploadFireMelt()
{
	auto& fireMelt = perFrame.fireMelt;
	fireMelt.Count = 0;
	fireMelt.Strength = fireMeltSettings.Strength;
	fireMelt.RadiusScale = fireMeltSettings.RadiusScale;
	if (trackedFires.empty())
		return;

	const auto eye = Util::GetEyePosition();
	fireOrder.clear();
	for (uint32_t i = 0; i < trackedFires.size(); ++i) {
		if (trackedFires[i].strength > 0.0f)
			fireOrder.emplace_back(eye.GetDistance(trackedFires[i].center), i);
	}
	const size_t ranked = std::min<size_t>(fireOrder.size(), MAX_FIRE_MELT_SOURCES + 1);
	std::partial_sort(fireOrder.begin(), fireOrder.begin() + ranked, fireOrder.end());

	const float slotCutoff = fireOrder.size() > MAX_FIRE_MELT_SOURCES ? fireOrder[MAX_FIRE_MELT_SOURCES].first : std::numeric_limits<float>::max();
	for (size_t i = 0; i < std::min<size_t>(ranked, MAX_FIRE_MELT_SOURCES); ++i) {
		const auto [distance, index] = fireOrder[i];
		const auto& fire = trackedFires[index];
		const float weight = fire.strength *
		                     std::clamp((FIRE_MELT_MAX_DISTANCE - distance) / FIRE_MELT_DISTANCE_FADE, 0.0f, 1.0f) *
		                     std::clamp((slotCutoff - distance) / FIRE_MELT_SLOT_FADE, 0.0f, 1.0f);
		const float radius = fire.radius * weight;
		if (radius >= 1.0f)
			fireMelt.Spheres[fireMelt.Count++] = { fire.center.x, fire.center.y, fire.center.z, radius };
	}
}

void SnowCover::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	if (globals::features::snowCover.wsettings.EnableSnowCover) {
		globals::features::snowCover.BSLightingShader_Setup(Pass);
	}
	func(This, Pass, RenderFlags);
}

void SnowCover::BSLightingShader_Setup(RE::BSRenderPass* a_pass)
{
	auto state = globals::state;
	auto userData = a_pass->geometry->GetUserData();
	auto name = a_pass->geometry->name.c_str();
	// Hash once per pass; fnv_hash dereferences unguarded and BSFixedString::c_str() can be null.
	const uint64_t nameHash = name ? FormIdParser::fnv_hash(name) : 0;
	if ((a_pass->geometry->HasAnimation() || (userData && ((userData->GetObjectReference() && userData->GetObjectReference()->IsBoundAnimObject()) || userData->CanBeMoved()))) && !(name && whitelist.contains(nameHash))) {
		if (settings.AffectHavok && userData && userData->formType != RE::FormType::ActorCharacter && userData->CanBeMoved())
			state->permutationData.ExtraShaderDescriptor &= ~(uint)State::ExtraShaderDescriptors::NoSnow;
		else
			state->permutationData.ExtraShaderDescriptor |= (uint)State::ExtraShaderDescriptors::NoSnow;
	} else if (name && blacklist.contains(nameHash)) {
		state->permutationData.ExtraShaderDescriptor |= (uint)State::ExtraShaderDescriptors::NoSnow;
	} else {
		state->permutationData.ExtraShaderDescriptor &= ~(uint)State::ExtraShaderDescriptors::NoSnow;
	}

	if (!settings.AffectFloraTint && IsHarvestableFlora(userData))
		state->permutationData.ExtraShaderDescriptor |= (uint)State::ExtraShaderDescriptors::NoFoliageTint;
	else
		state->permutationData.ExtraShaderDescriptor &= ~(uint)State::ExtraShaderDescriptors::NoFoliageTint;
}

bool SnowCover::IsHarvestableFlora(RE::TESObjectREFR* a_ref)
{
	if (!a_ref)
		return false;
	auto base = a_ref->GetObjectReference();
	if (!base)
		return false;
	switch (base->GetFormType()) {
	case RE::FormType::Flora:
		return true;
	case RE::FormType::Tree:
		return static_cast<RE::TESObjectTREE*>(base)->produceItem != nullptr;
	default:
		return false;
	}
}
