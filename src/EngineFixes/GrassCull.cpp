#include "GrassCull.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <d3d11.h>

#include "Buffer.h"
#include "Features/OcclusionCulling/MOC.h"
#include "Globals.h"
#include "State.h"
#include "Util.h"

// GPU-driven grass frustum culling. All grass batches (BSMultiStreamInstanceTriShape) are concatenated into
// one instance buffer and culled in a SINGLE 2D compute dispatch (per-batch dispatches split the DXVK render
// pass ~360x/frame = 2ms). Survivors are compacted per-batch and drawn via DrawIndexedInstancedIndirect over
// the shared buffer with a per-batch vertex-buffer offset. Byte-exact for survivors (identical FP16 layout ->
// unchanged input layout / grass VS+PS). SE-only (every offset is 1.5.97).
namespace
{
	namespace engine
	{
		inline REL::Relocation<ID3D11DeviceContext**> g_context{ REL::Offset(0x3027EA0) };
		inline REL::Relocation<std::uint8_t*>         S_base{ REL::Offset(0x3027EB0) };
		inline REL::Relocation<void(std::uint32_t)>   SetDirtyStates{ REL::Offset(0xD705B0) };
		constexpr std::size_t                         kVertexDescOff = 0x340;
		constexpr std::size_t                         kTopologyOff = 0x358;
	}

	constexpr std::uint32_t kMinInstances = 256;
	constexpr float         kClumpRadius = 256.0f;
	// Grass blade height (world units). Hi-Z tests the clump's TIP cell (base + up*height): the blades poke
	// upward into the least-occluded part of the screen, so the tip cell holds the FARTHEST background over the
	// whole clump -- culling only when the clump is behind even that is hole-free (behind its entire footprint).
	constexpr float         kGrassHeight = 100.0f;
	// Cull clumps whose view depth (clip.w) exceeds this -- past the grass fade-out they are fully alpha-faded
	// (invisible) yet still drawn, so removing them is free visually. Generous so only invisible grass is cut.
	constexpr float         kDistanceCullEnd = 8192.0f;
	// Stochastic distance thinning (a distinct LOD technique, NOT occlusion): from kThinStart to kThinEnd,
	// drop up to kThinMax of clumps. OFF for now (kThinMax=0) -- to be exposed as an OcclusionCulling option.
	constexpr float         kThinStart = 3000.0f;
	constexpr float         kThinEnd = 8000.0f;
	constexpr float         kThinMax = 0.0f;
	// Capacity must exceed worst-case loaded density -- dense-grass exteriors reach ~1000 batches / ~725k
	// instances, and hitting either cap forces ResetSlots() to churn every frame (batches never stay captured,
	// so they flip between culled and full = flickering/disappearing grass). Sized ~3-4x the observed worst
	// case with headroom (concat+compacted = 2 * 2M * 32B = 128 MB VRAM, trivial on modern cards).
	constexpr std::uint32_t kMaxBatches = 4096;
	constexpr std::uint32_t kMaxInstances = 2u * 1024u * 1024u;  // concat/compacted capacity (2M)
	// Per-frame budgets for the two render-pass-splitting copies, tracked SEPARATELY so a cell-load burst of new
	// registrations can never starve the (cheap, 64B) World captures -- starved captures leave batches drawing
	// vanilla forever while the cull counter reports garbage. Each bounds its own DXVK render-pass splits.
	constexpr std::uint32_t kMaxRegistersPerFrame = 24;  // concat instance-stream copies (a few MB each)
	constexpr std::uint32_t kMaxCapturesPerFrame = 32;   // b2 World captures (64 B each)

	// Hi-Z occlusion margin (WORLD units). A clump is culled only when it is at least this far BEHIND the
	// tip cell's farthest visible surface -- i.e. behind a solid occluder (tree/hill), never merely behind
	// neighbouring grass at a similar depth. Comparing in view space (not saturated NDC) with a physical
	// margin makes the cull decision stable frame-to-frame (no thrashing stripes). Only the forward burst.
	constexpr float kHiZMargin = 700.0f;

