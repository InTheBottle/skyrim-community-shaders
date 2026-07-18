#include "ShadowMapCache.h"

#include "Globals.h"
#include "Util.h"
#include "Utils/D3D.h"

#include <RE/N/NiAVObject.h>

namespace ShadowMapCache
{
	namespace
	{
		constexpr std::uint32_t kLayers = 64;      // >= max concurrent shadow-map slices
		constexpr std::uint32_t kEvictAfter = 240; // drop a unit not seen for ~4s (light despawned/moved)

		// Shadow depth atlas = depth-stencil target 4. The SRV lives on the renderer object at
		// stride 152/target, base 0x2040 (same access the shadow-instancing detour uses).
		REL::Relocation<std::uint8_t*> g_renderer{ REL::Offset(0x3028490) };

		ID3D11ShaderResourceView* AtlasSRV()
		{
			auto* rtPool = reinterpret_cast<std::uint8_t*>(g_renderer.address());
			return *reinterpret_cast<ID3D11ShaderResourceView**>(rtPool + 152 * 4 + 0x2040);
		}
		ID3D11Resource* AtlasResource()
		{
			auto* srv = AtlasSRV();
			if (!srv)
				return nullptr;
			static winrt::com_ptr<ID3D11Resource> s_res;  // cached; the atlas resource is fixed for the process
			if (!s_res)
				srv->GetResource(s_res.put());
			return s_res.get();
		}

		winrt::com_ptr<ID3D11Texture2D> g_cacheTex;
		D3D11_TEXTURE2D_DESC            g_cacheDesc{};

		// Per-CAMERA private cache layer (each shadow map owns a slice-sized layer).
		std::unordered_map<void*, std::uint32_t> g_layerOf;
		std::uint32_t                            g_nextLayer = 0;
		bool                                     g_layerValid[kLayers] = {};

		// UNITS. A unit groups maps that update together (a point light's two halves share a unit; each spot is
		// its own). Keyed on the shadow camera world position so a moved light becomes a new unit. `sig` is the
		// last static-caster-set signature this unit rendered -- the unit renders fresh only when it differs.
		struct Unit
		{
			std::uint64_t sig = 0;
			std::uint32_t lastFrame = 0;
			bool          hasSig = false;
		};
		std::unordered_map<std::uint64_t, Unit> g_unitOf;
		std::uint32_t                           g_frame = 0;

		Stats g_stats{};

		inline std::uint64_t Fnv(std::uint64_t h, std::uint32_t v)
		{
			h ^= v;
			h *= 0x100000001b3ull;
			return h;
		}
		inline std::uint32_t Bits(float f)
		{
			std::uint32_t u;
			std::memcpy(&u, &f, 4);
			return u;
		}

		// A local light's unit key is the shadow camera's WORLD POSITION so a point light's two hemisphere
		// cameras (same position) share a unit and update together, and a moved light gets a new key.
		std::uint64_t UnitKey(void* a_camera, std::uint32_t)
		{
			std::uint64_t h = 0xcbf29ce484222325ull;
			if (a_camera) {
				const RE::NiPoint3 t = reinterpret_cast<RE::NiAVObject*>(a_camera)->world.translate;
				h = Fnv(h, Bits(t.x));
				h = Fnv(h, Bits(t.y));
				h = Fnv(h, Bits(t.z));
			}
			return h;
		}

		std::uint32_t LayerFor(void* a_camera)
		{
			auto it = g_layerOf.find(a_camera);
			if (it != g_layerOf.end())
				return it->second;
			if (g_nextLayer >= kLayers)
				return UINT32_MAX;
			const std::uint32_t l = g_nextLayer++;
			g_layerOf.emplace(a_camera, l);
			return l;
		}

		// Copy one map's port sub-rect, split into vertical halves so no single CopySubresourceRegion
		// spans a full D16 subresource (proven DXVK quirk: full-subresource copies of the shadow
		// Texture2DArray corrupt; sub-region copies are byte-exact).
		void CopyPort(ID3D11DeviceContext* a_ctx, ID3D11Resource* a_dst, std::uint32_t a_dstSub,
			ID3D11Resource* a_src, std::uint32_t a_srcSub, const std::int32_t* a_port)
		{
			const std::int32_t pl = a_port[0], pr = a_port[1], pt = a_port[2], pb = a_port[3];
			const UINT         bl = static_cast<UINT>(std::min(pl, pr)), br = static_cast<UINT>(std::max(pl, pr));
			const UINT         bt = static_cast<UINT>(std::min(pt, pb)), bb = static_cast<UINT>(std::max(pt, pb));
			if (br <= bl || bb <= bt) {  // degenerate port -> whole-subresource split in two
				a_ctx->CopySubresourceRegion(a_dst, a_dstSub, 0, 0, 0, a_src, a_srcSub, nullptr);
				return;
			}
			const UINT midY = bt + (bb - bt) / 2u;
			if (midY > bt) {
				const D3D11_BOX b{ bl, bt, 0, br, midY, 1 };
				a_ctx->CopySubresourceRegion(a_dst, a_dstSub, bl, bt, 0, a_src, a_srcSub, &b);
			}
			if (bb > midY) {
				const D3D11_BOX b{ bl, midY, 0, br, bb, 1 };
				a_ctx->CopySubresourceRegion(a_dst, a_dstSub, bl, midY, 0, a_src, a_srcSub, &b);
			}
		}
	}

