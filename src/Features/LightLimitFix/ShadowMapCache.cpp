#include "ShadowMapCache.h"

#include "Globals.h"
#include "Util.h"
#include "Utils/D3D.h"

#include <RE/N/NiAVObject.h>

#include <vector>

namespace ShadowMapCache
{
	namespace
	{
		constexpr std::uint32_t kLayers = 64;      // >= max concurrent shadow-map slices
		constexpr std::uint32_t kEvictAfter = 240; // drop a camera not seen for ~4s (light despawned/moved)

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

		// Per-slice D16 depth-stencil views onto the shadow atlas (target 4), created lazily. Used to re-attach a
		// slice as the depth target after a Blit/Capture detaches it for the copy, so the engine's caster draws
		// (and our static replay) land back in the slice.
		winrt::com_ptr<ID3D11DepthStencilView> g_atlasSliceDSV[kLayers];

		// Per-CAMERA cache state. Each shadow map (one per spot light, one per point-light hemisphere) has its own
		// camera pointer, its own cache-slice layer, and its own change signature -- so a point light's two halves
		// never invalidate each other. posKey (the camera world-position hash) detects a MOVED light (its shadow
		// view changed); sig detects a changed static caster set. UINT32_MAX layer = no slice captured yet.
		struct CamState
		{
			std::uint32_t layer = UINT32_MAX;
			std::uint64_t posKey = 0;
			std::uint32_t lastFrame = 0;
			bool          hasState = false;
			bool          pendingCapture = false;  // base needs (re)building -> capture static-only next frame
		};
		std::unordered_map<void*, CamState> g_camState;
		bool                                g_layerValid[kLayers] = {};
		std::uint32_t                       g_nextLayer = 0;
		std::vector<std::uint32_t>          g_freeLayers;  // reclaimed from evicted cameras
		std::uint32_t                       g_frame = 0;

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

		// The camera world-position hash -- a moved light gets a new posKey and re-renders.
		std::uint64_t PosKey(void* a_camera)
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
			auto& st = g_camState[a_camera];
			if (st.layer != UINT32_MAX)
				return st.layer;
			std::uint32_t l;
			if (!g_freeLayers.empty()) {
				l = g_freeLayers.back();
				g_freeLayers.pop_back();
			} else if (g_nextLayer < kLayers) {
				l = g_nextLayer++;
			} else {
				return UINT32_MAX;
			}
			st.layer = l;
			g_layerValid[l] = false;
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

		// A per-slice D16 DSV onto the shadow atlas (target 4), created lazily.
		ID3D11DepthStencilView* AtlasSliceDSV(std::uint32_t a_slice)
		{
			if (a_slice >= kLayers)
				return nullptr;
			if (g_atlasSliceDSV[a_slice])
				return g_atlasSliceDSV[a_slice].get();
			auto* atlas = AtlasResource();
			if (!atlas)
				return nullptr;
			winrt::com_ptr<ID3D11Texture2D> tex;
			if (FAILED(atlas->QueryInterface(IID_PPV_ARGS(tex.put()))))
				return nullptr;
			D3D11_DEPTH_STENCIL_VIEW_DESC dv{};
			dv.Format = DXGI_FORMAT_D16_UNORM;
			dv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
			dv.Texture2DArray.FirstArraySlice = a_slice;
			dv.Texture2DArray.ArraySize = 1;
			if (FAILED(globals::d3d::device->CreateDepthStencilView(tex.get(), &dv, g_atlasSliceDSV[a_slice].put())))
				return nullptr;
			return g_atlasSliceDSV[a_slice].get();
		}
	}