	struct CullCB
	{
		float         camVP[16];      // this frame's main view-proj (frustum / distance / thinning)
		float         prevCamVP[16];  // last frame's main view-proj (Hi-Z reproject into the previous-frame grid)
		float         radiusWorld;
		float         distanceEnd;
		float         thinStart;
		float         thinEnd;
		std::uint32_t batchCount;
		float         thinMax;
		std::uint32_t hizValid;  // 1 = sample this-frame's Hi-Z grid at t3 (forward burst only)
		std::uint32_t hizGridW;
		std::uint32_t hizGridH;
		std::uint32_t hizFullW;
		std::uint32_t hizFullH;
		float         hizMargin;  // world-units a clump must be BEHIND the tip cell's occluder to cull
		float         hizNear;    // camera near/far the grid depth used -> reconstruct grid NDC-z from clip.w
		float         hizFar;
		float         hizHeight;  // blade height: sample the grid at the clump's TIP cell (base + up*height)
		float         pad0[1];
	};
	struct ArgsCB
	{
		std::uint32_t batchCount;
		std::uint32_t pad[3];
	};
	struct BatchDesc  // matches HLSL
	{
		std::uint32_t srcOffset;
		std::uint32_t count;
		std::uint32_t dstOffset;
		std::uint32_t triCount;
	};

	bool CullEnabled()
	{
		static std::atomic<bool>          s_on{ false };
		static std::atomic<std::uint32_t> s_ctr{ 0 };
		if ((s_ctr.fetch_add(1, std::memory_order_relaxed) % 120u) == 0) {
			std::error_code ec;
			s_on.store(std::filesystem::exists(L"F:\\claudetmp\\grass_cull.flag", ec), std::memory_order_relaxed);
		}
		return s_on.load(std::memory_order_relaxed);
	}

	// Frame-lagged Hi-Z occlusion is opt-in (separate flag, default OFF): the previous-frame reprojection
	// makes unstable per-frame decisions (flicker) and currently over-culls. The frustum/distance/thinning
	// cull is the shipped default. Kept for iteration on a stable Hi-Z.
	bool HiZCullEnabled()
	{
		static std::atomic<bool>          s_on{ false };
		static std::atomic<std::uint32_t> s_ctr{ 0 };
		if ((s_ctr.fetch_add(1, std::memory_order_relaxed) % 120u) == 0) {
			std::error_code ec;
			s_on.store(std::filesystem::exists(L"F:\\claudetmp\\grass_hiz.flag", ec), std::memory_order_relaxed);
		}
		return s_on.load(std::memory_order_relaxed);
	}

	winrt::com_ptr<ID3D11ComputeShader> g_cullCS, g_argsCS;
	std::unique_ptr<ConstantBuffer>     g_cullCB, g_argsCB;
	// Shared inputs (both bursts read these).
	winrt::com_ptr<ID3D11Buffer>             g_concat, g_desc, g_world;
	winrt::com_ptr<ID3D11ShaderResourceView> g_concatSRV, g_descSRV, g_worldSRV;
	// Per-pass output set (survivors + counters + indirect args). Set A = z-prepass (frustum/distance/thin).
	// Set B = forward = A minus Hi-Z-occluded, sampled against THIS frame's grid. B is a subset of A, so
	// occluded grass is skipped only in the expensive forward shading, never in the z-prepass -> hole-proof.
	struct OutSet
	{
		winrt::com_ptr<ID3D11Buffer>              compacted, counters, args;
		winrt::com_ptr<ID3D11ShaderResourceView>  countersSRV;
		winrt::com_ptr<ID3D11UnorderedAccessView> compactedUAV, countersUAV, argsUAV;
	};
	OutSet g_setA;  // single per-frame culled set; both the z-prepass and forward pass draw it (hole-free)
	bool   g_initFailed = false;

