#include "LocalLightShadowCache.h"

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <RE/B/BSShadowLight.h>
#include <RE/B/BSShadowParabolicLight.h>
#include <RE/B/BSShadowFrustumLight.h>
#include <RE/B/BSShadowDirectionalLight.h>
#include <RE/N/NiAVObject.h>
#include <RE/R/Renderer.h>

namespace LocalLightShadowCache
{
	namespace
	{
		enum class LightKind
		{
			Spot,
			Point,
			Directional
		};

		bool   g_enabled = false;
		bool   g_skipLocal = false;  // ceiling-measurement: skip local renders entirely
		double g_qpcToMs = 0.0;

		// Per-frame accumulators (reset in BeginShadowFrame).
		struct FrameStats
		{
			uint32_t localMaps = 0;      // spot + point Render calls (== lights)
			uint32_t localCasters = 0;   // sum of sceneAccumArray sizes
			uint32_t dirMaps = 0;        // directional (sun) for reference
			int64_t  localTicks = 0;     // QPC ticks spent in spot+point Render
			int64_t  dirTicks = 0;       // QPC ticks in directional Render
			uint32_t cacheHits = 0;      // local lights unchanged vs last frame
			uint32_t cacheHitCasters = 0;// casters we could have skipped
		};
		FrameStats g_frame;

		// Rolling window of per-frame local-light-shadow CPU time (ms).
		constexpr size_t kWindow = 120;
		double           g_localMs[kWindow] = {};
		double           g_hitRate[kWindow] = {};
		uint32_t         g_mapsHist[kWindow] = {};
		size_t           g_windowIdx = 0;
		uint32_t         g_frameNo = 0;

		// Dirty tracking: light -> {content hash, last frame seen}.
		struct LightState
		{
			uint64_t hash = 0;
			uint32_t lastFrame = 0;
		};
		std::unordered_map<RE::BSShadowLight*, LightState> g_lightState;

		inline int64_t Now()
		{
			LARGE_INTEGER li;
			QueryPerformanceCounter(&li);
			return li.QuadPart;
		}

		inline uint64_t FNV(uint64_t h, uint32_t v)
		{
			h ^= v;
			h *= 0x100000001B3ull;
			return h;
		}
		inline uint32_t Bits(float f)
		{
			uint32_t u;
			std::memcpy(&u, &f, 4);
			return u;
		}

		// Content hash of a light's shadow map: light transform + every caster's
		// identity and world position. Identical hash two frames running == the
		// shadow map is bit-identical == the render was pure waste (cacheable).
		uint64_t ComputeHash(RE::BSShadowLight* light, uint32_t& outCasters)
		{
			uint64_t h = 0xCBF29CE484222325ull;
			const auto& p = light->worldTranslate;
			h = FNV(h, Bits(p.x));
			h = FNV(h, Bits(p.y));
			h = FNV(h, Bits(p.z));

			auto&          rd = light->GetRuntimeData();
			const auto&    casters = rd.sceneAccumArray;
			const uint32_t n = static_cast<uint32_t>(casters.size());
			outCasters = n;
			h = FNV(h, n);
			for (uint32_t i = 0; i < n; ++i) {
				RE::NiAVObject* obj = casters[i].get();
				if (!obj) {
					h = FNV(h, 0xDEAD);
					continue;
				}
				const auto ptr = reinterpret_cast<uintptr_t>(obj);
				h = FNV(h, static_cast<uint32_t>(ptr));
				h = FNV(h, static_cast<uint32_t>(ptr >> 32));
				const auto& t = obj->world.translate;
				h = FNV(h, Bits(t.x));
				h = FNV(h, Bits(t.y));
				h = FNV(h, Bits(t.z));
			}
			return h;
		}

		void OnRender(RE::BSShadowLight* light, LightKind kind, int64_t ticks, uint64_t hash, uint32_t casters)
		{
			if (kind == LightKind::Directional) {
				g_frame.dirMaps++;
				g_frame.dirTicks += ticks;
				return;
			}

			g_frame.localMaps++;
			g_frame.localCasters += casters;
			g_frame.localTicks += ticks;

			auto& st = g_lightState[light];
			const bool unchanged = (st.lastFrame == g_frameNo - 1) && (st.hash == hash);
			if (unchanged) {
				g_frame.cacheHits++;
				g_frame.cacheHitCasters += casters;
			}
			st.hash = hash;
			st.lastFrame = g_frameNo;
		}

		void PruneStale()
		{
			// Drop lights not seen for ~2s so the map does not grow unbounded.
			if ((g_frameNo & 127) != 0)
				return;
			for (auto it = g_lightState.begin(); it != g_lightState.end();) {
				if (g_frameNo - it->second.lastFrame > 256)
					it = g_lightState.erase(it);
				else
					++it;
			}
		}