	void BeginFrame()
	{
		++g_frame;
		// Publish the completed frame's aggregates for the UI, then reset the per-frame counters (the lastRegen*
		// fields stay sticky so the UI can show "N frames ago").
		g_stats.frame = g_frame;
		g_stats.unitsTotal = static_cast<std::uint32_t>(g_unitOf.size());
		std::uint32_t valid = 0;
		for (bool v : g_layerValid)
			if (v)
				++valid;
		g_stats.layersValid = valid;
		g_stats.freshThisFrame = 0;
		g_stats.blitsThisFrame = 0;
		g_stats.regenThisFrame = 0;

		// Evict units not seen recently so despawned/moved lights don't hold cache slots.
		if ((g_frame & 63) == 0) {
			for (auto it = g_unitOf.begin(); it != g_unitOf.end();) {
				if (g_frame - it->second.lastFrame > kEvictAfter)
					it = g_unitOf.erase(it);
				else
					++it;
			}
		}
	}

	bool EnsureCache()
	{
		if (g_cacheTex)
			return true;
		if (!globals::d3d::device)
			return false;
		auto* atlasRes = AtlasResource();
		winrt::com_ptr<ID3D11Texture2D> atlas;
		if (!atlasRes || FAILED(atlasRes->QueryInterface(IID_PPV_ARGS(atlas.put()))))
			return false;
		D3D11_TEXTURE2D_DESC d{};
		atlas->GetDesc(&d);
		g_cacheDesc = d;
		D3D11_TEXTURE2D_DESC cd = d;  // mirror format/dims -> copy-compatible
		cd.ArraySize = kLayers;
		cd.Usage = D3D11_USAGE_DEFAULT;
		cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		cd.CPUAccessFlags = 0;
		cd.MiscFlags = 0;
		if (FAILED(globals::d3d::device->CreateTexture2D(&cd, nullptr, g_cacheTex.put()))) {
			logger::error("[ShadowMapCache] CreateTexture2D failed (fmt={} {}x{})", static_cast<std::uint32_t>(d.Format), d.Width, d.Height);
			return false;
		}
		Util::SetResourceName(g_cacheTex.get(), "ShadowMapCache::Slices");
		logger::info("[ShadowMapCache] slice cache: atlas fmt={} {}x{} -> {} layers", static_cast<std::uint32_t>(d.Format), d.Width, d.Height, kLayers);
		return true;
	}

	bool ShouldRender(void* a_camera, std::uint32_t a_rmode, const std::uint8_t*, std::uint64_t a_staticSig)
	{
		const std::uint64_t key = UnitKey(a_camera, a_rmode);
		auto [it, inserted] = g_unitOf.try_emplace(key, Unit{});
		it->second.lastFrame = g_frame;

		auto renderFresh = [&](Reason a_reason) {
			it->second.sig = a_staticSig;
			it->second.hasSig = true;
			++g_stats.freshThisFrame;
			if (a_reason == Reason::StaticSetChanged)
				++g_stats.regenThisFrame;
			if (a_reason != Reason::NoCache) {  // NoCache is a first capture, not a scene-change event
				g_stats.lastRegenFrame = g_frame;
				g_stats.lastReason = a_reason;
				g_stats.lastReasonKey = key;
			}
			return true;
		};

		if (inserted || !it->second.hasSig)
			return renderFresh(Reason::FirstBuild);  // a newly seen / moved light -> no cache yet

		if (it->second.sig != a_staticSig)
			return renderFresh(Reason::StaticSetChanged);  // a static was placed / disabled / moved

		// Static set unchanged: reuse the cached slice if this exact map has a valid layer, else capture once.
		auto lit = g_layerOf.find(a_camera);
		if (lit == g_layerOf.end() || !g_layerValid[lit->second])
			return renderFresh(Reason::NoCache);
		return false;  // pure blit
	}

	void Capture(ID3D11DeviceContext* a_ctx, void* a_camera, std::uint32_t a_slice, const std::int32_t* a_port)
	{
		auto* atlas = AtlasResource();
		if (!g_cacheTex || !a_ctx || !atlas)
			return;
		const std::uint32_t layer = LayerFor(a_camera);
		if (layer == UINT32_MAX)
			return;
		const UINT atlasSub = D3D11CalcSubresource(0, a_slice, g_cacheDesc.MipLevels);
		const UINT cacheSub = D3D11CalcSubresource(0, layer, g_cacheDesc.MipLevels);
		CopyPort(a_ctx, g_cacheTex.get(), cacheSub, atlas, atlasSub, a_port);
		g_layerValid[layer] = true;
	}

	bool Blit(ID3D11DeviceContext* a_ctx, void* a_camera, std::uint32_t a_slice, const std::int32_t* a_port)
	{
		auto* atlas = AtlasResource();
		if (!g_cacheTex || !a_ctx || !atlas)
			return false;
		auto it = g_layerOf.find(a_camera);
		if (it == g_layerOf.end() || !g_layerValid[it->second])
			return false;
		const UINT atlasSub = D3D11CalcSubresource(0, a_slice, g_cacheDesc.MipLevels);
		const UINT cacheSub = D3D11CalcSubresource(0, it->second, g_cacheDesc.MipLevels);
		CopyPort(a_ctx, atlas, atlasSub, g_cacheTex.get(), cacheSub, a_port);
		++g_stats.blitsThisFrame;
		return true;
	}

	const Stats& GetStats() { return g_stats; }

	const char* ReasonString(Reason a_reason)
	{
		switch (a_reason) {
		case Reason::FirstBuild:       return "new light";
		case Reason::StaticSetChanged: return "static object changed";
		case Reason::NoCache:          return "first capture";
		default:                       return "none";
		}
	}
}