	struct Batch
	{
		ID3D11Buffer* meshVB = nullptr;
		ID3D11Buffer* indexBuf = nullptr;
		std::uint32_t count = 0;
		std::uint32_t triCount = 0;
		std::uint64_t vertexDesc = 0;
		std::uint32_t batchIdx = 0;
		std::uint32_t srcOffset = 0;
		std::uint64_t lastSeen = 0;
		bool          registered = false;  // slot assigned + instances copied into concat
		bool          worldCaptured = false;
		std::uint64_t captureFrame = ~0ull;  // frame World was captured; draw indirect only once < current
	};
	std::unordered_map<ID3D11Buffer*, Batch> g_batches;
	std::vector<BatchDesc>                   g_descCPU;  // mirror uploaded to g_desc each cull
	std::uint32_t                            g_batchCount = 0;
	std::uint32_t                            g_concatUsed = 0;
	std::uint32_t                            g_maxCount = 0;
	std::uint64_t                            g_burstFrameA = ~0ull;  // z-prepass cull (set A)
	std::uint32_t                            g_registersThisFrame = 0;  // concat copies this frame
	std::uint32_t                            g_capturesThisFrame = 0;   // World captures this frame (own budget)
	std::uint64_t                            g_registerFrame = ~0ull;
		// Last frame's main view-proj: the Hi-Z grid is the PREVIOUS frame's, so a static clump reprojects with
		// the matching (previous) camera to hit the right grid cell -- else a turning camera misplaces it (stripes).
		float                                    g_prevCamVP[16] = {};
		bool                                     g_prevCamVPValid = false;
		std::uint64_t                            g_prevCamVPFrame = ~0ull;  // frame g_prevCamVP was captured

	ID3D11Buffer* MakeBuf(UINT bytes, UINT bind, UINT misc, UINT stride, const char* name)
	{
		D3D11_BUFFER_DESC d{};
		d.ByteWidth = bytes;
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = bind;
		d.MiscFlags = misc;
		d.StructureByteStride = stride;
		ID3D11Buffer* b = nullptr;
		if (FAILED(globals::d3d::device->CreateBuffer(&d, nullptr, &b)))
			return nullptr;
		Util::SetResourceName(b, name);
		return b;
	}

	bool EnsureShared()
	{
		if (g_cullCS)
			return true;
		if (g_initFailed)
			return false;
		auto* device = globals::d3d::device;
		try {
			g_cullCS.attach(static_cast<ID3D11ComputeShader*>(
				Util::CompileShader(L"Data\\Shaders\\GrassCull\\GrassCullCS.hlsl", {}, "cs_5_0")));
			g_argsCS.attach(static_cast<ID3D11ComputeShader*>(
				Util::CompileShader(L"Data\\Shaders\\GrassCull\\GrassCullArgsCS.hlsl", {}, "cs_5_0")));
			if (!g_cullCS || !g_argsCS) {
				g_initFailed = true;
				return false;
			}
			g_cullCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CullCB>(), "GrassCull::CullCB");
			g_argsCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ArgsCB>(), "GrassCull::ArgsCB");

			const UINT SR = D3D11_BIND_SHADER_RESOURCE, UA = D3D11_BIND_UNORDERED_ACCESS;
			const UINT ST = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, RAW = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
			g_concat.attach(MakeBuf(kMaxInstances * 32u, SR, ST, 32u, "GrassCull::Concat"));
			g_desc.attach(MakeBuf(kMaxBatches * sizeof(BatchDesc), SR, ST, sizeof(BatchDesc), "GrassCull::Desc"));
			g_world.attach(MakeBuf(kMaxBatches * 64u, SR, ST, 64u, "GrassCull::World"));
			if (!g_concat || !g_desc || !g_world) {
				g_initFailed = true;
				return false;
			}

			auto srvStruct = [&](ID3D11Buffer* buf, UINT n, ID3D11ShaderResourceView** out) {
				D3D11_SHADER_RESOURCE_VIEW_DESC s{};
				s.Format = DXGI_FORMAT_UNKNOWN;
				s.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
				s.BufferEx.NumElements = n;
				return SUCCEEDED(device->CreateShaderResourceView(buf, &s, out));
			};
			srvStruct(g_concat.get(), kMaxInstances, g_concatSRV.put());
			srvStruct(g_desc.get(), kMaxBatches, g_descSRV.put());
			srvStruct(g_world.get(), kMaxBatches, g_worldSRV.put());