		struct FrustumRender
		{
			static void thunk(RE::BSShadowLight* light, void* a2)
			{
				if (g_skipLocal)
					return;  // ceiling test: skip the render entirely
				uint32_t       casters = 0;
				const uint64_t hash = ComputeHash(light, casters);  // capture pre-render caster set
				const int64_t  t0 = Now();
				func(light, a2);
				OnRender(light, LightKind::Spot, Now() - t0, hash, casters);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct ParabolicRender
		{
			static void thunk(RE::BSShadowLight* light, void* a2)
			{
				if (g_skipLocal)
					return;
				uint32_t       casters = 0;
				const uint64_t hash = ComputeHash(light, casters);
				const int64_t  t0 = Now();
				func(light, a2);
				OnRender(light, LightKind::Point, Now() - t0, hash, casters);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		// Accumulate (vtable 0x9) builds the per-light caster pass list. When skipping
		// the render for the ceiling test we also skip Accumulate so no passes are
		// allocated-but-unwalked (that orphans nodes in the shared pool and corrupts
		// it within ~1 frame -- the documented skip-Func42 wall).
		struct FrustumAccumulate
		{
			static void thunk(RE::BSShadowLight* light, std::uint32_t& a_count, std::uint32_t a_channel,
				RE::NiAVObject* a_scene, std::uint8_t a_vr)
			{
				if (g_skipLocal)
					return;
				func(light, a_count, a_channel, a_scene, a_vr);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct ParabolicAccumulate
		{
			static void thunk(RE::BSShadowLight* light, std::uint32_t& a_count, std::uint32_t a_channel,
				RE::NiAVObject* a_scene, std::uint8_t a_vr)
			{
				if (g_skipLocal)
					return;
				func(light, a_count, a_channel, a_scene, a_vr);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct DirectionalRender
		{
			static void thunk(RE::BSShadowLight* light, void* a2)
			{
				const int64_t t0 = Now();
				func(light, a2);
				OnRender(light, LightKind::Directional, Now() - t0, 0, 0);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		double Median(double* arr, size_t n)
		{
			std::vector<double> v(arr, arr + n);
			std::sort(v.begin(), v.end());
			return v[n / 2];
		}
	}

	bool Enabled() { return g_enabled; }
	void SetSkipLocal(bool a_skip) { g_skipLocal = a_skip; }
	bool GetSkipLocal() { return g_skipLocal; }

	void Install()
	{
		char b[8] = {};
		if (!(GetEnvironmentVariableA("CS_LIGHTSHADOW_PROF", b, sizeof(b)) && b[0] && b[0] != '0'))
			return;
		g_enabled = true;

		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		g_qpcToMs = 1000.0 / static_cast<double>(freq.QuadPart);

		stl::write_vfunc<0xA, DirectionalRender>(RE::VTABLE_BSShadowDirectionalLight[0]);
		stl::write_vfunc<0xA, FrustumRender>(RE::VTABLE_BSShadowFrustumLight[0]);
		stl::write_vfunc<0xA, ParabolicRender>(RE::VTABLE_BSShadowParabolicLight[0]);
		stl::write_vfunc<0x9, FrustumAccumulate>(RE::VTABLE_BSShadowFrustumLight[0]);
		stl::write_vfunc<0x9, ParabolicAccumulate>(RE::VTABLE_BSShadowParabolicLight[0]);

		logger::info("[LocalLightShadowCache] CS_LIGHTSHADOW_PROF=1: profiling local-light shadow renders");
	}

	void BeginShadowFrame()
	{
		if (!g_enabled)
			return;
		g_frame = FrameStats{};
		g_frameNo++;
	}

	void EndShadowFrame()
	{
		if (!g_enabled)
			return;

		const double localMs = g_frame.localTicks * g_qpcToMs;
		const double dirMs = g_frame.dirTicks * g_qpcToMs;
		const double hitRate = g_frame.localMaps ? (100.0 * g_frame.cacheHits / g_frame.localMaps) : 0.0;

		g_localMs[g_windowIdx] = localMs;
		g_hitRate[g_windowIdx] = hitRate;
		g_mapsHist[g_windowIdx] = g_frame.localMaps;
		g_windowIdx = (g_windowIdx + 1) % kWindow;

		PruneStale();

		if (g_frameNo % kWindow == 0) {
			const double medLocalMs = Median(g_localMs, kWindow);
			const double medHit = Median(g_hitRate, kWindow);
			uint32_t maxMaps = 0;
			for (auto m : g_mapsHist)
				maxMaps = std::max(maxMaps, m);
			logger::info(
				"[LocalLightShadowCache] local shadow maps: {} this frame (max {} in window) | "
				"casters {} (cacheable {}) | CPU {:.3f}ms/frame (median {:.3f}) | dir(sun) {:.3f}ms | "
				"cache-hit {:.1f}% (median {:.1f}%)",
				g_frame.localMaps, maxMaps, g_frame.localCasters, g_frame.cacheHitCasters,
				localMs, medLocalMs, dirMs, hitRate, medHit);
		}
	}
}
