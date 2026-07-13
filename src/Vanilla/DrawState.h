#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <d3d11_1.h>
#include <winrt/base.h>

// vanilla:: draw-state validator (regime B). At each shadow DrawIndexed it reads back the EFFECTIVE
// D3D11 pipeline state (*Get* + constant-buffer bytes) and reduces it to a normalized fingerprint
// that ignores everything the multithreaded path is allowed to change -- pointer identity of cloned
// state objects, per-thread buffer objects, call order, which context, and unused slots/bytes -- while
// pinning everything that determines the rendered result. Two runs (engine vs reimpl, serial vs
// threaded) are correct iff their per-pass fingerprints match. See ShaderReflect for the used-byte /
// used-slot masking that makes constant-buffer and resource comparison order/identity independent.
namespace vanilla
{
	/// Normalized effective pipeline state at one DrawIndexed. Equality of two fingerprints ⇔ the two
	/// draws render identically (for the shader-used state). Fields are already normalized: state
	/// objects reduced to a hash of their DESC (blend/depth/raster/sampler may be distinct clones),
	/// resources reduced to their underlying-resource identity (RTV/DSV/SRV/IB views may differ),
	/// constant buffers reduced to a hash of only the shader-used bytes.
	struct DrawFingerprint
	{
		std::uint64_t vs = 0;             // bound VS object ptr (same object expected)
		std::uint64_t ps = 0;             // bound PS object ptr (may be null for depth-only)
		std::uint64_t inputLayout = 0;    // bound IL object ptr (engine-cached, same object)
		std::uint32_t topology = 0;

		std::uint32_t vbCount = 0;        // number of non-null vertex-buffer slots
		std::uint64_t vbHash = 0;         // hash of {resource, stride, offset} over bound VB slots
		std::uint64_t ib = 0;             // index-buffer underlying resource
		std::uint32_t ibFormat = 0;
		std::uint32_t ibOffset = 0;

		std::uint32_t rtCount = 0;        // number of bound RTVs
		std::uint64_t rtHash = 0;         // hash of RTV underlying resources
		std::uint64_t dsvResource = 0;    // DSV underlying resource
		std::uint32_t dsvSlice = 0;       // DSV array slice (cascade)
		std::uint32_t dsvFlags = 0;       // read-only depth/stencil flags

		std::uint64_t blendDescHash = 0;
		std::uint32_t blendFactorHash = 0;
		std::uint32_t sampleMask = 0;
		std::uint64_t depthDescHash = 0;
		std::uint32_t stencilRef = 0;
		std::uint64_t rasterDescHash = 0;

		std::uint32_t vpCount = 0;
		std::uint64_t vpHash = 0;         // hash of D3D11_VIEWPORT array
		std::uint32_t scCount = 0;
		std::uint64_t scHash = 0;         // hash of D3D11_RECT scissor array

		std::uint64_t psSrvHash = 0;      // hash of USED PS-SRV underlying resources
		std::uint64_t psSampHash = 0;     // hash of USED PS-sampler DESCs
		std::uint64_t vsCBHash = 0;       // hash of USED VS constant-buffer bytes
		std::uint64_t psCBHash = 0;       // hash of USED PS constant-buffer bytes
		std::uint32_t usedCBReflected = 0;  // 1 if reflection was available (masks applied), else 0

		std::uint32_t indexCount = 0;
		std::uint32_t startIndex = 0;
		std::int32_t  baseVertex = 0;

		bool operator==(const DrawFingerprint&) const = default;

		/// A compact 64-bit digest of the whole fingerprint (for cheap logging / first-diff detection).
		[[nodiscard]] std::uint64_t Digest() const;
		/// One-line human-readable summary for logs.
		[[nodiscard]] std::string   Summary() const;
	};

	// Named DrawStateValidator (not DrawState) to avoid the Win32 GDI macro `#define DrawState DrawStateW`.
	class DrawStateValidator
	{
	public:
		static DrawStateValidator* GetSingleton();

		/// Arm/disarm per-draw capture for the CALLING thread. The shadow driver arms it around the
		/// shadow-map walk so only shadow draws are fingerprinted; disarms after. thread_local, so each
		/// worker context arms independently.
		void ArmCapture(bool a_on);
		[[nodiscard]] bool IsArmed() const;

		/// Identify the pass a subsequent draw belongs to, so fingerprints can be PAIRED across runs by
		/// pass identity (order-independent). A pass may issue several draws (sub-index) -> a per-pass
		/// draw counter disambiguates. Called by the shadow render loop before dispatching a pass.
		void SetCurrentPass(const void* a_pass, std::uint32_t a_technique);

