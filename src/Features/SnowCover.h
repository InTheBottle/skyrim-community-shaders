#pragma once

#include "Buffer.h"
#include "Feature.h"
#include "State.h"
#include "TruePBR.h"
#include "Utils/FormIdParser.h"

struct SnowCover : Feature
{
private:
	static constexpr float DEFAULT_FOLIAGE_EFFECT_OFFSET = -2048.0f;  // color foliage earlier/later than snow appears
	static constexpr float DEFAULT_UV_SCALE = 0.5f;
	static constexpr float DEFAULT_PEAK_MAIN_ANGLE = 0.45f;  // material is strongest at this angle
	static constexpr float DEFAULT_PEAK_ALT_ANGLE = 0.9f;    // material is strongest at this angle
	static constexpr float DEFAULT_MIN_ANGLE = 0.3f;         // lowest angle snow appears at
	static constexpr float DEFAULT_MAX_ANGLE = 0.9f;         // angle for full opacity snow
	static constexpr float DEFAULT_MAIN_SPEC = 0.02f;        // specular for main material
	static constexpr float DEFAULT_ALT_SPEC = 0.02f;         // specular for alt material
	static constexpr float DEFAULT_MAP_ZSCALE = 75000.0f;    // vertical scale of the map of 'altitude offsets'
	static constexpr float DEFAULT_GLINT_1 = 1.2f;           // glint values based on Faultier's snow
	static constexpr float DEFAULT_GLINT_2 = 33.f;
	static constexpr float DEFAULT_GLINT_3 = .15f;
	static constexpr float DEFAULT_GLINT_4 = 2.f;
	static constexpr float DEFAULT_BLEND_SMOOTHNESS = 5000.0f;              // range in game units in which the snow transition gradually happens
	static constexpr float DEFAULT_WEATHER_FADE_START = 60000.0f;           // weather snow stays at full strength past the terrain LOD rings
	static constexpr float DEFAULT_WEATHER_FADE_END = 150000.0f;            // ...and dissolves out around the fog horizon
	static constexpr float DEFAULT_OBJECT_FADE_START = 6144.0f;             // where snow on statics and trees starts weakening
	static constexpr float DEFAULT_OBJECT_FADE_END = 9192.0f;               // where that falloff is complete
	static constexpr float DEFAULT_OBJECT_FADE_AMOUNT = 0.5f;               // fraction of snow removed past the falloff; 0 disables it
	static constexpr float2 DEFAULT_MAP_MIN = float2(-233472.0, 208896.0);  // one corner of skyrim map (where cells end)
	static constexpr float2 DEFAULT_MAP_MAX = float2(253952.0, -176128.0);  // other corner of skyrim map
	static constexpr float DEFAULT_SUMMER_HEIGHT_OFFSET = 20000.0f;         // how high snow is in summer (in game units)
	static constexpr float DEFAULT_WINTER_HEIGHT_OFFSET = -20000.0f;        // how high snow is in winter (in game units)
	static constexpr uint DEFAULT_PEAK_SUMMER_MONTH = 6;
	static constexpr uint DEFAULT_PEAK_WINTER_MONTH = 0;
	static constexpr uint32_t FIRST_SRV_SLOT = 38;  // t38-t44, see SnowCover.hlsli

	static constexpr uint32_t MAX_FIRE_SOURCES = 16;
	static constexpr uint32_t FIRE_SEEN_GRACE_TICKS = 4;    // tolerate a few culled frames before fading out
	static constexpr float FIRE_FADE_IN_RATE = 1.6f;        // melt strength gained per second while lit
	static constexpr float FIRE_FADE_OUT_RATE = 0.02f;      // melt strength lost per second once gone; snow re-covers over ~50s
	static constexpr float FIRE_MERGE_DISTANCE = 96.0f;     // one fire emits many draws
	static constexpr float FIRE_MAX_BOUND_RADIUS = 512.0f;  // larger additive draws are not flames
	static constexpr float FIRE_MAX_MELT_RADIUS = 1024.0f;

public:
	virtual inline std::string GetName() { return "Snow Cover"; }
	virtual inline std::string GetShortName() { return "SnowCover"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kLandscapeAndTextures; }
	inline std::string_view GetShaderDefineName() override { return "SNOW_COVER"; }

	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	struct UserSettings
	{
		uint EnableExpensiveFoliage = 1;
		float SnowHeightOffset = 0.0f;
		uint AffectHavok = 0;
		uint AffectFloraTint = 0;

		uint EnableFireMelt = 1;
		float FireRadiusScale = 3.0f;
		float FireInnerScale = 0.35f;
		float FireMaxDistance = 12288.0f;
	};
	static_assert(sizeof(UserSettings) % 16 == 0);

	struct WorldSettings
	{
		uint EnableSnowCover = false;
		uint AffectGrassTint = true;
		uint AffectTreeTint = true;
		float FoliageHeightOffset = DEFAULT_FOLIAGE_EFFECT_OFFSET;

		float UVScale = DEFAULT_UV_SCALE;
		float PeakMainAngle = DEFAULT_PEAK_MAIN_ANGLE;
		float PeakAltAngle = DEFAULT_PEAK_ALT_ANGLE;
		float MinAngle = DEFAULT_MIN_ANGLE;

		float MaxAngle = DEFAULT_MAX_ANGLE;
		float MainSpec = DEFAULT_MAIN_SPEC;
		float AltSpec = DEFAULT_ALT_SPEC;
		float mapZscale = DEFAULT_MAP_ZSCALE;

		float2 mapScale;
		float2 mapOffset;

