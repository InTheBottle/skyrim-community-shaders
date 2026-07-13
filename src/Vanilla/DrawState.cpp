#include "DrawState.h"

#include <format>
#include <string>

#include "Globals.h"
#include "ShaderReflect.h"

namespace vanilla
{
	namespace
	{
		// Per-thread capture arming + current-pass identity. thread_local so each worker context
		// (immediate render thread, or a deferred worker) arms and keys independently.
		thread_local bool          t_armed = false;
		thread_local const void*   t_pass = nullptr;
		thread_local std::uint32_t t_tech = 0;
		thread_local std::vector<DrawFingerprint>* t_fpSink = nullptr;  // compare harness capture sink

		inline std::uint64_t Fnv(const void* p, std::size_t n, std::uint64_t h = 0xcbf29ce484222325ULL)
		{
			const auto* b = static_cast<const std::uint8_t*>(p);
			for (std::size_t i = 0; i < n; ++i) {
				h ^= b[i];
				h *= 0x100000001b3ULL;
			}
			return h;
		}
		inline std::uint64_t FnvU64(std::uint64_t v, std::uint64_t h = 0xcbf29ce484222325ULL)
		{
			return Fnv(&v, sizeof(v), h);
		}

		// Underlying-resource identity of a view (RTV/DSV/SRV). Two different view objects over the same
		// resource fingerprint equal -- what the threaded path needs.
		inline std::uint64_t ResourceId(ID3D11View* a_view)
		{
			if (!a_view)
				return 0;
			winrt::com_ptr<ID3D11Resource> res;
			a_view->GetResource(res.put());
			return reinterpret_cast<std::uint64_t>(res.get());
		}
	}

	std::uint64_t DrawFingerprint::Digest() const
	{
		std::uint64_t h = 0xcbf29ce484222325ULL;
		auto f = [&](std::uint64_t v) { h ^= v; h *= 0x100000001b3ULL; };
		f(vs); f(ps); f(inputLayout); f(topology);
		f(vbCount); f(vbHash); f(ib); f((static_cast<std::uint64_t>(ibFormat) << 32) | ibOffset);
		f(rtCount); f(rtHash); f(dsvResource); f((static_cast<std::uint64_t>(dsvSlice) << 32) | dsvFlags);
		f(blendDescHash); f((static_cast<std::uint64_t>(blendFactorHash) << 32) | sampleMask);
		f(depthDescHash); f(stencilRef); f(rasterDescHash);
		f((static_cast<std::uint64_t>(vpCount) << 32) | scCount); f(vpHash); f(scHash);
		f(psSrvHash); f(psSampHash); f(vsCBHash); f(psCBHash); f(usedCBReflected);
		f(indexCount); f(startIndex); f(static_cast<std::uint32_t>(baseVertex));
		return h;
	}

	std::string DrawFingerprint::Summary() const
	{
		return std::format(
			"vs={:X} ps={:X} il={:X} topo={} rt={} rtH={:016X} dsv={:X}/s{}/f{} bl={:016X} bf={:X}/m{:X} "
			"ds={:016X}/ref{} rs={:016X} vp={} vpH={:016X} sc={}/scH={:016X} psSrv={:016X} psSamp={:016X} "
			"vsCB={:016X} psCB={:016X} refl={} vb={}/{:016X} ib={:X}/f{}/o{} idx={} start={} base={}",
			vs, ps, inputLayout, topology, rtCount, rtHash, dsvResource, dsvSlice, dsvFlags,
			blendDescHash, blendFactorHash, sampleMask, depthDescHash, stencilRef, rasterDescHash,
			vpCount, vpHash, scCount, scHash, psSrvHash, psSampHash, vsCBHash, psCBHash, usedCBReflected,
			vbCount, vbHash, ib, ibFormat, ibOffset, indexCount, startIndex, baseVertex);
	}

	DrawStateValidator* DrawStateValidator::GetSingleton()
	{
		static DrawStateValidator singleton;
		return &singleton;
	}

