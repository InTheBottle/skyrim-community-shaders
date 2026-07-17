#include "GrassCull.h"

#include <atomic>
#include <cstring>
#include <filesystem>
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
	// Cull clumps whose view depth (clip.w) exceeds this -- past the grass fade-out they are fully alpha-faded
	// (invisible) yet still drawn, so removing them is free visually. Generous so only invisible grass is cut.
	constexpr float         kDistanceCullEnd = 8192.0f;
	// Stochastic distance thinning: from kThinStart to kThinEnd, drop up to kThinMax of clumps (denser near,
	// sparser far). Far grass is small/low-detail so this cuts real vertex+overdraw with minimal visual change.
	constexpr float         kThinStart = 3000.0f;
	constexpr float         kThinEnd = 8000.0f;
	constexpr float         kThinMax = 0.6f;
	constexpr std::uint32_t kMaxBatches = 1024;
	constexpr std::uint32_t kMaxInstances = 768u * 1024u;  // concat/compacted capacity
	constexpr std::uint32_t kMaxRegistersPerFrame = 24;    // spread concat-copy + World-capture over frames

	// Hi-Z occlusion bias (NDC-z). Generous: grass is drawn if within this of the cell's farthest
	// occluder, so the coarse (16px) grid + one-frame reprojection error never over-cull visible grass.
	constexpr float kHiZBias = 5.0e-4f;

	struct CullCB
	{
		float         camVP[16];      // this frame's main view-proj (frustum / distance / thinning)
		float         prevCamVP[16];  // last frame's main view-proj (Hi-Z reproject; grid is last frame's)
		float         radiusWorld;
		float         distanceEnd;
		float         thinStart;
		float         thinEnd;
		std::uint32_t batchCount;
		float         thinMax;
		std::uint32_t hizValid;  // 1 = sample the frame-lagged Hi-Z grid bound at t3
		std::uint32_t hizGridW;
		std::uint32_t hizGridH;
		std::uint32_t hizFullW;
		std::uint32_t hizFullH;
		float         hizBias;
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

	winrt::com_ptr<ID3D11ComputeShader> g_cullCS, g_argsCS;
	std::unique_ptr<ConstantBuffer>     g_cullCB, g_argsCB;
	// Shared pools.
	winrt::com_ptr<ID3D11Buffer>              g_concat, g_desc, g_world, g_compacted, g_counters, g_args;
	winrt::com_ptr<ID3D11ShaderResourceView>  g_concatSRV, g_descSRV, g_worldSRV, g_countersSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> g_compactedUAV, g_countersUAV, g_argsUAV;
	bool                                      g_initFailed = false;

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
	std::uint64_t                            g_burstFrame = ~0ull;
	std::uint32_t                            g_registersThisFrame = 0;
	std::uint64_t                            g_registerFrame = ~0ull;
	// Last frame's main view-proj, for the frame-lagged Hi-Z reproject (the grid is last frame's depth).
	float                                    g_prevCamVP[16] = {};
	bool                                     g_prevCamVPValid = false;

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
			g_compacted.attach(MakeBuf(kMaxInstances * 32u, SR | UA | D3D11_BIND_VERTEX_BUFFER, RAW, 0, "GrassCull::Compacted"));
			g_counters.attach(MakeBuf(kMaxBatches * 4u, SR | UA, ST, 4u, "GrassCull::Counters"));
			g_args.attach(MakeBuf(kMaxBatches * 20u, UA | D3D11_BIND_SHADER_RESOURCE, RAW | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0, "GrassCull::Args"));
			if (!g_concat || !g_desc || !g_world || !g_compacted || !g_counters || !g_args) {
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
			srvStruct(g_counters.get(), kMaxBatches, g_countersSRV.put());

			D3D11_UNORDERED_ACCESS_VIEW_DESC ur{};
			ur.Format = DXGI_FORMAT_R32_TYPELESS;
			ur.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			ur.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
			ur.Buffer.NumElements = (kMaxInstances * 32u) / 4u;
			device->CreateUnorderedAccessView(g_compacted.get(), &ur, g_compactedUAV.put());
			ur.Buffer.NumElements = (kMaxBatches * 20u) / 4u;
			device->CreateUnorderedAccessView(g_args.get(), &ur, g_argsUAV.put());
			D3D11_UNORDERED_ACCESS_VIEW_DESC us{};
			us.Format = DXGI_FORMAT_UNKNOWN;
			us.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			us.Buffer.NumElements = kMaxBatches;
			device->CreateUnorderedAccessView(g_counters.get(), &us, g_countersUAV.put());

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

	// Cull all registered batches in one dispatch, then fill the indirect args.
	void RunCull(std::uint64_t frame)
	{
		auto* ctx = globals::d3d::context;

		// Upload the descriptor table.
		D3D11_BOX descBox{ 0, 0, 0, g_batchCount * (UINT)sizeof(BatchDesc), 1, 1 };
		if (g_batchCount)
			ctx->UpdateSubresource(g_desc.get(), 0, &descBox, g_descCPU.data(), 0, 0);

		globals::profiler->BeginPass("GrassCull::Cull");
		auto   vp = globals::game::frameBufferCached.GetCameraViewProj();  // this frame's main view-proj (row-major)
		CullCB cb{};
		std::memcpy(cb.camVP, &vp, sizeof(cb.camVP));
		// Hi-Z reproject uses LAST frame's VP (the grid is last frame's depth); first frame reuses this VP.
		std::memcpy(cb.prevCamVP, g_prevCamVPValid ? g_prevCamVP : cb.camVP, sizeof(cb.prevCamVP));
		cb.radiusWorld = kClumpRadius;
		cb.distanceEnd = kDistanceCullEnd;
		cb.thinStart = kThinStart;
		cb.thinEnd = kThinEnd;
		cb.batchCount = g_batchCount;
		cb.thinMax = kThinMax;
		cb.hizBias = kHiZBias;

		// Frame-lagged Hi-Z occlusion: reuse the OcclusionCulling feature's GPU grid (max-reduced
		// post-z-prepass depth). Grass is IN the z-prepass so the current-frame grid includes this-frame
		// grass -> must use the PREVIOUS frame's grid. At z-prepass time g_hizGridTex still holds last
		// frame's reduce (HiZPrepass overwrites it later at PrepassPasses); MOC stamps the build frame.
		// Require a lag of 1-2 frames: >=1 guarantees the grid is LAST frame's (never this-frame grass ->
		// no self-occlusion), <=2 rejects a stale/frozen grid (loading, paused). Over-cull here degrades to
		// benign missing grass (never holes) because the z-prepass and forward pass now cull identically.
		ID3D11ShaderResourceView* hizSRV = nullptr;
		{
			ID3D11ShaderResourceView* srvTmp = nullptr;
			int                       gw = 0, gh = 0, fw = 0, fh = 0;
			std::uint64_t             buildFrame = 0;
			if (g_prevCamVPValid && MOC::GetHiZGridForCompute(srvTmp, gw, gh, fw, fh, buildFrame)) {
				const std::uint64_t lag = frame - buildFrame;  // buildFrame now filled by the call above
				if (frame > buildFrame && lag <= 2ull) {
					hizSRV = srvTmp;
					cb.hizValid = 1;
					cb.hizGridW = static_cast<std::uint32_t>(gw);
					cb.hizGridH = static_cast<std::uint32_t>(gh);
					cb.hizFullW = static_cast<std::uint32_t>(fw);
					cb.hizFullH = static_cast<std::uint32_t>(fh);
				}
			}
		}
		g_cullCB->Update(cb);

		const UINT clear[4] = { 0, 0, 0, 0 };
		ctx->ClearUnorderedAccessViewUint(g_countersUAV.get(), clear);

		ID3D11Buffer*              cb0[1] = { g_cullCB->CB() };
		ID3D11ShaderResourceView*  srv[4] = { g_concatSRV.get(), g_descSRV.get(), g_worldSRV.get(), hizSRV };
		ID3D11UnorderedAccessView* uav[2] = { g_compactedUAV.get(), g_countersUAV.get() };
		ctx->CSSetConstantBuffers(0, 1, cb0);
		ctx->CSSetShaderResources(0, 4, srv);
		ctx->CSSetUnorderedAccessViews(0, 2, uav, nullptr);
		ctx->CSSetShader(g_cullCS.get(), nullptr, 0);
		ctx->Dispatch((g_maxCount + 63u) / 64u, g_batchCount, 1);

		ID3D11UnorderedAccessView* nu[2] = { nullptr, nullptr };
		ID3D11ShaderResourceView*  ns[4] = { nullptr, nullptr, nullptr, nullptr };
		ctx->CSSetUnorderedAccessViews(0, 2, nu, nullptr);
		ctx->CSSetShaderResources(0, 4, ns);

		// Remember this frame's VP for next frame's Hi-Z reproject.
		std::memcpy(g_prevCamVP, &vp, sizeof(g_prevCamVP));
		g_prevCamVPValid = true;

		// Fill the indirect args from the survivor counters.
		ArgsCB ab{};
		ab.batchCount = g_batchCount;
		g_argsCB->Update(ab);
		ID3D11Buffer*              acb[1] = { g_argsCB->CB() };
		ID3D11ShaderResourceView*  asrv[2] = { g_descSRV.get(), g_countersSRV.get() };
		ID3D11UnorderedAccessView* auav[1] = { g_argsUAV.get() };
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

		static std::uint64_t s_diag = 0;
		if (frame - s_diag >= 240 && g_batchCount) {
			s_diag = frame;
			// Read back the survivor counters (occasional stall) to report the cull rate.
			static winrt::com_ptr<ID3D11Buffer> s_stg;
			if (!s_stg) {
				D3D11_BUFFER_DESC sd{};
				sd.ByteWidth = kMaxBatches * 4u;
				sd.Usage = D3D11_USAGE_STAGING;
				sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				globals::d3d::device->CreateBuffer(&sd, nullptr, s_stg.put());
			}
			std::uint32_t survivors = 0;
			if (s_stg) {
				ctx->CopyResource(s_stg.get(), g_counters.get());
				D3D11_MAPPED_SUBRESOURCE m{};
				if (SUCCEEDED(ctx->Map(s_stg.get(), 0, D3D11_MAP_READ, 0, &m))) {
					const std::uint32_t* c = reinterpret_cast<const std::uint32_t*>(m.pData);
					for (std::uint32_t i = 0; i < g_batchCount; ++i)
						survivors += c[i];
					ctx->Unmap(s_stg.get(), 0);
				}
			}
			logger::info("[GrassCull] {} batches, {}/{} survived ({}% culled) hiz={}", g_batchCount, survivors, g_concatUsed,
				g_concatUsed ? (100u - survivors * 100u / g_concatUsed) : 0u, cb.hizValid);
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

		// Burst once per frame on the FIRST kMAIN-depth grass draw (the z-prepass, earliest in the frame).
		// Both the z-prepass and the forward pass then issue indirect draws over the same compacted survivors.
		if (g_burstFrame != frame && g_batchCount > 0) {
			RunCull(frame);
			g_burstFrame = frame;
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
				if (g_registersThisFrame < kMaxRegistersPerFrame) {
					ID3D11Buffer* b2 = nullptr;
					ctx->VSGetConstantBuffers(2, 1, &b2);
					if (b2) {
						const D3D11_BOX c8{ 128, 0, 0, 192, 1, 1 };
						ctx->CopySubresourceRegion(g_world.get(), 0, b.batchIdx * 64u, 0, 0, b2, 0, &c8);
						b2->Release();
						b.worldCaptured = true;
						b.captureFrame = frame;
						++g_registersThisFrame;
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

		ctx->IASetIndexBuffer(indexBuf, DXGI_FORMAT_R16_UINT, 0);
		const UINT    strides[2] = { static_cast<UINT>((4u * a6) & 0x3Cu), static_cast<UINT>((a6 >> 2) & 0x3Cu) };
		const UINT    offsets[2] = { 0, b.srcOffset * 32u };  // stream1 offset into the shared compacted buffer
		ID3D11Buffer* vbs[2] = { meshVB, g_compacted.get() };
		ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
		ctx->DrawIndexedInstancedIndirect(g_args.get(), b.batchIdx * 20u);
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