		/// Capture point, called from the DrawIndexed hooks (immediate + deferred). When armed, reads
		/// back the effective state on `a_ctx`, builds a fingerprint, and (per mode) logs it or records
		/// it for cross-run comparison. Cheap no-op when disarmed.
		void OnDrawIndexed(ID3D11DeviceContext* a_ctx, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex);

		/// Route subsequent OnDrawIndexed captures on THIS thread into `a_sink` (append per draw) instead
		/// of the sampling log. Pass nullptr to stop. Used by the per-pass compare to collect a pass's
		/// draw fingerprints for engine-vs-replica (and later serial-vs-threaded) diffing. When a sink is
		/// set, capture happens regardless of ArmCapture.
		void SetFingerprintSink(std::vector<DrawFingerprint>* a_sink);

		/// Public capture helper (reads back the effective state on `a_ctx`). Exposed so callers that
		/// already know they want a fingerprint (compare harness) can capture directly.
		DrawFingerprint CaptureNow(ID3D11DeviceContext* a_ctx, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex);

		/// Record the bytes written to a constant buffer (a WRITE_DISCARD/NO_OVERWRITE Unmap) so a
		/// draw on a DEFERRED context can read the shader-used bytes without an illegal Map(READ).
		/// Keyed by (context, buffer): DXVK gives a shared buffer object per-context discard slices, so
		/// the immediate and each deferred context must each shadow their own last write. Thread-safe;
		/// fed by the deferred-context Map/Unmap hooks. No-op unless CS_RE_REFLECT.
		void OnCBWrite(ID3D11DeviceContext* a_ctx, ID3D11Buffer* a_buf, const void* a_data, std::size_t a_size);
		/// Drop all shadowed CB contents for a context (e.g. when its command list is finished/reset).
		void ClearCBShadow(ID3D11DeviceContext* a_ctx);

		/// Reset accumulated per-pass fingerprints (start of a run).
		void BeginRun();
		/// Log a summary of the current run's captures (count, digest rollup).
		void LogRunSummary(const char* a_tag);

		/// True while the fingerprint-soundness verify wants more pairs (CS_RE_REFLECT set + budget left).
		/// The compare harness gates its sink set/compare on this to bound the staging-read cost.
		[[nodiscard]] bool FpVerifyActive();

		/// Diff a pass's engine vs replica (or serial vs threaded) draw-fingerprint lists. Tallies
		/// pairs/divergences, logs the first divergences with the diverging draw's before/after summary,
		/// and periodically logs a rolling total. Equal lists ⇒ the two renders produce identical
		/// effective state for every draw of the pass.
		void CompareFingerprints(const std::vector<DrawFingerprint>& a_engine,
			const std::vector<DrawFingerprint>& a_replica, const void* a_pass, std::uint32_t a_tech);

	private:
		DrawStateValidator() = default;

		DrawFingerprint Capture(ID3D11DeviceContext* a_ctx, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex);
		// Read the USED bytes of a bound constant buffer via a staging copy (immediate context only) and
		// fold them into `io_hash`. Returns false if the buffer couldn't be read (e.g. deferred context).
		bool HashBoundCBBytes(ID3D11DeviceContext* a_ctx, ID3D11Buffer* a_cb, void* a_shaderPtr, int a_slot, bool a_ps, std::uint64_t& io_hash);

		// Staging buffers for CB read-back (immediate context), cached by byte size.
		std::unordered_map<std::uint32_t, winrt::com_ptr<ID3D11Buffer>> stagingBuffers;

		// CB content shadow for deferred contexts, keyed by (context, buffer).
		struct CBKey
		{
			ID3D11DeviceContext* ctx;
			ID3D11Buffer*        buf;
			bool                 operator==(const CBKey&) const = default;
		};
		struct CBKeyHash
		{
			std::size_t operator()(const CBKey& k) const
			{
				return std::hash<void*>{}(k.ctx) * 1099511628211ull ^ std::hash<void*>{}(k.buf);
			}
		};
		std::unordered_map<CBKey, std::vector<std::uint8_t>, CBKeyHash> cbShadow;
		std::mutex                                                     cbShadowMutex;

		std::uint64_t captureCount = 0;    // draws fingerprinted this run
		std::uint64_t digestRollup = 0;    // order-independent XOR of per-draw digests (quick run compare)
		std::uint64_t logBudget = 0;       // remaining detailed logs this run

		// Fingerprint-soundness verify (engine-vs-replica in the compare harness) accounting.
		std::uint64_t fpVerifyBudget = 200000;  // max pass-pairs to fingerprint-diff (bounds staging cost)
		std::uint64_t fpVerifyPairs = 0;        // pass-pairs compared
		std::uint64_t fpVerifyDiverged = 0;     // pairs whose fingerprint lists differed
		std::uint64_t fpVerifyDrawDiverged = 0;  // individual draws that differed
	};
}