	void DrawStateValidator::ArmCapture(bool a_on) { t_armed = a_on; }
	bool DrawStateValidator::IsArmed() const { return t_armed; }

	void DrawStateValidator::SetCurrentPass(const void* a_pass, std::uint32_t a_technique)
	{
		t_pass = a_pass;
		t_tech = a_technique;
	}

	void DrawStateValidator::BeginRun()
	{
		captureCount = 0;
		digestRollup = 0;
		logBudget = 48;  // detailed per-draw lines for the first frame captured
	}

	void DrawStateValidator::LogRunSummary(const char* a_tag)
	{
		logger::info("[DrawState][{}] draws={} rollup={:016X}", a_tag, captureCount, digestRollup);
	}

	bool DrawStateValidator::FpVerifyActive()
	{
		static const bool on = ShaderReflect::WantsCapture();
		return on && fpVerifyBudget > 0;
	}

	void DrawStateValidator::CompareFingerprints(const std::vector<DrawFingerprint>& a_engine,
		const std::vector<DrawFingerprint>& a_replica, const void* a_pass, std::uint32_t a_tech)
	{
		if (fpVerifyBudget)
			--fpVerifyBudget;
		++fpVerifyPairs;

		bool        diverged = a_engine.size() != a_replica.size();
		std::size_t firstDiff = SIZE_MAX;
		if (!diverged) {
			for (std::size_t i = 0; i < a_engine.size(); ++i) {
				if (!(a_engine[i] == a_replica[i])) {
					diverged = true;
					firstDiff = i;
					++fpVerifyDrawDiverged;
				}
			}
		}
		if (diverged) {
			++fpVerifyDiverged;
			if (fpVerifyDiverged <= 32) {
				logger::warn("[FpVerify] DIVERGE pass={:X} tech={} draws(E={},R={}) firstDiff={}",
					reinterpret_cast<std::uint64_t>(a_pass), a_tech, a_engine.size(), a_replica.size(),
					firstDiff == SIZE_MAX ? -1 : static_cast<int>(firstDiff));
				if (firstDiff != SIZE_MAX) {
					logger::warn("  E: {}", a_engine[firstDiff].Summary());
					logger::warn("  R: {}", a_replica[firstDiff].Summary());
				}
			}
		}
		if ((fpVerifyPairs & 0x3FFF) == 1)
			logger::info("[FpVerify] pairs={} diverged={} drawDiverged={} budgetLeft={}",
				fpVerifyPairs, fpVerifyDiverged, fpVerifyDrawDiverged, fpVerifyBudget);
	}

	void DrawStateValidator::OnCBWrite(ID3D11DeviceContext* a_ctx, ID3D11Buffer* a_buf, const void* a_data, std::size_t a_size)
	{
		if (!a_ctx || !a_buf || !a_data || !a_size)
			return;
		std::lock_guard lock(cbShadowMutex);
		auto& v = cbShadow[CBKey{ a_ctx, a_buf }];
		v.assign(static_cast<const std::uint8_t*>(a_data), static_cast<const std::uint8_t*>(a_data) + a_size);
	}

	void DrawStateValidator::ClearCBShadow(ID3D11DeviceContext* a_ctx)
	{
		std::lock_guard lock(cbShadowMutex);
		for (auto it = cbShadow.begin(); it != cbShadow.end();)
			it = (it->first.ctx == a_ctx) ? cbShadow.erase(it) : std::next(it);
	}