		//glint
		float ScreenSpaceScale = DEFAULT_GLINT_1;
		float LogMicrofacetDensity = DEFAULT_GLINT_2;
		float MicrofacetRoughness = DEFAULT_GLINT_3;
		float DensityRandomization = DEFAULT_GLINT_4;

		float4 MainTint = float4(1.0f, 1.0f, 1.0f, 1.0f);
		float4 AltTint = float4(1.0f, 1.0f, 1.0f, 1.0f);

		float BlendSmoothness = DEFAULT_BLEND_SMOOTHNESS;
		float WeatherFadeStart = DEFAULT_WEATHER_FADE_START;
		float WeatherFadeEnd = DEFAULT_WEATHER_FADE_END;
		float ObjectFadeStart = DEFAULT_OBJECT_FADE_START;

		float ObjectFadeEnd = DEFAULT_OBJECT_FADE_END;
		float ObjectFadeAmount = DEFAULT_OBJECT_FADE_AMOUNT;
		uint pad[2];
	};
	static_assert(sizeof(WorldSettings) % 16 == 0);
	// Mirrors SharedData.hlsli's SnowCoverSettings; drift corrupts every struct after it.
	static_assert(sizeof(WorldSettings) == 144);

	struct alignas(16) PerFrame
	{
		float Month;
		float TimeSnowing;
		float SnowingDensity;
		float SeasonalAltitude;

		UserSettings settings;
		WorldSettings wsettings;

		uint FireCount;
		uint firePad[3];
		float4 FireSources[MAX_FIRE_SOURCES];  // xyz = world position, w = melt radius
	};
	static_assert(sizeof(PerFrame) % 16 == 0);
	static_assert(sizeof(PerFrame) == 464);

	UserSettings settings;
	WorldSettings wsettings;
	PerFrame perFrame;

	PerFrame GetCommonBufferData();

	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 7> views;

	struct FireSource
	{
		RE::NiPoint3 position;
		float radius;           // full melt radius while lit
		float strength = 0.0f;  // eased 0..1 so melt grows in and recedes out smoothly
		uint32_t lastSeenTick;
	};
	std::vector<FireSource> fireSources;
	std::mutex fireSourcesMutex;
	uint32_t fireTick = 0;

	void CheckFireSource(RE::BSRenderPass* a_pass, uint32_t a_descriptor);

	std::string status;
	std::string last_worldspace;  // owned copy + content comparison; a raw editorID pointer can dangle/rehash
	std::filesystem::path map_tex;
	std::filesystem::path main_tex;
	std::filesystem::path alt_tex;
	char mapbuf[256] = "";
	char tbuf[256] = "";
	char altbuf[256] = "";

	float snowing_speed = 0.0f;
	float melting_speed = 0.0f;
	float2 mapMin = DEFAULT_MAP_MIN;
	float2 mapMax = DEFAULT_MAP_MAX;
	uint MaxSummerMonth = DEFAULT_PEAK_SUMMER_MONTH;
	uint MaxWinterMonth = DEFAULT_PEAK_WINTER_MONTH;
	float SummerHeightOffset = DEFAULT_SUMMER_HEIGHT_OFFSET;
	float WinterHeightOffset = DEFAULT_WINTER_HEIGHT_OFFSET;

	float lastHour = 12;
	float timeSnowing = 0.0f;
	float snowingDensity = 0.0f;
	const char* debug_text = nullptr;
	std::unordered_set<std::uint64_t> whitelist;
	std::unordered_set<std::uint64_t> blacklist;

	float GetSeasonalAltitude()
	{
		float maxMonth = static_cast<float>(std::max(MaxSummerMonth, MaxWinterMonth));
		float minMonth = static_cast<float>(std::min(MaxSummerMonth, MaxWinterMonth));
		// Equal months make the seasonal cycle degenerate (division by zero below); use a constant midpoint snow line.
		if (maxMonth == minMonth)
			return -0.5f * (SummerHeightOffset + WinterHeightOffset);
		float summerToWinter;
		auto month = (maxMonth + minMonth) / 2.0f;  // fallback value if calendar not exist
		if (auto calendar = RE::Calendar::GetSingleton()) {
			auto time = calendar->GetTime();
			month = static_cast<float>(time.tm_mon + (time.tm_mday + (time.tm_hour + (time.tm_min + time.tm_sec / 60.0) / 60.0) / 24.0) / 32.0);
		}
		if (month > maxMonth) {
			summerToWinter = (month - maxMonth) / (minMonth + 12.0f - maxMonth);
			if (MaxWinterMonth > MaxSummerMonth)
				summerToWinter = 1.0f - summerToWinter;
		} else if (month < minMonth) {
			summerToWinter = (12.0f - maxMonth + month) / (minMonth + 12.0f - maxMonth);
			if (MaxSummerMonth > MaxWinterMonth)
				summerToWinter = 1.0f - summerToWinter;
		} else {
			summerToWinter = (month - minMonth) / (maxMonth - minMonth);
			if (MaxSummerMonth > MaxWinterMonth)
				summerToWinter = 1.0f - summerToWinter;
		}

		return -std::lerp(SummerHeightOffset, WinterHeightOffset, summerToWinter);
	}

	virtual void SetupResources();
	virtual void Reset();
	virtual void Prepass() override;

	virtual void DrawSettings();

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;
	void Reload();
	void SaveConfig();

	virtual inline void PostPostLoad() override { Hooks::Install(); }

	void BSLightingShader_Setup(RE::BSRenderPass* Pass);
	static bool IsHarvestableFlora(RE::TESObjectREFR* a_ref);

	struct Hooks
	{
		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSEffectShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
			stl::write_vfunc<0x6, BSEffectShader_SetupGeometry>(RE::VTABLE_BSEffectShader[0]);
			logger::info("[SnowCover] Installed hooks");
		}
	};
};