	void BeginFrame()
	{
		++g_frame;
		// Publish the completed frame's aggregates for the UI, then reset the per-frame counters (the lastRegen*
		// fields stay sticky so the UI can show "N frames ago").
		g_stats.frame = g_frame;
		g_stats.unitsTotal = static_cast<std::uint32_t>(g_camState.size());
		std::uint32_t valid = 0;
		for (bool v : g_layerValid)
			if (v)
				++valid;
		g_stats.layersValid = valid;
		g_stats.freshThisFrame = 0;
		g_stats.blitsThisFrame = 0;
		g_stats.regenThisFrame = 0;

		// Evict cameras not seen recently (light despawned / cell change) and reclaim their cache layers.
		if ((g_frame & 63) == 0) {
			for (auto it = g_camState.begin(); it != g_camState.end();) {
				if (g_frame - it->second.lastFrame > kEvictAfter) {
					if (it->second.layer != UINT32_MAX) {
						g_layerValid[it->second.layer] = false;
						g_freeLayers.push_back(it->second.layer);
					}
					it = g_camState.erase(it);
				} else {
					++it;
				}
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

	Action PreWalkDecide(void* a_camera)
	{
		auto&               st = g_camState[a_camera];
		st.lastFrame = g_frame;
		const std::uint64_t posKey = PosKey(a_camera);
		const bool          haveCache = st.layer != UINT32_MAX && g_layerValid[st.layer];
		const bool          moved = st.hasState && st.posKey != posKey;  // light moved -> its shadow view changed
		st.posKey = posKey;
		st.hasState = true;

		if (moved) {
			// A moving light cannot cache (the whole view changes): render full every frame, invalidate the base.
			if (st.layer != UINT32_MAX)
				g_layerValid[st.layer] = false;
			st.pendingCapture = false;
			g_stats.lastRegenFrame = g_frame;
			g_stats.lastReason = Reason::MovedLight;
			g_stats.lastReasonKey = posKey;
			return Action::RenderFull;
		}
		if (st.pendingCapture) {
			++g_stats.freshThisFrame;
			return Action::StaticCapture;  // (re)build the base this frame
		}
		if (haveCache) {
			++g_stats.blitsThisFrame;
			return Action::CopyBase;  // the win: copy the cached static, draw dynamics on top
		}
		// No cache yet -> render full this frame + schedule a capture for next frame.
		st.pendingCapture = true;
		return Action::RenderFull;
	}

	void NoteCaptured(void* a_camera)
	{
		auto& st = g_camState[a_camera];
		st.pendingCapture = false;  // the layer was just filled (Capture sets g_layerValid)
	}

	void NoteChanged(void* a_camera)
	{
		auto& st = g_camState[a_camera];
		if (!st.pendingCapture) {
			st.pendingCapture = true;  // rebuild the base next frame
			++g_stats.regenThisFrame;
			g_stats.lastRegenFrame = g_frame;
			g_stats.lastReason = Reason::StaticSetChanged;
			g_stats.lastReasonKey = st.posKey;
		}
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
		auto it = g_camState.find(a_camera);
		if (it == g_camState.end() || it->second.layer == UINT32_MAX || !g_layerValid[it->second.layer])
			return false;
		const UINT atlasSub = D3D11CalcSubresource(0, a_slice, g_cacheDesc.MipLevels);
		const UINT cacheSub = D3D11CalcSubresource(0, it->second.layer, g_cacheDesc.MipLevels);
		CopyPort(a_ctx, atlas, atlasSub, g_cacheTex.get(), cacheSub, a_port);
		++g_stats.blitsThisFrame;
		return true;
	}

	void BindSlice(ID3D11DeviceContext* a_ctx, std::uint32_t a_slice)
	{
		if (!a_ctx)
			return;
		auto* dsv = AtlasSliceDSV(a_slice);
		if (dsv)
			a_ctx->OMSetRenderTargets(0, nullptr, dsv);  // depth-only: no color RTV on a shadow map
	}

	const Stats& GetStats() { return g_stats; }

	const char* ReasonString(Reason a_reason)
	{
		switch (a_reason) {
		case Reason::FirstBuild:       return "new light";
		case Reason::MovedLight:       return "light moved";
		case Reason::StaticSetChanged: return "static object changed";
		case Reason::NoCache:          return "first capture";
		default:                       return "none";
		}
	}
}