			// Build a per-pass output set: compacted survivors (also a VB) + per-batch counters + indirect args.
			auto makeSet = [&](OutSet& set, const char* tag) -> bool {
				const std::string cn = std::string("GrassCull::Compacted") + tag;
				const std::string kn = std::string("GrassCull::Counters") + tag;
				const std::string an = std::string("GrassCull::Args") + tag;
				set.compacted.attach(MakeBuf(kMaxInstances * 32u, SR | UA | D3D11_BIND_VERTEX_BUFFER, RAW, 0, cn.c_str()));
				set.counters.attach(MakeBuf(kMaxBatches * 4u, SR | UA, ST, 4u, kn.c_str()));
				set.args.attach(MakeBuf(kMaxBatches * 20u, UA | D3D11_BIND_SHADER_RESOURCE, RAW | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0, an.c_str()));
				if (!set.compacted || !set.counters || !set.args)
					return false;
				srvStruct(set.counters.get(), kMaxBatches, set.countersSRV.put());
				D3D11_UNORDERED_ACCESS_VIEW_DESC ur{};
				ur.Format = DXGI_FORMAT_R32_TYPELESS;
				ur.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
				ur.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
				ur.Buffer.NumElements = (kMaxInstances * 32u) / 4u;
				device->CreateUnorderedAccessView(set.compacted.get(), &ur, set.compactedUAV.put());
				ur.Buffer.NumElements = (kMaxBatches * 20u) / 4u;
				device->CreateUnorderedAccessView(set.args.get(), &ur, set.argsUAV.put());
				D3D11_UNORDERED_ACCESS_VIEW_DESC us{};
				us.Format = DXGI_FORMAT_UNKNOWN;
				us.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
				us.Buffer.NumElements = kMaxBatches;
				device->CreateUnorderedAccessView(set.counters.get(), &us, set.countersUAV.put());
				return set.compactedUAV && set.argsUAV && set.countersUAV && set.countersSRV;
			};
			if (!makeSet(g_setA, "A")) {
				g_initFailed = true;
				return false;
			}