	bool DrawStateValidator::HashBoundCBBytes(ID3D11DeviceContext* a_ctx, ID3D11Buffer* a_cb, void* a_shaderPtr, int a_slot, bool /*a_ps*/, std::uint64_t& io_hash)
	{
		if (!a_cb) {
			io_hash = FnvU64(0xDEAD0000u | static_cast<std::uint32_t>(a_slot), io_hash);  // used-but-null slot
			return true;
		}
		const ShaderUsage* u = ShaderReflect::GetSingleton()->Get(a_shaderPtr);
		if (!u || !u->valid)
			return false;

		// Deferred context: Map(READ) is illegal, so read the shadowed last write for (ctx, buffer).
		if (a_ctx->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) {
			std::lock_guard lock(cbShadowMutex);
			auto it = cbShadow.find(CBKey{ a_ctx, a_cb });
			if (it == cbShadow.end())
				return false;  // not written on this context yet -> caller marks CB unread
			const std::uint64_t slotHash = u->HashUsedCB(a_slot, it->second.data(), it->second.size());
			io_hash = FnvU64(slotHash, io_hash);
			return true;
		}

		// Immediate context: CopyResource -> staging -> Map(READ).
		D3D11_BUFFER_DESC bd{};
		a_cb->GetDesc(&bd);
		const std::uint32_t size = bd.ByteWidth;
		if (!size)
			return false;

		auto& staging = stagingBuffers[size];
		if (!staging) {
			D3D11_BUFFER_DESC sd{};
			sd.ByteWidth = size;
			sd.Usage = D3D11_USAGE_STAGING;
			sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (!globals::d3d::device || FAILED(globals::d3d::device->CreateBuffer(&sd, nullptr, staging.put())))
				return false;
		}
		a_ctx->CopyResource(staging.get(), a_cb);
		D3D11_MAPPED_SUBRESOURCE m{};
		if (FAILED(a_ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &m)))
			return false;
		const std::uint64_t slotHash = u->HashUsedCB(a_slot, static_cast<const std::uint8_t*>(m.pData), size);
		a_ctx->Unmap(staging.get(), 0);
		io_hash = FnvU64(slotHash, io_hash);
		return true;
	}

	DrawFingerprint DrawStateValidator::Capture(ID3D11DeviceContext* a_ctx, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex)
	{
		DrawFingerprint fp;
		fp.indexCount = a_indexCount;
		fp.startIndex = a_startIndex;
		fp.baseVertex = a_baseVertex;

		winrt::com_ptr<ID3D11VertexShader> vs;
		winrt::com_ptr<ID3D11PixelShader>  ps;
		a_ctx->VSGetShader(vs.put(), nullptr, nullptr);
		a_ctx->PSGetShader(ps.put(), nullptr, nullptr);
		fp.vs = reinterpret_cast<std::uint64_t>(vs.get());
		fp.ps = reinterpret_cast<std::uint64_t>(ps.get());

		winrt::com_ptr<ID3D11InputLayout> il;
		a_ctx->IAGetInputLayout(il.put());
		fp.inputLayout = reinterpret_cast<std::uint64_t>(il.get());

		D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		a_ctx->IAGetPrimitiveTopology(&topo);
		fp.topology = static_cast<std::uint32_t>(topo);

		// Vertex buffers (static geometry: same buffer object across contexts -> identity is stable).
		{
			constexpr UINT kVB = 4;
			ID3D11Buffer* vbs[kVB] = {};
			UINT          strides[kVB] = {}, offsets[kVB] = {};
			a_ctx->IAGetVertexBuffers(0, kVB, vbs, strides, offsets);
			std::uint64_t vbh = 0xcbf29ce484222325ULL;
			std::uint32_t vbc = 0;
			for (UINT i = 0; i < kVB; ++i) {
				if (vbs[i]) {
					++vbc;
					vbh = FnvU64(reinterpret_cast<std::uint64_t>(vbs[i]), vbh);
					vbh = FnvU64((static_cast<std::uint64_t>(strides[i]) << 32) | offsets[i], vbh);
					vbs[i]->Release();
				}
			}
			fp.vbCount = vbc;
			fp.vbHash = vbh;
		}

		{
			winrt::com_ptr<ID3D11Buffer> ib;
			DXGI_FORMAT                  ibf = DXGI_FORMAT_UNKNOWN;
			UINT                         ibo = 0;
			a_ctx->IAGetIndexBuffer(ib.put(), &ibf, &ibo);
			fp.ib = reinterpret_cast<std::uint64_t>(ib.get());
			fp.ibFormat = static_cast<std::uint32_t>(ibf);
			fp.ibOffset = ibo;
		}

		// Render targets + depth-stencil (underlying resource identity + cascade slice).
		{
			ID3D11RenderTargetView* rtvs[8] = {};
			ID3D11DepthStencilView* dsv = nullptr;
			a_ctx->OMGetRenderTargets(8, rtvs, &dsv);
			std::uint64_t rth = 0xcbf29ce484222325ULL;
			std::uint32_t rtc = 0;
			for (UINT i = 0; i < 8; ++i) {
				if (rtvs[i]) {
					++rtc;
					rth = FnvU64(ResourceId(rtvs[i]), rth);
					rtvs[i]->Release();
				}
			}
			fp.rtCount = rtc;
			fp.rtHash = rth;
			if (dsv) {
				fp.dsvResource = ResourceId(dsv);
				D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
				dsv->GetDesc(&dvd);
				fp.dsvFlags = dvd.Flags;
				if (dvd.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY)
					fp.dsvSlice = dvd.Texture2DArray.FirstArraySlice;
				dsv->Release();
			}
		}

		// Blend / depth-stencil / rasterizer: compare by DESC (the threaded path may bind distinct clones).
		{
			ID3D11BlendState* bs = nullptr;
			FLOAT             bf[4] = {};
			UINT              mask = 0;
			a_ctx->OMGetBlendState(&bs, bf, &mask);
			fp.sampleMask = mask;
			fp.blendFactorHash = static_cast<std::uint32_t>(Fnv(bf, sizeof(bf)));
			if (bs) {
				D3D11_BLEND_DESC d{};
				bs->GetDesc(&d);
				fp.blendDescHash = Fnv(&d, sizeof(d));
				bs->Release();
			}
		}
		{
			ID3D11DepthStencilState* dss = nullptr;
			UINT                     ref = 0;
			a_ctx->OMGetDepthStencilState(&dss, &ref);
			fp.stencilRef = ref;
			if (dss) {
				D3D11_DEPTH_STENCIL_DESC d{};
				dss->GetDesc(&d);
				fp.depthDescHash = Fnv(&d, sizeof(d));
				dss->Release();
			}
		}
		{
			ID3D11RasterizerState* rs = nullptr;
			a_ctx->RSGetState(&rs);
			if (rs) {
				D3D11_RASTERIZER_DESC d{};
				rs->GetDesc(&d);
				fp.rasterDescHash = Fnv(&d, sizeof(d));
				rs->Release();
			}
		}
		{
			UINT n = 0;
			a_ctx->RSGetViewports(&n, nullptr);
			if (n > 16)
				n = 16;
			D3D11_VIEWPORT vp[16] = {};
			if (n)
				a_ctx->RSGetViewports(&n, vp);
			fp.vpCount = n;
			fp.vpHash = n ? Fnv(vp, n * sizeof(D3D11_VIEWPORT)) : 0;
		}
		{
			UINT n = 0;
			a_ctx->RSGetScissorRects(&n, nullptr);
			if (n > 16)
				n = 16;
			D3D11_RECT sc[16] = {};
			if (n)
				a_ctx->RSGetScissorRects(&n, sc);
			fp.scCount = n;
			fp.scHash = n ? Fnv(sc, n * sizeof(D3D11_RECT)) : 0;
		}

		// Reflection masks the resource/CB comparison to what the shaders actually use.
		const ShaderUsage* vsU = ShaderReflect::GetSingleton()->Get(vs.get());
		const ShaderUsage* psU = ShaderReflect::GetSingleton()->Get(ps.get());
		fp.usedCBReflected = ((vsU && vsU->valid) || (psU && psU->valid)) ? 1u : 0u;

		// PS SRVs (used slots only, by underlying resource).
		{
			std::uint64_t srvh = 0xcbf29ce484222325ULL;
			ID3D11ShaderResourceView* srvs[32] = {};
			a_ctx->PSGetShaderResources(0, 32, srvs);
			for (UINT s = 0; s < 32; ++s) {
				const bool used = psU && psU->valid && (psU->usedSRVMask & (1u << s));
				if (used)
					srvh = FnvU64(ResourceId(srvs[s]), srvh);
				if (srvs[s])
					srvs[s]->Release();
			}
			fp.psSrvHash = srvh;
		}
		// PS samplers (used slots only, by DESC).
		{
			std::uint64_t samph = 0xcbf29ce484222325ULL;
			ID3D11SamplerState* samps[32] = {};
			a_ctx->PSGetSamplers(0, 32, samps);
			for (UINT s = 0; s < 32; ++s) {
				const bool used = psU && psU->valid && (psU->usedSamplerMask & (1u << s));
				if (used) {
					if (samps[s]) {
						D3D11_SAMPLER_DESC d{};
						samps[s]->GetDesc(&d);
						samph = Fnv(&d, sizeof(d), samph);
					} else {
						samph = FnvU64(0, samph);
					}
				}
				if (samps[s])
					samps[s]->Release();
			}
			fp.psSampHash = samph;
		}

		// Constant buffers: hash the shader-used bytes of the PER-OBJECT slots (b0 per-technique, b1
		// per-material, b2 per-geometry). These vary per draw and are what a threaded/optimized path is
		// most likely to get wrong (wrong arena slice / stale data). Persistent CBs (b12 camera, feature
		// CBs b4-6/b11) are shared, per-frame-stable objects -- trivially equal serial vs threaded -- and
		// aren't written on a worker's deferred context, so they're excluded from the fingerprint.
		constexpr std::uint32_t kPerObjectCBMask = 0b111u;  // slots 0,1,2
		{
			std::uint64_t vscbh = 0xcbf29ce484222325ULL;
			ID3D11Buffer* cbs[3] = {};
			a_ctx->VSGetConstantBuffers(0, 3, cbs);
			for (int s = 0; s < 3; ++s) {
				if (vsU && vsU->valid && (vsU->usedCBMask & kPerObjectCBMask & (1u << s)))
					HashBoundCBBytes(a_ctx, cbs[s], vs.get(), s, false, vscbh);
				if (cbs[s])
					cbs[s]->Release();
			}
			fp.vsCBHash = vscbh;
		}
		{
			std::uint64_t pscbh = 0xcbf29ce484222325ULL;
			ID3D11Buffer* cbs[3] = {};
			a_ctx->PSGetConstantBuffers(0, 3, cbs);
			for (int s = 0; s < 3; ++s) {
				if (psU && psU->valid && (psU->usedCBMask & kPerObjectCBMask & (1u << s)))
					HashBoundCBBytes(a_ctx, cbs[s], ps.get(), s, true, pscbh);
				if (cbs[s])
					cbs[s]->Release();
			}
			fp.psCBHash = pscbh;
		}

		return fp;
	}

	DrawFingerprint DrawStateValidator::CaptureNow(ID3D11DeviceContext* a_ctx, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex)
	{
		return Capture(a_ctx, a_indexCount, a_startIndex, a_baseVertex);
	}

	void DrawStateValidator::SetFingerprintSink(std::vector<DrawFingerprint>* a_sink) { t_fpSink = a_sink; }

	void DrawStateValidator::OnDrawIndexed(ID3D11DeviceContext* a_ctx, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex)
	{
		if (!a_ctx)
			return;
		// Compare harness: collect every draw's fingerprint into the active sink.
		if (t_fpSink) {
			t_fpSink->push_back(Capture(a_ctx, a_indexCount, a_startIndex, a_baseVertex));
			return;
		}
		// Milestone-1 sampling: capture+log only the first `logBudget` shadow draws per run. The CB
		// used-byte read goes through a CopyResource+Map(READ) staging stall, so it is NOT run on every
		// draw yet -- the full-coverage, non-stalling path uses a Map/Unmap CB shadow (comparison stage).
		if (!t_armed || logBudget == 0)
			return;
		const DrawFingerprint fp = Capture(a_ctx, a_indexCount, a_startIndex, a_baseVertex);
		++captureCount;
		digestRollup ^= fp.Digest();
		--logBudget;
		logger::info("[DrawState] pass={:X} t={} {}", reinterpret_cast<std::uint64_t>(t_pass), t_tech, fp.Summary());
	}
}