			g_descCPU.resize(kMaxBatches);
			return true;
		} catch (const std::exception& e) {
			logger::error("[GrassCull] init failed: {}", e.what());
			g_initFailed = true;
			return false;
		}
	}

	void ResetSlots()
	{
		for (auto& [vb, b] : g_batches) {
			b.registered = false;
			b.worldCaptured = false;
		}
		g_batchCount = 0;
		g_concatUsed = 0;
		g_maxCount = 0;
	}

	// Cull all registered batches in one dispatch into `out`, then fill its indirect args. doHiZ adds the
	// occlusion test against THIS frame's grid -- valid only for the FORWARD burst, where the grid (built at
	// PrepassPasses from the z-prepass depth) already exists. The z-prepass burst (doHiZ=false) runs earlier,
	// before the grid, and produces the superset the z-prepass draws.
	void RunCull(std::uint64_t frame, OutSet& out, bool doHiZ)
	{
		auto* ctx = globals::d3d::context;

		// Upload the descriptor table.
		D3D11_BOX descBox{ 0, 0, 0, g_batchCount * (UINT)sizeof(BatchDesc), 1, 1 };
		if (g_batchCount)
			ctx->UpdateSubresource(g_desc.get(), 0, &descBox, g_descCPU.data(), 0, 0);

		globals::profiler->BeginPass(doHiZ ? "GrassCull::CullHiZ" : "GrassCull::Cull");
		auto   vp = globals::game::frameBufferCached.GetCameraViewProj();  // this frame's main view-proj (row-major)
		CullCB cb{};
		std::memcpy(cb.camVP, &vp, sizeof(cb.camVP));
		// Hi-Z reprojects into the PREVIOUS frame's grid, so use last frame's VP (first frame reuses this one).
		// Only trust it if it is EXACTLY last frame's -- a frame that drew no grass leaves it stale, which would
		// misplace clumps for one frame (transient wrong cull) on the frame grass reappears.
		const bool prevFresh = g_prevCamVPValid && g_prevCamVPFrame + 1 == frame;
		std::memcpy(cb.prevCamVP, prevFresh ? g_prevCamVP : cb.camVP, sizeof(cb.prevCamVP));
		cb.radiusWorld = kClumpRadius;
		cb.distanceEnd = kDistanceCullEnd;
		cb.thinStart = kThinStart;
		cb.thinEnd = kThinEnd;
		cb.batchCount = g_batchCount;
		cb.thinMax = kThinMax;
		cb.hizMargin = kHiZMargin;

		// Hi-Z runs in the single z-prepass burst against the PREVIOUS frame's grid (this frame's is not reduced
		// until PrepassPasses, after the z-prepass), so both passes cull the same set = hole-free. Require an
		// exact one-frame lag; if the grid is missing/stale (loading), skip Hi-Z -> plain frustum/distance cull.
		ID3D11ShaderResourceView* hizSRV = nullptr;
		if (doHiZ) {
			ID3D11ShaderResourceView* srvTmp = nullptr;
			int                       gw = 0, gh = 0, fw = 0, fh = 0;
			std::uint64_t             buildFrame = 0;
			float                     zn = 0.0f, zf = 0.0f;
			if (prevFresh && MOC::GetHiZGridForCompute(srvTmp, gw, gh, fw, fh, buildFrame, zn, zf) &&
				buildFrame + 1 == frame && zf > zn && zn > 0.0f) {
				hizSRV = srvTmp;
				cb.hizValid = 1;
				cb.hizGridW = static_cast<std::uint32_t>(gw);
				cb.hizGridH = static_cast<std::uint32_t>(gh);
				cb.hizFullW = static_cast<std::uint32_t>(fw);
				cb.hizFullH = static_cast<std::uint32_t>(fh);
				cb.hizNear = zn;
				cb.hizFar = zf;
				cb.hizHeight = kGrassHeight;
			}
		}
		g_cullCB->Update(cb);
		// Remember this frame's VP for next frame's Hi-Z reproject (into what will then be the previous grid).
		std::memcpy(g_prevCamVP, cb.camVP, sizeof(g_prevCamVP));
		g_prevCamVPValid = true;
		g_prevCamVPFrame = frame;

		const UINT clear[4] = { 0, 0, 0, 0 };
		ctx->ClearUnorderedAccessViewUint(out.countersUAV.get(), clear);

		ID3D11Buffer*              cb0[1] = { g_cullCB->CB() };
		ID3D11ShaderResourceView*  srv[4] = { g_concatSRV.get(), g_descSRV.get(), g_worldSRV.get(), hizSRV };
		ID3D11UnorderedAccessView* uav[2] = { out.compactedUAV.get(), out.countersUAV.get() };
		ctx->CSSetConstantBuffers(0, 1, cb0);
		ctx->CSSetShaderResources(0, 4, srv);
		ctx->CSSetUnorderedAccessViews(0, 2, uav, nullptr);
		ctx->CSSetShader(g_cullCS.get(), nullptr, 0);
		ctx->Dispatch((g_maxCount + 63u) / 64u, g_batchCount, 1);

		ID3D11UnorderedAccessView* nu[2] = { nullptr, nullptr };
		ID3D11ShaderResourceView*  ns[4] = { nullptr, nullptr, nullptr, nullptr };
		ctx->CSSetUnorderedAccessViews(0, 2, nu, nullptr);
		ctx->CSSetShaderResources(0, 4, ns);

		// Fill the indirect args from the survivor counters.
		ArgsCB ab{};
		ab.batchCount = g_batchCount;
		g_argsCB->Update(ab);
		ID3D11Buffer*              acb[1] = { g_argsCB->CB() };
		ID3D11ShaderResourceView*  asrv[2] = { g_descSRV.get(), out.countersSRV.get() };
		ID3D11UnorderedAccessView* auav[1] = { out.argsUAV.get() };
		ctx->CSSetConstantBuffers(0, 1, acb);
		ctx->CSSetShaderResources(0, 2, asrv);
		ctx->CSSetUnorderedAccessViews(0, 1, auav, nullptr);
		ctx->CSSetShader(g_argsCS.get(), nullptr, 0);
		ctx->Dispatch((g_batchCount + 63u) / 64u, 1, 1);

		ID3D11UnorderedAccessView* nu1[1] = { nullptr };
		ID3D11ShaderResourceView*  ns2[2] = { nullptr, nullptr };
		ctx->CSSetUnorderedAccessViews(0, 1, nu1, nullptr);
		ctx->CSSetShaderResources(0, 2, ns2);
		ctx->CSSetShader(nullptr, nullptr, 0);
		globals::profiler->EndPass();

		static std::uint64_t s_diagA = 0, s_diagB = 0;
		std::uint64_t&       sd = doHiZ ? s_diagB : s_diagA;
		if (frame - sd >= 240 && g_batchCount) {
			sd = frame;
			// Read back the survivor counters (occasional stall) to report the cull rate.
			static winrt::com_ptr<ID3D11Buffer> s_stg;
			if (!s_stg) {
				D3D11_BUFFER_DESC sd2{};
				sd2.ByteWidth = kMaxBatches * 4u;
				sd2.Usage = D3D11_USAGE_STAGING;
				sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				globals::d3d::device->CreateBuffer(&sd2, nullptr, s_stg.put());
			}
			std::uint32_t survivors = 0;
			if (s_stg) {
				ctx->CopyResource(s_stg.get(), out.counters.get());
				D3D11_MAPPED_SUBRESOURCE m{};
				if (SUCCEEDED(ctx->Map(s_stg.get(), 0, D3D11_MAP_READ, 0, &m))) {
					const std::uint32_t* c = reinterpret_cast<const std::uint32_t*>(m.pData);
					for (std::uint32_t i = 0; i < g_batchCount; ++i)
						survivors += c[i];
					ctx->Unmap(s_stg.get(), 0);
				}
			}
			logger::info("[GrassCull] {} {} batches, {}/{} survived ({}% culled) hiz={}", doHiZ ? "hiz" : "base",
				g_batchCount, survivors, g_concatUsed, g_concatUsed ? (100u - survivors * 100u / g_concatUsed) : 0u,
				cb.hizValid);
		}
	}

	std::int32_t GrassDrawDetour(std::int64_t a1, std::int64_t a2, std::uint32_t a3, std::int32_t a4,
		std::uint32_t a5, std::uint64_t a6, std::int64_t* a7, void* a_original)
	{
		auto callOriginal = *static_cast<std::int32_t (**)(std::int64_t, std::int64_t, std::uint32_t,
			std::int32_t, std::uint32_t, std::uint64_t, std::int64_t*)>(a_original);

		// Upper cap is the concat/compacted buffer capacity: a single batch larger than a pool can never be
		// registered without overflowing it, so fall back to the vanilla draw (correct, just unculled).
		if (a5 < kMinInstances || a5 > kMaxInstances || !CullEnabled() || !EnsureShared())
			return callOriginal(a1, a2, a3, a4, a5, a6, a7);

		auto* const instVB = reinterpret_cast<ID3D11Buffer*>(a7[0]);
		auto* const meshVB = reinterpret_cast<ID3D11Buffer*>(*reinterpret_cast<std::int64_t*>(a2));
		auto* const indexBuf = reinterpret_cast<ID3D11Buffer*>(*reinterpret_cast<std::int64_t*>(a2 + 8));
		if (!instVB || !meshVB || !indexBuf)
			return callOriginal(a1, a2, a3, a4, a5, a6, a7);

		auto* const         ctx = *engine::g_context;
		const std::uint64_t frame = globals::state->frameCountAtomic.load(std::memory_order_relaxed);
		if (frame != g_registerFrame) {
			g_registerFrame = frame;
			g_registersThisFrame = 0;
			g_capturesThisFrame = 0;
		}

		Batch& b = g_batches[instVB];
		b.meshVB = meshVB;
		b.indexBuf = indexBuf;
		b.triCount = static_cast<std::uint32_t>(a4);
		b.vertexDesc = a6;
		b.lastSeen = frame;

		// Pass discriminator. Cull ONLY the passes that write the kMAIN scene depth -- the z-prepass and the
		// forward pass -- which share the main camera view-proj and whose depth is mutually early-Z coupled
		// (the z-prepass depth is the forward pass' LESS_EQUAL reference, and grass IS drawn into the
		// z-prepass). They MUST cull the identical survivor set, else a clump the forward pass drops still has
		// z-prepass depth and the terrain behind fails LESS_EQUAL = a grass-shaped hole. Shadows/reflections
		// (other depth targets) stay vanilla. World is captured only in the forward pass (rtv bound), where
		// VS b2 is the grass PerGeometry World.
		ID3D11Texture2D* mainDepthTex = nullptr;
		if (auto* rend = globals::game::renderer)
			mainDepthTex = rend->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].texture;
		ID3D11RenderTargetView* rtv = nullptr;
		ID3D11DepthStencilView* dsv = nullptr;
		ctx->OMGetRenderTargets(1, &rtv, &dsv);
		bool mainDepthPass = false;
		if (dsv && mainDepthTex) {
			ID3D11Resource* res = nullptr;
			dsv->GetResource(&res);
			mainDepthPass = res == static_cast<ID3D11Resource*>(mainDepthTex);
			if (res)
				res->Release();
		}
		const bool forwardPass = rtv != nullptr;
		if (rtv)
			rtv->Release();
		if (dsv)
			dsv->Release();
		if (!mainDepthPass)
			return callOriginal(a1, a2, a3, a4, a5, a6, a7);

		// Register (assign a slot + copy instances into the concat) -- spread across frames to bound the
		// per-new-batch render-pass split cost on cell load.
		if ((!b.registered || b.count != a5) && g_registersThisFrame < kMaxRegistersPerFrame) {
			if (g_batchCount >= kMaxBatches - 1 || g_concatUsed + a5 > kMaxInstances)
				ResetSlots();
			b.batchIdx = g_batchCount++;
			b.srcOffset = g_concatUsed;
			b.count = a5;
			g_concatUsed += a5;
			g_maxCount = a5 > g_maxCount ? a5 : g_maxCount;
			g_descCPU[b.batchIdx] = BatchDesc{ b.srcOffset, a5, b.srcOffset, b.triCount };
			const D3D11_BOX box{ 0, 0, 0, a5 * 32u, 1, 1 };
			ctx->CopySubresourceRegion(g_concat.get(), 0, b.srcOffset * 32u, 0, 0, instVB, 0, &box);
			b.registered = true;
			b.worldCaptured = false;
			b.captureFrame = ~0ull;
			++g_registersThisFrame;
		}

		// ONE burst per frame on the FIRST kMAIN-depth grass draw (z-prepass, earliest). Hi-Z uses the
		// PREVIOUS frame's grid (this frame's is not reduced until PrepassPasses, AFTER the z-prepass), so both
		// the z-prepass AND the forward pass draw the IDENTICAL culled set -> a clump is never in the z-prepass
		// but missing from the forward pass = HOLE-FREE (no orphan depth). Occluded grass leaves both together.
		if (g_burstFrameA != frame && g_batchCount > 0) {
			RunCull(frame, g_setA, HiZCullEnabled());
			g_burstFrameA = frame;
		}

		if (!b.registered)
			return callOriginal(a1, a2, a3, a4, a5, a6, a7);

		// Not yet captured -> draw vanilla in BOTH passes (consistent). Capture this batch's STATIC World
		// (b2 register c8 == bytes 128..192) only in the forward pass, AFTER the engine's own draw has bound
		// b2. World is camera-INDEPENDENT (per-cell translation), captured once; the cull reconstructs
		// WVP = camVP x World per frame.
		if (!b.worldCaptured) {
			if (forwardPass) {
				const std::int32_t r = callOriginal(a1, a2, a3, a4, a5, a6, a7);
				if (g_capturesThisFrame < kMaxCapturesPerFrame) {
					ID3D11Buffer* b2 = nullptr;
					ctx->VSGetConstantBuffers(2, 1, &b2);
					if (b2) {
						const D3D11_BOX c8{ 128, 0, 0, 192, 1, 1 };
						ctx->CopySubresourceRegion(g_world.get(), 0, b.batchIdx * 64u, 0, 0, b2, 0, &c8);
						b2->Release();
						b.worldCaptured = true;
						b.captureFrame = frame;
						++g_capturesThisFrame;
					}
				}
				return r;
			}
			return callOriginal(a1, a2, a3, a4, a5, a6, a7);  // z-prepass, not captured -> vanilla (matches forward)
		}

		// Captured only THIS frame -> the burst ran (on the earlier z-prepass draw) with a stale World for
		// this batch, so its compacted region is garbage. Draw vanilla in BOTH passes until next frame, when
		// the burst has a valid World. Guarantees the z-prepass and forward pass always agree.
		if (b.captureFrame >= frame)
			return callOriginal(a1, a2, a3, a4, a5, a6, a7);

		// --- engine prologue then indirect draw over this batch's compacted region (already culled by the
		// once-per-frame burst above, which processes every registered+captured batch) ---
		auto* const S = reinterpret_cast<std::uint8_t*>(engine::S_base.address());
		auto&       sword = *reinterpret_cast<std::uint32_t*>(S);
		auto&       sVDesc = *reinterpret_cast<std::uint64_t*>(S + engine::kVertexDescOff);
		auto&       sTopo = *reinterpret_cast<std::uint32_t*>(S + engine::kTopologyOff);
		std::uint32_t v10 = sword;
		if (sVDesc != a6) {
			v10 = sword | 0x400u;
			sVDesc = a6;
			sword = v10;
		}
		if (sTopo != 4u) {
			sTopo = 4u;
			sword = v10 | 0x800u;
		}
		engine::SetDirtyStates(0);

		// Both the z-prepass and the forward pass draw the SAME frame-lagged culled set (hole-free).
		OutSet&       out = g_setA;
		ctx->IASetIndexBuffer(indexBuf, DXGI_FORMAT_R16_UINT, 0);
		const UINT    strides[2] = { static_cast<UINT>((4u * a6) & 0x3Cu), static_cast<UINT>((a6 >> 2) & 0x3Cu) };
		const UINT    offsets[2] = { 0, b.srcOffset * 32u };  // stream1 offset into the compacted buffer
		ID3D11Buffer* vbs[2] = { meshVB, out.compacted.get() };
		ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
		ctx->DrawIndexedInstancedIndirect(out.args.get(), b.batchIdx * 20u);
		return 0;
	}

	struct GrassDrawHook
	{
		static std::int32_t thunk(std::int64_t a1, std::int64_t a2, std::uint32_t a3, std::int32_t a4,
			std::uint32_t a5, std::uint64_t a6, std::int64_t* a7)
		{
			return GrassDrawDetour(a1, a2, a3, a4, a5, a6, a7, &func);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void GrassCull::Install()
{
	if (!REL::Module::IsSE()) {
		logger::info("[GrassCull] SE-only; not installing on this runtime");
		return;
	}
	GrassDrawHook::func = REL::Offset(0xD6C1E0).address();
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(reinterpret_cast<PVOID*>(&GrassDrawHook::func), reinterpret_cast<PVOID>(GrassDrawHook::thunk));
	DetourTransactionCommit();
	logger::info("[GrassCull] detoured fDrawGrass @ 0x{:X}", GrassDrawHook::func.address());
}
