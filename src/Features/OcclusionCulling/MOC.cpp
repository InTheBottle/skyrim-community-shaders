#include <DirectXPackedVector.h>
#include "MOC.h"

#include "Globals.h"
#include "Utils/D3D.h"

#include <d3dcompiler.h>

#include <MaskedOcclusionCulling/CullingThreadpool.h>
#include <MaskedOcclusionCulling/MaskedOcclusionCulling.h>
#include <meshoptimizer.h>

#include <d3d11.h>
#include <winrt/base.h>
#include <tlhelp32.h>
#include <DirectXTex.h>

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <RE/B/BSGeometry.h>
#include <RE/B/BSLightingShaderProperty.h>
#include <RE/B/BSSceneGraph.h>
#include <RE/B/BSMultiBound.h>
#include <RE/B/BSMultiBoundAABB.h>
#include <RE/B/BSMultiBoundNode.h>
#include <RE/B/BSMultiBoundShape.h>
#include <RE/B/BSShaderProperty.h>
#include <RE/B/BSTriShape.h>
#include <RE/A/Actor.h>
#include <RE/N/NiCamera.h>
#include <RE/N/NiMatrix3.h>
#include <RE/N/NiNode.h>
#include <RE/N/NiPoint3.h>
#include <RE/N/NiRTTI.h>
#include <RE/N/NiTransform.h>
#include <RE/R/Renderer.h>
#include <RE/R/RendererShadowState.h>
#include <RE/L/LoadingMenu.h>
#include <RE/S/State.h>
#include <RE/U/UI.h>
#include <RE/V/VertexDesc.h>

using namespace DirectX;

namespace MOC
{
	// ---- Settings (defaults mirror Nukem's) ----
	bool  EnableOcclusionTesting = true;
	bool  EnableOccluderRendering = true;
	float OccluderMaxDistance = 20000.0f;
	float OccluderFirstLevelMinSize = 200.0f;
	// Cap on occluders rasterized per frame (closest-first after the sort). NOT a MOC
	// library limit -- a budget for the per-frame build. With the threaded raster +
	// simplified meshes the default covers everything a typical scene gathers.
	std::uint32_t MaxOccludersPerFrame = 4096;
	// Occluder mesh simplification at cache time -- fully live-tunable (changing ANY of
	// these invalidates the cache and rebuilds the active occluder geometry). Every
	// meshoptimizer algorithm is selectable so the effect can be compared in-game.
	//   Mode: 0 = Off (full-res occluders)
	//         1 = Quality (meshopt_simplify, topology-preserving, honours Options)
	//         2 = Sloppy  (meshopt_simplifySloppy, ignores topology, silhouette may move)
	//         3 = Prune   (meshopt_simplifyPrune, only drops small disconnected parts)
	//   Strength = base target as a fraction of the index count (Quality/Sloppy).
	//   Error    = target error (relative to mesh extents unless the ErrorAbsolute
	//              option bit is set); also the Prune size threshold.
	//   Options  = meshopt_SimplifyX bitmask (Quality only): LockBorder|Sparse|
	//              ErrorAbsolute|Prune = bits 0..3.
	int          SimplifyMode = 1;
	float        SimplifyStrength = 0.25f;
	float        SimplifyError = 1e-2f;
	unsigned int SimplifyOptions = 0;
	// Build + use the runtime distance LODs (two sloppy-simplified coarser meshes picked
	// per frame by projected error). OFF = every occluder rasterizes at its base mesh
	// regardless of distance. Sloppy LODs are NOT conservative (the silhouette can grow),
	// so turning this off removes any LOD-caused over-occlusion at the cost of a heavier
	// raster. Live-rebuilt on change, like the other simplify knobs.
	bool BuildRuntimeLODs = true;
	// Only objects/subtrees with at least this world-bound radius are occlusion-tested.
	// Lower = more tested objects (more draws saved) at more test cost per frame.
	float OccluderTestMinRadius = 0.0f;
	// MOC-exclusive mode: zero the compound frustum's operator count around the main
	// culls so the vanilla occlusion planes never process; the engine keeps its plain
	// view-frustum culling and MOC provides ALL occlusion. Off by default.
	bool ExclusiveOcclusion = false;
	// Leaf gate for the occluder GATHER: geometry below this world-bound radius is not
	// rasterized into the buffer ("render everything apart from small objects").
	float OccluderMinLeafSize = 100.0f;
	// DIAGNOSTIC: forcibly cull this percentage of objects that survive the real tests
	// (stable per-object hash). Answers "at what cull volume does perf improve" on a
	// given profile, independent of MOC's actual yield. BREAKS THE IMAGE -- probe only.
	std::int32_t DiagForceCullPercent = 0;
	bool CullTreeLODGroups = true;
	bool TreeOccluders = true;
	bool AlphaTestedOccluders = false;
	// Rasterize the game's distant terrain LOD (.btr under LODRoot/LandLOD) as occluders.
	// OFF by default: unlike the heightmap-built land meshes (which take the per-quad MIN
	// height and so stay below the real surface), the coarse LOD terrain can bulge ABOVE
	// the real ground and over-occlude -> wrongly culls visible geometry. Heightmap land
	// (loaded grid) is always used regardless.
	bool TerrainLODOccluders = false;
	// Async occlusion testing: move the per-object test OFF the scene-list cull walk. The
	// builder tests a snapshot of last frame's candidates against THIS frame's buffer and
	// publishes a visibility table; Process1 reads the table instead of testing inline, so
	// the test never blocks the BuildSceneLists barrier (no Utility inflation). Bounds are
	// value-copied (world-space -> frame-stable for statics), the buffer is same-frame, and
	// the object pointer is a hash key only (never deref'd off-thread) -> no dangling deref,
	// no wrong culls (a miss keeps). Experimental; off by default.
	bool AsyncOcclusionTest = false;

	// Builder-internal (defined after TestSphereBound; called from BuilderLoop above it).
	void RunAsyncTest();

	// Two independent distance-scaled small-object culls, both pure size/distance
	// math vs the player camera (min + slope*camDist), no raster buffer. Neither
	// ever culls actors or kAlwaysDraw objects.
	//   VISIBLE: drops the small object from the MAIN view (its mesh stops
	//     rendering). Off by default -- removing on-screen objects pops in motion.
	//   SHADOWS: drops the small object from the SHADOW MAPS only (mesh still
	//     renders; just its shadow disappears). On by default -- shadow-map draws
	//     are the bulk of the Utility shader time and a distant small object's
	//     shadow is imperceptible, so this cuts Utility with no visible change.
	bool  CullSmallVisible = false;
	float SmallVisibleMinSize = 0.0f;
	float SmallVisibleSlope = 0.012f;
	bool  CullSmallShadows = true;
	float SmallShadowMinSize = 0.0f;
	float SmallShadowSlope = 0.03f;  // measured to net Utility down vs occlusion-off

	namespace
	{
		// Lower res = far cheaper raster (coverage cost scales with pixels) at coarser but
		// still-conservative occlusion. 400 full-detail meshes @1280x720 cost ~65ms/frame.
		// Buffer resolution (boot-time; CS_MOC_RES=WxH overrides, W mult of 8, H mult of 4).
		// Higher = tighter conservative tests = more culling yield; raster cost is on the
		// worker threads, so the on-path cost is resolution-independent.
		unsigned int MOC_WIDTH = 480;
		unsigned int MOC_HEIGHT = 270;

		// STANDARD Intel MOC model (sample code / paper): ONE buffer per frame --
		// clear, rasterize the occluders with THIS frame's matrices, and only
		// after the raster is COMPLETE run the occlusion tests with the same
		// matrices. Tests that arrive before completion briefly wait (bounded);
		// raster and tests are always matched and deterministic, so every cull
		// walk of the frame computes identical verdicts -- no verdict caches,
		// no temporal hysteresis, no flicker by construction.
		MaskedOcclusionCulling* g_moc = nullptr;       // BACK: builder rasterizes this frame's occluders here
		MaskedOcclusionCulling* g_mocSpare = nullptr;  // ping-pong partner instance
		std::atomic<MaskedOcclusionCulling*> g_mocFront{ nullptr };  // FRONT: prev-frame completed buffer (testers)
		bool          g_prevFrameOcclusion = false;   // CS_MOC_PREV_FRAME: test last frame's buffer, no raster wait
		bool          g_mocFlagTest = false;          // CS_MOC_FLAGTEST: flag-file runtime A/B (off/sync/prev-frame)
		std::uint32_t g_lastKicked = 0xFFFFFFFFu;      // frame of the last builder kick (build-claim thread only)
		// Buffer the cull tests against: FRONT (prev-frame) in one-frame-behind mode, else the live BACK
		// (paired with WaitForRasterComplete). One-frame-behind holds NO cross-frame OBJECT pointers -- it
		// tests LIVE objects against a stale BUFFER -- so it is lifetime-safe (unlike AsyncOcclusionTest),
		// and every walk in a frame reads the same buffer, so there is no per-walk flicker.
		inline MaskedOcclusionCulling* TestBuf()
		{
			return g_prevFrameOcclusion ? g_mocFront.load(std::memory_order_acquire) : g_moc;
		}

		// =========================================================================================
		// Hi-Z GPU occlusion (CS_MOC_HIZ / moc_hiz.flag under CS_MOC_FLAGTEST): replace the CPU
		// occluder RASTER with the game's own depth buffer. A tiny compute pass max-reduces the
		// post-zprepass depth (STANDARD Z: near=0, far=1 -- verified via SharedData::GetScreenDepth)
		// into a (w/16, h/16) grid of "farthest depth per 16x16 block"; a 3-deep staging ring reads
		// it back without stalling (Map DO_NOT_WAIT on a slot written kRing frames earlier); the
		// cull tests object screen rects against the CPU grid (HiZTestRect) instead of MOC TestRect.
		// EVERY opaque surface occludes (not 384 hand-picked meshes) and the software raster + its
		// builder thread disappear from the frame. Inherently 1+kRing frames stale -- the same
		// class as the one-frame-behind mode that was visually clean; each snapshot carries the
		// matrices of the frame its depth was rendered with, so the test projection matches the
		// buffer exactly (no reprojection error by construction).
		bool  g_hizMode = false;
		float g_hizBias = 1e-4f;  // occluded iff objNdcMin > cellMax + bias (standard Z)

#pragma warning(push)
#pragma warning(disable: 4324)  // padding from the XMMATRIX/XMVECTOR alignment is intended
		struct HiZSnapshot
		{
			std::vector<float> grid;  // gridW*gridH, farthest (max) depth per 16x16 block
			int                gridW = 0, gridH = 0;
			int                fullW = 0, fullH = 0;
			XMMATRIX           view{};
			XMMATRIX           viewProj{};
			RE::NiPoint3       posAdjust{};
			XMVECTOR           posAdjustV = _mm_setzero_ps();
		};
		// Two snapshots recycled alternately; readers grab the published pointer and finish a
		// per-object test in microseconds while publishes are a frame apart -- same benign
		// 2-slot recycle contract as g_mocFront above.
		HiZSnapshot                      g_hizSnaps[2];
		std::atomic<const HiZSnapshot*>  g_hizFront{ nullptr };
		int                              g_hizWriteSnap = 0;

		constexpr int kHizRing = 3;
		struct HiZRingSlot
		{
			winrt::com_ptr<ID3D11Texture2D> staging;
			XMMATRIX                        view{};
			XMMATRIX                        viewProj{};
			RE::NiPoint3                    posAdjust{};
			bool                            pending = false;
		};
		HiZRingSlot                               g_hizRing[kHizRing];
#pragma warning(pop)
		int                                       g_hizWrite = 0;
		winrt::com_ptr<ID3D11ComputeShader>       g_hizCS;
		winrt::com_ptr<ID3D11Texture2D>           g_hizGridTex;
		winrt::com_ptr<ID3D11UnorderedAccessView> g_hizGridUAV;
		int                                       g_hizGridW = 0, g_hizGridH = 0, g_hizFullW = 0, g_hizFullH = 0;
		bool                                      g_hizInitTried = false, g_hizReady = false;

		// The Hi-Z replacement for MaskedOcclusionCulling::TestRect. Inputs are the SAME NDC rect
		// the MOC seams already compute, plus the object's NEAREST NDC depth (min z/w over the
		// bound). Occluded iff that nearest point is deeper than the FARTHEST depth over every
		// grid cell the rect touches. Conservative by construction: coarse cells only over-
		// estimate occluder distance (max-reduce), OOB depth reads contributed 0 (near) which a
		// max ignores, off-screen rects are kept (the engine's frustum cull owns that verdict).
		bool HiZTestRect(float a_minX, float a_minY, float a_maxX, float a_maxY, float a_objNdcZMin)
		{
			const HiZSnapshot* s = g_hizFront.load(std::memory_order_acquire);
			if (!s || s->grid.empty())
				return true;
			if (a_maxX < -1.0f || a_minX > 1.0f || a_maxY < -1.0f || a_minY > 1.0f)
				return true;  // fully off-screen in the snapshot's view: keep
			// NDC -> UV (y flips) -> grid-cell range (cells cover 16px blocks of the full res).
			const float u0 = (a_minX + 1.0f) * 0.5f, u1 = (a_maxX + 1.0f) * 0.5f;
			const float v0 = (1.0f - a_maxY) * 0.5f, v1 = (1.0f - a_minY) * 0.5f;
			const int cx0 = std::clamp(static_cast<int>(u0 * s->fullW) / 16, 0, s->gridW - 1);
			const int cx1 = std::clamp(static_cast<int>(u1 * s->fullW) / 16, 0, s->gridW - 1);
			const int cy0 = std::clamp(static_cast<int>(v0 * s->fullH) / 16, 0, s->gridH - 1);
			const int cy1 = std::clamp(static_cast<int>(v1 * s->fullH) / 16, 0, s->gridH - 1);
			if ((cx1 - cx0 + 1) * (cy1 - cy0 + 1) > 256)
				return true;  // huge screen rect: almost surely visible; skip the scan
			float farthest = 0.0f;
			for (int y = cy0; y <= cy1; ++y) {
				const float* row = &s->grid[static_cast<std::size_t>(y) * s->gridW];
				for (int x = cx0; x <= cx1; ++x)
					farthest = std::max(farthest, row[x]);
			}
			return a_objNdcZMin <= farthest + g_hizBias;  // false => occluded
		}

		// SUN-SHADOW occlusion (the +30% pool: utility/shadow = 52% of draws).
		// A second buffer rasterized from the sun's shadow-gather view; the
		// engine's stage-1 caster pre-gather (camera = dirLight+0x578, IDA
		// 2026-07-11) dispatches through the SAME Process1 body we detour, so
		// casters provably occluded from the sun are rejected for ALL cascades
		// at once. The gather camera is hysteresis-stabilized by the engine, so
		// a buffer built at kick time is valid for the whole frame. The ortho
		// sun volume is approximated by a pushed-back narrow-FOV perspective
		// camera so all existing (perspective, 1/w) machinery applies.
		std::atomic<const RE::NiCamera*> g_sunGatherCam{ nullptr };  // shadow-caster pre-gather cam
		std::atomic<std::uint64_t>       g_visTested{ 0 };
		std::atomic<std::uint64_t>       g_visCulled{ 0 };
		std::atomic<std::uint64_t>       g_shadowTested{ 0 };
		std::atomic<std::uint64_t>       g_shadowCulled{ 0 };
		CullingThreadpool*      g_pool = nullptr;
		bool                    g_init = false;

		// Camera matrices for the current build (camera-relative, row-vector layout).
		XMMATRIX     g_view = XMMatrixIdentity();
		XMMATRIX     g_proj = XMMatrixIdentity();
		XMMATRIX     g_viewProj = XMMatrixIdentity();
		RE::NiPoint3 g_posAdjust{ 0.0f, 0.0f, 0.0f };
		XMVECTOR     g_posAdjustV = _mm_setzero_ps();  // (x, y, z, 0)
		fplanes      g_frustum{};

		// Once-per-frame build coordination. Main-scene culls run CONCURRENTLY on several
		// job threads (BuildSceneLists cell culls) plus the render thread, all reaching
		// BuildOccluders -- a plain frame compare let multiple threads build at once and
		// race on g_geoList (crash: null geometry in the raster loop). One thread CLAIMS
		// the frame (CAS), builds, then PUBLISHES via g_buildDone; concurrent losers skip
		// testing for their pass (conservative: everything stays visible) until published.
		std::atomic<std::uint32_t> g_buildClaim{ 0xFFFFFFFFu };
		std::atomic<std::uint32_t> g_buildDone{ 0xFFFFFFFFu };

		struct GeoEntry
		{
			RE::BSGeometry* geometry;
			float           distanceSquared;
			float           coverageScore;  // worldBound.radius / distance -- projected-size proxy
			// Resolved at gather time (Phase 2 pre-warm) so the raster enqueue does
			// ZERO map lookups: enq was ~1.0ms of the 2.4ms raster at 409 meshes,
			// and raster-completion latency is the frame's entire MOC tax (the cull
			// jobs wait on it -- NO_WAIT probe measured -1.3% vs -19.3%).
			const std::uint32_t* idxData = nullptr;
			std::uint32_t        idxCount = 0;
			const float*         verts = nullptr;
			bool                 twoSided = false;
			float                radius = 0.0f;
			const std::uint32_t* lod1Data = nullptr;
			std::uint32_t        lod1Count = 0;
			float                lod1ErrAbs = 0.0f;  // errRel * radius
			const std::uint32_t* lod2Data = nullptr;
			std::uint32_t        lod2Count = 0;
			float                lod2ErrAbs = 0.0f;
		};
		std::vector<GeoEntry> g_geoList;

		// Occluder-list BUILDER thread: the scene-graph walk + sort + vertex-conversion
		// pre-warm run here, OFF the engine's cull critical path. The claim thread only
		// enqueues the previously prepared list (fresh matrices, warm caches). The list is
		// one frame stale -- occluders are static meshes, and a newly streamed cell's
		// occluders appearing a frame late is conservative (less culling, never wrong).
		std::thread             g_builder;
		std::mutex              g_builderMtx;
		std::condition_variable g_builderCV;
		bool                    g_builderKick = false;
		bool                    g_builderQuit = false;
		std::vector<GeoEntry>   g_readyList;   // builder-owned prepared list
		std::atomic<bool>       g_builderBusy{ false };  // true while a kick is being processed
		double                  g_lastGatherMs = 0.0;
		std::uint32_t              g_lastOccluderCount = 0;

		// Walk-shape tallies (written by the single build thread, read by its diag print).
		std::uint32_t g_cellsSeen = 0;
		std::uint32_t g_cellsCulled = 0;



		// Lightweight diagnostic tallies (logged from BuildOccluders, not per-test).
		std::atomic<std::uint64_t> g_tested{ 0 };
		std::atomic<std::uint64_t> g_culled{ 0 };
		std::atomic<std::uint64_t> g_testedAABB{ 0 };
		std::atomic<std::uint64_t> g_culledAABB{ 0 };
		std::atomic<std::uint64_t> g_testedSphere{ 0 };
		std::atomic<std::uint64_t> g_culledSphere{ 0 };
		std::atomic<std::uint64_t> g_treeTested{ 0 };
		std::atomic<std::uint64_t> g_treeCulled{ 0 };
		std::atomic<std::uint64_t> g_kickCounter{ 0 };
		std::atomic<std::uint64_t> g_settleUntilKick{ 0 };

		// Raster-complete handshake: the claim path stamps g_kickFrame before
		// waking the builder; the builder stamps g_rasterFrame when the raster
		// phase finishes. Tests wait (bounded) until g_rasterFrame matches the
		// frame they are testing for.
		std::atomic<std::uint32_t> g_kickFrame{ 0xFFFFFFFFu };    // claim -> builder
		std::atomic<std::uint32_t> g_rasterFrame{ 0xFFFFFFFFu };  // builder -> testers
		// CS_MOC_NO_WAIT=1: PERF PROBE ONLY (like force-cull) -- queries that
		// arrive before raster completion return visible instead of waiting.
		// Reintroduces per-walk inconsistency (flicker class); never ship on.
		bool g_noWaitProbe = false;
		bool g_hotWorkers = false;
		std::atomic<std::uint64_t> g_waitNs{ 0 };          // total test-side wait
		std::atomic<std::uint64_t> g_waitCount{ 0 };       // waits that actually spun
		double                     g_lastRasterMs = 0.0;   // builder thread only
		double                     g_lastEnqueueMs = 0.0;  // builder thread only

		// True once the buffer for @p a_frame is complete; false when no build
		// was kicked this frame (menus, loads) or the bounded wait expired --
		// callers then skip culling (conservative).
		bool WaitForRasterComplete(std::uint32_t a_frame)
		{
			if (g_rasterFrame.load(std::memory_order_acquire) == a_frame)
				return true;
			if (g_kickFrame.load(std::memory_order_relaxed) != a_frame)
				return false;  // no build this frame -- nothing to test against
			if (g_noWaitProbe)
				return false;  // probe: never wait, skip culling until complete
			const auto t0 = std::chrono::steady_clock::now();
			const auto deadline = t0 + std::chrono::milliseconds(4);
			bool ok = false;
			for (;;) {
				if (g_rasterFrame.load(std::memory_order_acquire) == a_frame) {
					ok = true;
					break;
				}
				if (std::chrono::steady_clock::now() >= deadline)
					break;
				std::this_thread::yield();
			}
			g_waitNs.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed);
			g_waitCount.fetch_add(1, std::memory_order_relaxed);
			return ok;
		}
		std::chrono::high_resolution_clock::time_point g_buildStart;

		// Debug depth-buffer view: MOC's software depth rasterized to an RGBA8 texture
		// for display in the feature settings (ComputePixelDepthBuffer -> grayscale).
		winrt::com_ptr<ID3D11Texture2D>          g_debugTex;
		winrt::com_ptr<ID3D11ShaderResourceView> g_debugSRV;
		std::vector<float>                       g_debugDepth;
		// Flicker-free debug view: the BUILDER publishes a complete-buffer snapshot
		// right after rasterization finishes; the UI only ever uploads settled
		// snapshots (a live read from the UI thread races the async fill and shows
		// half-built buffers -- "objects flicker", far/late occluders like terrain
		// mostly absent). g_debugViewFrames > 0 keeps snapshotting for ~1s after the
		// last UI request so the view stays live while the menu is open.
		std::atomic<int>   g_debugViewFrames{ 0 };
		std::mutex         g_debugSnapMtx;
		std::vector<float> g_debugSnapshot;
		bool               g_debugSnapValid = false;

		// --- Per-mesh converted vertex/index cache, keyed by rendererData (stable GPU mesh ptr) ---
		struct IndexPair
		{
			std::uint32_t* Data = nullptr;
			std::uint32_t  Count = 0;      // total indices (== triCount * 3)
			std::uint32_t  VertCount = 0;  // verts in the cached position stream
			// Runtime occluder LODs (user design): sloppy-simplified index sets
			// sharing the same vertex stream, selected per frame by projected
			// error < half a buffer pixel -- silhouette expansion the buffer
			// cannot resolve is harmless, and distant occluders carry most of
			// the triangle volume. errRel = meshopt result_error (relative to
			// mesh extents); absolute error ~= errRel * mesh radius.
			std::uint32_t* Lod1Data = nullptr;
			std::uint32_t  Lod1Count = 0;
			float          Lod1ErrRel = 0.0f;
			std::uint32_t* Lod2Data = nullptr;
			std::uint32_t  Lod2Count = 0;
			float          Lod2ErrRel = 0.0f;
		};

		std::unordered_map<void*, float*>    g_vertMap;
		std::unordered_map<void*, IndexPair> g_indexMap;
		std::mutex                           g_cacheMutex;

		// Free all three index buffers an IndexPair owns (base + both LODs). The old
		// per-mesh/shutdown frees only released Data, leaking the LOD buffers.
		inline void FreeIndexPair(IndexPair& p)
		{
			delete[] p.Data;
			delete[] p.Lod1Data;
			delete[] p.Lod2Data;
			p = {};
		}

		// Set by the UI/REST thread when a simplification setting changes; the builder
		// clears the mesh cache at the top of its next kick (it owns the ready list, so
		// no cross-thread race) and re-converts every occluder with the new settings.
		std::atomic<bool> g_rebuildMeshCache{ false };

		// --- Async occlusion-test state (see AsyncOcclusionTest) ---
		// Snapshot of cull candidates, double-buffered: Process1 appends to g_snapAppend
		// (this frame), the builder tests the OTHER buffer (last frame's) after its raster.
		struct SnapEntry
		{
			const void* key;             // object pointer -- hash key ONLY, never deref'd here
			float       cx, cy, cz, r;   // value-copied world bound
		};
		constexpr std::uint32_t    kSnapCap = 1u << 18;  // 262144 candidates/frame cap
		// PROBE: verdict stored directly on the object as an unused NiAVObject flag bit
		// (bit 31 -- absent from the Skyrim Flag enum). Read is then a free bit-test on a
		// cache line Process1 already touches (no map lookup) -- measures the async CEILING.
		constexpr std::uint32_t    kMocOccludedBit = 1u << 31;
		std::vector<SnapEntry>     g_snap[2];
		std::atomic<std::uint32_t> g_snapCount[2]{ { 0 }, { 0 } };
		std::atomic<int>           g_snapAppend{ 0 };  // buffer Process1 appends to this frame
		// {version, testBuffer} published as ONE atomic (version << 1 | bufIdx) so the builder
		// reads a CONSISTENT pair -- a flip between separate reads would let it test a buffer
		// the current frame is appending to yet stamp with the current version (defeating the
		// gate). The builder stamps its table with the version it captured here; if a flip
		// then advances the version, the gate rejects the (possibly torn) table -> keep.
		std::atomic<std::uint64_t> g_asyncStamp{ 0 };  // (version << 1) | testBuffer, published together


		// WorldScenegraph global (holds a NiNode*). Nukem 0x2F4CE30 -> SE REL::ID(517006).
		REL::Relocation<RE::NiNode**> g_worldScenegraph{ REL::ID(517006) };

		// The engine's frozen CULL camera (0x1432333F0 in 1.5.97): the NiCamera object that
		// DrawWorld hands to every MAIN-scene cull (BuildSceneLists cell culls, MainAccum
		// subtree culls, RenderDepth, RenderWorld). Distinct from the render camera -- its
		// NiFrustum is normalized to (-1,1,1,-1), so it identifies the pass but can NOT
		// provide the projection.
		REL::Relocation<RE::NiCamera**> g_cullCamera{ REL::ID(528062) };

		// ---------------------------------------------------------------------
		// Vertex / index conversion (solved raw-geometry code, FULLPREC-aware).
		// ---------------------------------------------------------------------

		// Vertex-buffer SLOT 0 stride, straight from the desc bits -- the game's own
		// BSGeometry::CalculateVertexSize: (desc << 2) & 0x3C (lowest nibble * 4). This is
		// NUKEM-EXACT and NOT CommonLib's VertexDesc::GetSize() (which describes a
		// different, GPU-interleaved layout; deriving the raw stride from it produced
		// garbage triangle soup for most meshes).
		std::uint32_t GetVertexStride(const RE::BSGraphics::VertexDesc& a_desc)
		{
			const auto raw = reinterpret_cast<const std::uint64_t&>(a_desc);
			return static_cast<std::uint32_t>((raw << 2) & 0x3C);
		}

		std::uint32_t* ConvertIndices(const std::uint16_t* a_in, std::uint32_t a_count)
		{
			auto* out = new std::uint32_t[a_count];
			for (std::uint32_t i = 0; i < a_count; ++i)
				out[i] = a_in[i];
			return out;
		}

		// MOC float4 layout: (x, y, 1.0, z) -- W carries z, per Nukem. NUKEM-EXACT decode:
		// the slot-0 raw CPU data stores positions as PLAIN FLOATS at byte offset 0 of each
		// vertex (his working mod reads floats unconditionally -- no half-precision decode,
		// regardless of VF_FULLPREC, which describes the GPU-side packing only).
		float* ConvertVerts(const std::uint8_t* a_raw, std::uint32_t a_count, std::uint32_t a_stride)
		{
			auto* out = new float[static_cast<std::size_t>(a_count) * 4];
			for (std::uint32_t i = 0; i < a_count; ++i) {
				const auto* f = reinterpret_cast<const float*>(a_raw + static_cast<std::size_t>(i) * a_stride);
				out[i * 4 + 0] = f[0];
				out[i * 4 + 1] = f[1];
				out[i * 4 + 2] = 1.0f;
				out[i * 4 + 3] = f[2];
			}
			return out;
		}

		// BSShaderUtil::GetXMFromNiPosAdjust equivalent: local -> (world - posAdjust),
		// row-vector convention (uses R transposed since NiTransform is column-vector).
		XMMATRIX GetXMFromNiPosAdjust(const RE::NiTransform& t, const RE::NiPoint3& posAdjust)
		{
			const RE::NiMatrix3& R = t.rotate;
			const float          s = t.scale;

			XMMATRIX m;
			m.r[0] = XMVectorSet(s * R.entry[0][0], s * R.entry[1][0], s * R.entry[2][0], 0.0f);
			m.r[1] = XMVectorSet(s * R.entry[0][1], s * R.entry[1][1], s * R.entry[2][1], 0.0f);
			m.r[2] = XMVectorSet(s * R.entry[0][2], s * R.entry[1][2], s * R.entry[2][2], 0.0f);
			m.r[3] = XMVectorSet(t.translate.x - posAdjust.x,
				t.translate.y - posAdjust.y,
				t.translate.z - posAdjust.z, 1.0f);
			return m;
		}

		// Fetch (and lazily convert + cache) the MOC verts/indices for a static kTriShape.
		bool GetCachedGeometry(RE::BSGeometry* a_geom, IndexPair& a_outIndices, float*& a_outVerts)
		{
			// Accept every BSTriShape-derived type with the standard TriShape runtime
			// data. Critically this includes kSubIndexLandTriShape/kSubIndexTriShape --
			// ACTIVE-CELL TERRAIN -- which the kTriShape-only gate silently rejected,
			// leaving holes in the buffer floor ("seeing through the ground" = nothing
			// behind terrain ever culled). Dynamic/particle/instanced types stay out.
			switch (a_geom->GetType().get()) {
			case RE::BSGeometry::Type::kTriShape:
			case RE::BSGeometry::Type::kMeshLODTriShape:
			case RE::BSGeometry::Type::kLODMultiIndexTriShape:
			case RE::BSGeometry::Type::kMultiIndexTriShape:
			case RE::BSGeometry::Type::kSubIndexTriShape:
			case RE::BSGeometry::Type::kSubIndexLandTriShape:
				break;
			default:
				return false;
			}

			auto&                     geomRT = a_geom->GetGeometryRuntimeData();
			RE::BSGraphics::TriShape* rendererData = geomRT.rendererData;
			if (!rendererData || !rendererData->rawVertexData || !rendererData->rawIndexData) {
				// One-shot diag: LARGE geometry rejected here = holes in the buffer floor.
				static std::atomic<int> s_logBudget{ 8 };
				if (a_geom->worldBound.radius > 500.0f && s_logBudget.fetch_sub(1, std::memory_order_relaxed) > 0)
					logger::info("[MOC][reject] '{}' type={} radius={:.0f} rendererData={} rawVtx={} rawIdx={}",
						a_geom->name.c_str(), static_cast<int>(a_geom->GetType().get()), a_geom->worldBound.radius,
						rendererData != nullptr,
						rendererData && rendererData->rawVertexData, rendererData && rendererData->rawIndexData);
				return false;
			}

			void* key = rendererData;

			std::scoped_lock lock(g_cacheMutex);

			bool haveIdx = false;
			bool haveVtx = false;
			if (auto it = g_indexMap.find(key); it != g_indexMap.end()) {
				a_outIndices = it->second;
				haveIdx = true;
			}
			if (auto it = g_vertMap.find(key); it != g_vertMap.end()) {
				a_outVerts = it->second;
				haveVtx = true;
			}
			if (haveIdx && haveVtx)
				return true;

			auto*               triShape = static_cast<RE::BSTriShape*>(a_geom);
			auto&               triRT = triShape->GetTrishapeRuntimeData();
			const std::uint32_t vertCount = triRT.vertexCount;
			const std::uint32_t triCount = triRT.triangleCount;
			if (triCount < 2 || vertCount == 0)
				return false;

			// The renderer's vertexDesc copy (identical to geomRT.vertexDesc).
			const std::uint32_t stride = GetVertexStride(rendererData->vertexDesc);
			if (stride == 0) {
				static std::atomic<int> s_strideBudget{ 8 };
				if (a_geom->worldBound.radius > 500.0f && s_strideBudget.fetch_sub(1, std::memory_order_relaxed) > 0)
					logger::info("[MOC][reject] '{}' type={} radius={:.0f} STRIDE0",
						a_geom->name.c_str(), static_cast<int>(a_geom->GetType().get()), a_geom->worldBound.radius);
				return false;  // no position stream in slot 0
			}

			if (!haveIdx) {
				IndexPair p;
				p.Count = triCount * 3;
				p.Data = ConvertIndices(reinterpret_cast<const std::uint16_t*>(rendererData->rawIndexData), p.Count);

				// Simplify the base occluder mesh via the selected meshoptimizer algorithm
				// (only above 300 indices). The raw slot-0 buffer IS the float3-position
				// stream meshopt expects; in-place (dst == src) is supported.
				const float* const vpos = reinterpret_cast<const float*>(rendererData->rawVertexData);
				if (SimplifyMode != 0 && p.Count > 300) {
					const std::size_t target = static_cast<std::size_t>(p.Count * SimplifyStrength);
					std::size_t       simplified = p.Count;
					switch (SimplifyMode) {
					case 1:  // quality, topology-preserving (honours Options bitmask)
						simplified = meshopt_simplify(p.Data, p.Data, p.Count, vpos, vertCount, stride,
							target, SimplifyError, SimplifyOptions, nullptr);
						break;
					case 2:  // sloppy, faster, silhouette may move
						simplified = meshopt_simplifySloppy(p.Data, p.Data, p.Count, vpos, vertCount, stride,
							target, SimplifyError, nullptr);
						break;
					case 3:  // prune small disconnected components only (no ratio target)
						simplified = meshopt_simplifyPrune(p.Data, p.Data, p.Count, vpos, vertCount, stride,
							SimplifyError);
						break;
					default:
						break;
					}
					if (simplified >= 3 && simplified <= p.Count)
						p.Count = static_cast<std::uint32_t>(simplified);
				}
				// Runtime occluder LODs: sloppy targets ~30% and ~8% of the BASE
				// indices; result_error reported back for the per-frame projected
				// error budget. Built once per mesh on the builder thread. Independent
				// of the base algorithm -- gated only on BuildRuntimeLODs, so the base
				// mesh can be full-res (mode Off) yet still get distance LODs, or a
				// simplified base can skip LODs entirely.
				if (BuildRuntimeLODs && p.Count > 900) {
					float err1 = 0.0f;
					auto* lod1 = new std::uint32_t[p.Count];
					const std::size_t c1 = meshopt_simplifySloppy(
						lod1, p.Data, p.Count,
						vpos, vertCount, stride,
						static_cast<std::size_t>(p.Count * 0.30f), 2e-2f, &err1);
					if (c1 >= 3 && c1 < p.Count) {
						p.Lod1Data = lod1;
						p.Lod1Count = static_cast<std::uint32_t>(c1);
						p.Lod1ErrRel = err1;
					} else {
						delete[] lod1;
					}
					if (p.Lod1Data) {
						float err2 = 0.0f;
						auto* lod2 = new std::uint32_t[p.Lod1Count];
						const std::size_t c2 = meshopt_simplifySloppy(
							lod2, p.Lod1Data, p.Lod1Count,
							vpos, vertCount, stride,
							static_cast<std::size_t>(p.Count * 0.08f), 8e-2f, &err2);
						if (c2 >= 3 && c2 < p.Lod1Count) {
							p.Lod2Data = lod2;
							p.Lod2Count = static_cast<std::uint32_t>(c2);
							p.Lod2ErrRel = err2;
						} else {
							delete[] lod2;
						}
					}
				}

				p.VertCount = vertCount;
				g_indexMap.insert_or_assign(key, p);
				a_outIndices = p;
			}
			if (!haveVtx) {
				a_outVerts = ConvertVerts(reinterpret_cast<const std::uint8_t*>(rendererData->rawVertexData), vertCount, stride);
				g_vertMap.insert_or_assign(key, a_outVerts);

				// One-shot sanity: model-space positions of the first converted mesh should be
				// modest local coordinates (|v| within a few thousand units), not garbage.
				static std::atomic<bool> s_vertsLogged{ false };
				if (!s_vertsLogged.exchange(true)) {
					logger::info("[MOC][vert] first mesh: stride={} verts={} tris={} v0=({:.1f},{:.1f},{:.1f}) v1=({:.1f},{:.1f},{:.1f})",
						stride, vertCount, triCount,
						a_outVerts[0], a_outVerts[1], a_outVerts[3],
						a_outVerts[4], a_outVerts[5], a_outVerts[7]);
				}
			}
			return true;
		}

		// ---------------------------------------------------------------------
		// Matrix / camera resolution helpers.
		// ---------------------------------------------------------------------
		// Faithful port of Nukem's NiCamera::CalculateViewProjection (itself ported from
		// game code): the view matrix from the camera's world basis (his naming: dir =
		// rotate column 0, up = column 1, right = column 2; translation handled separately
		// via posAdjust), the projection from the camera's NiFrustum. Only valid on a
		// camera with a REAL (aspect-correct) frustum -- i.e. the RENDER camera. The
		// frozen cull camera's frustum is normalized to (-1,1,1,-1) and yields garbage.

		void CalculateViewProjection(RE::NiCamera* a_camera, XMMATRIX& a_view, XMMATRIX& a_proj, XMMATRIX& a_viewProj)
		{
			const RE::NiMatrix3& R = a_camera->world.rotate;
			const RE::NiPoint3   dir{ R.entry[0][0], R.entry[1][0], R.entry[2][0] };    // col 0
			const RE::NiPoint3   up{ R.entry[0][1], R.entry[1][1], R.entry[2][1] };     // col 1
			const RE::NiPoint3   right{ R.entry[0][2], R.entry[1][2], R.entry[2][2] };  // col 2

			a_view.r[0] = XMVectorSet(right.x, up.x, dir.x, 0.0f);
			a_view.r[1] = XMVectorSet(right.y, up.y, dir.y, 0.0f);
			a_view.r[2] = XMVectorSet(right.z, up.z, dir.z, 0.0f);
			a_view.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);

			const RE::NiFrustum& fr = a_camera->GetRuntimeData2().viewFrustum;
			const float          rightLeftDiff = fr.fRight - fr.fLeft;
			const float          rightLeftRatio = -((1.0f / rightLeftDiff) * (fr.fRight + fr.fLeft));
			const float          topBottomDiff = fr.fTop - fr.fBottom;
			const float          topBottomRatio = -((1.0f / topBottomDiff) * (fr.fTop + fr.fBottom));
			const float          invNearFarDiff = 1.0f / (fr.fFar - fr.fNear);

			a_proj.r[0] = _mm_setzero_ps();
			a_proj.r[1] = _mm_setzero_ps();
			a_proj.r[2] = _mm_setzero_ps();
			a_proj.r[3] = _mm_setzero_ps();
			a_proj.r[0].m128_f32[0] = (1.0f / rightLeftDiff) * 2.0f;
			a_proj.r[1].m128_f32[1] = (1.0f / topBottomDiff) * 2.0f;
			if (!fr.bOrtho) {
				a_proj.r[2].m128_f32[0] = rightLeftRatio;
				a_proj.r[2].m128_f32[1] = topBottomRatio;
				a_proj.r[2].m128_f32[2] = invNearFarDiff * fr.fFar;
				a_proj.r[2].m128_f32[3] = 1.0f;
				a_proj.r[3].m128_f32[2] = -((fr.fNear * fr.fFar) * invNearFarDiff);
			} else {
				a_proj.r[2].m128_f32[2] = invNearFarDiff;
				a_proj.r[3].m128_f32[0] = rightLeftRatio;
				a_proj.r[3].m128_f32[1] = topBottomRatio;
				a_proj.r[3].m128_f32[2] = -(invNearFarDiff * fr.fNear);
				a_proj.r[3].m128_f32[3] = 1.0f;
			}

			a_viewProj = XMMatrixMultiply(a_view, a_proj);
		}

		// ---------------------------------------------------------------------
		// Scene-graph helpers (safe indexed child access).
		// ---------------------------------------------------------------------
		RE::NiAVObject* ChildAt(RE::NiNode* a_node, std::uint32_t a_index)
		{
			if (!a_node)
				return nullptr;
			auto& children = a_node->GetChildren();
			if (a_index >= children.capacity())
				return nullptr;
			return children[static_cast<std::uint16_t>(a_index)].get();
		}

		RE::NiNode* ChildNodeAt(RE::NiNode* a_node, std::uint32_t a_index)
		{
			auto* obj = ChildAt(a_node, a_index);
			return obj ? obj->AsNode() : nullptr;
		}

		bool IsLeafAnimNode(RE::NiNode* a_node)
		{
			if (auto* rtti = a_node->GetRTTI())
				return std::string_view(rtti->name) == std::string_view("BSLeafAnimNode");
			return false;
		}

		RE::BSMultiBoundAABB* GetAABBNode(RE::NiAVObject* a_object)
		{
			auto* mbn = netimmerse_cast<RE::BSMultiBoundNode*>(a_object);
			if (!mbn)
				return nullptr;
			auto* mb = mbn->GetRuntimeData().multiBound.get();
			if (!mb || !mb->data)
				return nullptr;
			return netimmerse_cast<RE::BSMultiBoundAABB*>(mb->data.get());
		}

		// ---------------------------------------------------------------------
		// Occluder gather (RenderRecursive / RegisterGeometry, adapted to RE::).
		// ---------------------------------------------------------------------
		void RegisterGeometry(RE::BSGeometry* a_geom)
		{
			// Strictly BSLightingShaderProperty occluders only.
			if (!a_geom->lightingShaderProp_cast())
				return;

			// kTriShape = statics/land; kSubIndexTriShape = terrain-LOD chunks
			// (.btr meshes under LODRoot/LandLOD -- the distant ground at vistas).
			if (const auto t = a_geom->GetType().get();
				t != RE::BSGeometry::Type::kTriShape && t != RE::BSGeometry::Type::kSubIndexTriShape)
				return;

			auto& geomRT = a_geom->GetGeometryRuntimeData();
			auto* rendererData = geomRT.rendererData;
			if (!rendererData || !rendererData->rawIndexData)
				return;

			auto* triShape = static_cast<RE::BSTriShape*>(a_geom);
			if (triShape->GetTrishapeRuntimeData().triangleCount <= 1)
				return;

			const RE::NiPoint3& c = a_geom->worldBound.center;
			const float         dx = c.x - g_posAdjust.x;
			const float         dy = c.y - g_posAdjust.y;
			const float         dz = c.z - g_posAdjust.z;

			GeoEntry entry;
			entry.geometry = a_geom;
			entry.distanceSquared = dx * dx + dy * dy + dz * dz;
			// Projected-size proxy: radius over distance. Selecting the raster budget by
			// CENTER DISTANCE starved huge occluders -- terrain cells have far-away centers
			// even when they fill the near view, so nearby pebbles consumed the budget and
			// the terrain went MISSING from the buffer. Coverage picks what blocks pixels.
			entry.coverageScore = a_geom->worldBound.radius / (sqrtf(entry.distanceSquared) + 1.0f);
			g_geoList.push_back(entry);
		}

		// Coarse frustum + flag pre-cull for the occluder gather. true = skip.
		bool CullObject(RE::NiAVObject* a_object)
		{
			if (!a_object)
				return true;

			const auto flags = a_object->GetFlags();
			const bool alwaysDraw = flags.any(RE::NiAVObject::Flag::kAlwaysDraw);
			if (a_object->GetAppCulled() && !alwaysDraw)
				return true;

			if (auto* aabb = GetAABBNode(a_object)) {
				// Not all objects have valid boundaries (certain global cells).
				if (aabb->size.z > 1.0f) {
					const XMVECTOR center = _mm_sub_ps(_mm_setr_ps(aabb->center.x, aabb->center.y, aabb->center.z, 0.0f), g_posAdjustV);
					const XMVECTOR halfExtents = _mm_setr_ps(aabb->size.x, aabb->size.y, aabb->size.z, 0.0f);
					if (!g_frustum.AABBInFrustum(center, halfExtents))
						return true;
				}
			} else if (a_object->worldBound.radius > 10.0f) {
				const auto&    b = a_object->worldBound;
				const XMVECTOR center = _mm_sub_ps(_mm_setr_ps(b.center.x, b.center.y, b.center.z, b.radius), g_posAdjustV);
				if (!g_frustum.SphereInFrustum(center))
					return true;
			}

			return false;
		}

		void RenderRecursive(RE::NiAVObject* a_object, bool a_firstLevel)
		{
			if (CullObject(a_object))
				return;

			const bool validBounds = a_object->worldBound.radius > 1.0f;
			if (a_firstLevel && validBounds && a_object->worldBound.radius < OccluderFirstLevelMinSize)
				return;

			if (RE::NiNode* node = a_object->AsNode()) {
				// Trees/bushes (BSLeafAnimNode subtrees): descend when TreeOccluders is
				// on -- their OPAQUE parts (trunks, solid branches) are legitimate
				// occluders; every alpha-tested/blended leaf card is rejected by the
				// per-geometry alpha gate below. With the option off, skip the whole
				// subtree (the pre-tree-occluder behavior).
				if (TreeOccluders || !IsLeafAnimNode(node)) {
					auto& children = node->GetChildren();
					for (std::uint16_t i = 0; i < children.capacity(); ++i) {
						if (auto* child = children[i].get())
							RenderRecursive(child, false);
					}
				}
			} else if (RE::BSGeometry* geometry = a_object->AsGeometry()) {
				// One-shot diag: LAND geometry flowing through the walk (type 8/9). Suspect:
				// land world bounds are degenerate -> the size gate drops the ground plane.
				const auto geoType = geometry->GetType().get();
				if (geoType == RE::BSGeometry::Type::kSubIndexTriShape || geoType == RE::BSGeometry::Type::kSubIndexLandTriShape) {
					static std::atomic<int> s_landLog{ 6 };
					if (s_landLog.fetch_sub(1, std::memory_order_relaxed) > 0)
						logger::info("[MOC][land] '{}' type={} radius={:.1f} pos=({:.0f},{:.0f},{:.0f})",
							geometry->name.c_str(), static_cast<int>(geoType), geometry->worldBound.radius,
							geometry->world.translate.x, geometry->world.translate.y, geometry->world.translate.z);
				}
				// Alpha gate: alpha-BLENDED surfaces are translucent and never occlude.
				// Alpha-TESTED textures have holes, so they are excluded by default --
				// but AlphaTestedOccluders opts them in as solid occluders (aggressive:
				// dense foliage blocks almost everything behind it in practice, at the
				// risk of over-culling through sparse cutouts).
				if (auto* alpha = geometry->GetGeometryRuntimeData().alphaProperty.get();
					alpha && (alpha->GetAlphaBlending() || (!AlphaTestedOccluders && alpha->GetAlphaTesting())))
					return;
				if (geometry->worldBound.radius > OccluderMinLeafSize) {
					const float d1 = geometry->world.translate.x - g_posAdjust.x;
					const float d2 = geometry->world.translate.y - g_posAdjust.y;
					if (((d1 * d1) + (d2 * d2)) < (OccluderMaxDistance * OccluderMaxDistance))
						RegisterGeometry(geometry);
				}
			}
		}

		// Terrain occluders, built from the decoded heightmap. The engine's land
		// render meshes keep NO CPU-side raw index copy (rendererData->rawIndexData
		// is null for every land quad in the grid -- measured), so the normal
		// register/cache path can never rasterize terrain. Instead one mesh per
		// cell is generated from LoadedLandData::heights (4 quadrants of 17x17
		// samples, 128-unit grid step) and rasterized every kick. Verts are packed
		// (x, y, 1.0f, z) to match the pool's VertexLayout(16,4,12).
		struct LandOccluderMesh
		{
			std::vector<float>        verts;
			std::vector<unsigned int> indices;
		};
		std::unordered_map<std::uint64_t, LandOccluderMesh> g_landCache;  // builder thread only
		std::vector<const LandOccluderMesh*>                g_landReady;  // filled by gather, drawn next kick

		LandOccluderMesh BuildLandMesh(const RE::TESObjectLAND::LoadedLandData* a_data, float a_worldX, float a_worldY)
		{
			LandOccluderMesh m;
			m.verts.reserve(4u * 81u * 4u);
			m.indices.reserve(4u * 8u * 8u * 6u);
			// heights[] are CELL-LOCAL Z relative to the cell's height midpoint
			// ((min+max)/2, stored at LoadedLandData+0x49C0) -- the engine's own
			// consumers add it back (TESObjectLAND::GetNiPointHeight 0x14025B830).
			// IDA-verified 1.5.97.
			const float zBase = *reinterpret_cast<const float*>(reinterpret_cast<const std::uint8_t*>(a_data) + 0x49C0);
			for (int q = 0; q < 4; ++q) {
				const float qx = a_worldX + static_cast<float>(q & 1) * 2048.0f;
				const float qy = a_worldY + static_cast<float>(q >> 1) * 2048.0f;
				const unsigned int base = static_cast<unsigned int>(m.verts.size() / 4u);
				// Decimated 9x9 grid (every other sample, 256-unit step): the raster
				// cost of full 33x33 land dominated the frame (4-5ms). Each kept
				// vertex takes the MIN over its 2x2 source neighborhood -- lower is
				// farther, so the coarse mesh stays a strictly WEAKER (conservative)
				// occluder; the -16 bias guards LOD morph on top.
				for (int row = 0; row < 9; ++row) {
					for (int col = 0; col < 9; ++col) {
						const int r0 = row * 2, c0 = col * 2;
						float h = a_data->heights[q][r0 * 17 + c0];
						if (c0 + 1 < 17)
							h = std::min(h, a_data->heights[q][r0 * 17 + c0 + 1]);
						if (r0 + 1 < 17) {
							h = std::min(h, a_data->heights[q][(r0 + 1) * 17 + c0]);
							if (c0 + 1 < 17)
								h = std::min(h, a_data->heights[q][(r0 + 1) * 17 + c0 + 1]);
						}
						m.verts.push_back(qx + static_cast<float>(col) * 256.0f);
						m.verts.push_back(qy + static_cast<float>(row) * 256.0f);
						m.verts.push_back(1.0f);
						m.verts.push_back(h + zBase - 16.0f);
					}
				}
				for (unsigned int r = 0; r < 8; ++r) {
					for (unsigned int c = 0; c < 8; ++c) {
						const unsigned int i0 = base + r * 9u + c;
						const unsigned int i1 = i0 + 1u;
						const unsigned int i2 = i0 + 9u;
						const unsigned int i3 = i2 + 1u;
						m.indices.insert(m.indices.end(), { i0, i1, i2, i1, i3, i2 });
					}
				}
			}
			return m;
		}

		// Distant terrain LOD (LODRoot -> "LandLOD"): the loaded-grid heightmap
		// meshes only cover ~10k units; everything past that -- the ground the
		// player actually SEES at vistas/zooms -- is terrain-LOD geometry. No
		// distance cap (that is the point); chunks the engine hides where full
		// cells are loaded (kHidden/AppCulled) are skipped, so LOD and heightmap
		// meshes never fight over the same area.
		void RenderLandLOD(RE::NiAVObject* a_object)
		{
			if (!a_object || a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden) || a_object->GetAppCulled())
				return;
			if (CullObject(a_object))
				return;
			if (auto* node = a_object->AsNode()) {
				auto& kids = node->GetChildren();
				for (std::uint16_t i = 0; i < kids.capacity(); ++i)
					RenderLandLOD(kids[i].get());
			} else if (auto* geom = a_object->AsGeometry()) {
				const auto t = geom->GetType().get();
				if (t != RE::BSGeometry::Type::kSubIndexTriShape && t != RE::BSGeometry::Type::kTriShape)
					return;
				if (geom->worldBound.radius < 500.0f)
					return;  // big terrain chunks only
				// One-shot (CS_MOC_DUMP): do .btr meshes carry CPU raw streams?
				static std::atomic<int> s_lodDiag{ 8 };
				if (s_lodDiag.load(std::memory_order_relaxed) > 0 && s_lodDiag.fetch_sub(1, std::memory_order_relaxed) > 0) {
					const auto& rt = geom->GetGeometryRuntimeData();
					logger::info("[MOC][lodland] '{}' t={} r={:.0f} rd={} rawIdx={}",
						geom->name.c_str(), static_cast<int>(t), geom->worldBound.radius,
						rt.rendererData != nullptr,
						rt.rendererData && rt.rendererData->rawIndexData);
				}
				RegisterGeometry(geom);
			}
		}

		// Walk the ObjectLODRoot -> cell -> {LandNode, StaticNode} hierarchy (Nukem's
		// layout). Heavily null-guarded so a layout mismatch degrades to "no occluders"
		// (safe: an empty depth buffer occludes nothing, so everything stays visible).
		void GatherOccluders()
		{
			RE::NiNode* scene = *g_worldScenegraph;  // "WorldRoot Node" (SceneGraph)

			// DIAG (env-gated CS_MOC_DUMP=1, every ~600 frames): dump the scene-graph
			// structure so the walk can be verified against the live layout (menu,
			// interior and exterior all differ). The debug buffer is black iff this walk
			// misses the cell nodes.
			static const bool s_dumpStructure = [] {
				char buf[8] = {};
				return GetEnvironmentVariableA("CS_MOC_DUMP", buf, sizeof(buf)) && buf[0] == '1';
			}();
			if (scene && s_dumpStructure) {
				auto* ssn = ChildNodeAt(scene, 1);
				auto* gs = RE::BSGraphics::State::GetSingleton();
				static std::atomic<bool> s_ssnCensus{ false };
				if (ssn && gs && !s_ssnCensus.exchange(true)) {
					// Type census of EVERY shadow-scene child subtree: find where LAND
					// geometry (t8/t9) actually hangs -- the cells contain none.
					struct SsnCensus
					{
						int types[16] = {};
						int total = 0;
						void Walk(RE::NiAVObject* o, int depth)
						{
							if (!o || depth > 10)
								return;
							if (auto* g = o->AsGeometry()) {
								const int t = static_cast<int>(g->GetType().get());
								if (t >= 0 && t < 16)
									++types[t];
								++total;
							} else if (auto* n = o->AsNode()) {
								auto& kids = n->GetChildren();
								for (std::uint16_t k = 0; k < kids.capacity(); ++k)
									Walk(kids[k].get(), depth + 1);
							}
						}
					};
					logger::info("[MOC][diag] ssn census: '{}' childCount={}", ssn->name.c_str(), ssn->GetChildren().size());
					for (std::uint16_t i = 0; i < ssn->GetChildren().capacity() && i < 16; ++i) {
						auto* c = ChildAt(ssn, i);
						if (!c)
							continue;
						SsnCensus cen;
						cen.Walk(c, 0);
						std::string typeStr;
						for (int t = 0; t < 16; ++t)
							if (cen.types[t])
								typeStr += fmt::format("t{}={} ", t, cen.types[t]);
						logger::info("[MOC][diag]   ssn[{}]='{}' geoms={} {}", i, c->name.c_str(), cen.total, typeStr);
					}
					// Also the two ObjectLODRoot children the cell walk skips.
					if (auto* olr = ChildNodeAt(ssn, 3)) {
						for (std::uint16_t i = 0; i < 2; ++i)
							if (auto* c = ChildAt(olr, i)) {
								SsnCensus cen;
								cen.Walk(c, 0);
								std::string typeStr;
								for (int t = 0; t < 16; ++t)
									if (cen.types[t])
										typeStr += fmt::format("t{}={} ", t, cen.types[t]);
								logger::info("[MOC][diag]   olr[{}]='{}' geoms={} {}", i, c->name.c_str(), cen.total, typeStr);
							}
					}
				}
				// One-shot: trace the ACTUAL land quads (TES -> cell -> cellLand ->
				// loadedData->geom[4]) -- exact pointers, no name guessing. The parent
				// chain shows where they hang; radius/flags/desc show which filter
				// would drop them from the walk.
				static std::atomic<bool> s_landChain{ false };
				if (!s_landChain.exchange(true)) {
					if (auto* tes = RE::TES::GetSingleton()) {
						int logged = 0;
						auto traceCell = [&](RE::TESObjectCELL* cell) {
							if (!cell || logged >= 12)
								return;
							auto* land = cell->GetRuntimeData().cellLand;
							if (!land || !land->loadedData)
								return;
							for (int q = 0; q < 4 && logged < 12; ++q) {
								auto* g = land->loadedData->geom[q].get();
								if (!g)
									continue;
								std::string chain;
								for (RE::NiNode* pn = g->parent; pn; pn = pn->parent)
									chain += fmt::format(" <- '{}'", pn->name.c_str());
								const auto& rt = g->GetGeometryRuntimeData();
								const int stride = rt.rendererData ? static_cast<int>(GetVertexStride(rt.rendererData->vertexDesc)) : -1;
								logger::info("[MOC][landchain] quad{} '{}' type={} r={:.0f} pos=({:.0f},{:.0f},{:.0f}) stride={} flags={:X} chain:{}",
									q, g->name.c_str(), static_cast<int>(g->GetType().get()), g->worldBound.radius,
									g->world.translate.x, g->world.translate.y, g->world.translate.z,
									stride, g->GetFlags().underlying(), chain);
								++logged;
							}
						};
						if (auto* grid = tes->gridCells)
							for (std::uint32_t gx = 0; gx < grid->length; ++gx)
								for (std::uint32_t gy = 0; gy < grid->length; ++gy)
									traceCell(grid->GetCell(gx, gy));
						if (logged == 0)
							logger::info("[MOC][landchain] no land quads found in the loaded grid");
					}
				}
			}

			if (!scene)
				return;

			RE::NiNode* shadowScene = ChildNodeAt(scene, 1);          // "shadow scene node"
			RE::NiNode* objectLODRoot = ChildNodeAt(shadowScene, 3);  // "ObjectLODRoot"
			if (!objectLODRoot)
				return;

			// Walk-shape tallies for the rate-limited diag line (no logging inside the
			// walk -- it runs on job threads).
			g_cellsSeen = 0;
			g_cellsCulled = 0;


			auto& cells = objectLODRoot->GetChildren();
			for (std::uint16_t i = 2; i < cells.capacity(); ++i) {
				RE::NiNode* cellNode = ChildNodeAt(objectLODRoot, i);
				if (!cellNode)
					continue;
				++g_cellsSeen;
				if (CullObject(cellNode)) {
					++g_cellsCulled;
					continue;
				}

				// One-shot diag: EVERY cell's child census (finds where LAND geometry hangs).
				static std::atomic<int> s_cellCensus{ 30 };
				if (s_dumpStructure && s_cellCensus.fetch_sub(1, std::memory_order_relaxed) > 0) {
					logger::info("[MOC][cell] cell[{}] '{}' kids={}", i, cellNode->name.c_str(), cellNode->GetChildren().size());
					struct Census
					{
						int types[16] = {};
						int total = 0;
						void Walk(RE::NiAVObject* o, int depth)
						{
							if (!o || depth > 8)
								return;
							if (auto* g = o->AsGeometry()) {
								const int t = static_cast<int>(g->GetType().get());
								if (t >= 0 && t < 16)
									++types[t];
								++total;
							} else if (auto* n = o->AsNode()) {
								auto& kids = n->GetChildren();
								for (std::uint16_t k = 0; k < kids.capacity(); ++k)
									Walk(kids[k].get(), depth + 1);
							}
						}
					};
					Census cellCen;
					cellCen.Walk(cellNode, 0);
					std::string cellTypes;
					for (int t = 0; t < 16; ++t)
						if (cellCen.types[t])
							cellTypes += fmt::format("t{}={} ", t, cellCen.types[t]);
					logger::info("[MOC][cell]   TOTAL geoms={} {}", cellCen.total, cellTypes);
				}

				// Walk every child container from index 2 (skipping [0]=ActorNode and
				// [1]=MarkerNode, which are positional in both layouts) EXCEPT DynamicNode
				// (movables must never be occluders -- they'd leave stale depth when they
				// move). Exteriors thus cover LandNode[2]+StaticNode[3] as before; interiors
				// (children unnamed, statics parented under room BSMultiBoundNodes) get
				// their geometry via the room containers. Non-renderable helpers (occlusion
				// planes, portals, collision) are rejected by RenderRecursive's geometry
				// filters and the raster's rendererData/shader-property checks.
				for (std::uint16_t j = 2; j < cellNode->GetChildren().capacity(); ++j) {
					auto* container = ChildNodeAt(cellNode, j);
					if (!container)
						continue;
					if (std::string_view{ container->name.c_str() } == "DynamicNode")
						continue;
					if (CullObject(container))
						continue;
					auto& kids = container->GetChildren();
					for (std::uint16_t k = 0; k < kids.capacity(); ++k) {
						if (auto* child = kids[k].get())
							RenderRecursive(child, true);
					}
				}
			}

			// Land pass: rasterize terrain from heightmap-built meshes (see
			// LandOccluderMesh above). Cell selection = distance gate on the cell
			// center only; NO frustum gate (the cell the camera stands in has its
			// center behind the view and must still occlude the foreground).
			g_landReady.clear();
			if (g_landCache.size() > 512)
				g_landCache.clear();  // travel eviction; rebuilt on demand
			if (auto* tes = RE::TES::GetSingleton()) {
				// land3DAttached is the engine's own "all grid land is attached" flag --
				// the strongest available guard against reading half-built LoadedLandData.
				if (auto* grid = tes->gridCells; grid && grid->land3DAttached) {
					for (std::uint32_t gx = 0; gx < grid->length; ++gx) {
						for (std::uint32_t gy = 0; gy < grid->length; ++gy) {
							auto* cell = grid->GetCell(gx, gy);
							// Fully-attached cells only: during the post-load attach window
							// (after the LoadingMenu closes but before the scene settles)
							// half-built cells carry garbage land pointers -- reading
							// loadedData there crashed the builder (AV, crash-16-11-27).
							if (!cell || !cell->IsAttached())
								continue;
							auto* landForm = cell->GetRuntimeData().cellLand;
							if (!landForm || !landForm->loadedData)
								continue;
							auto* ext = cell->GetCoordinates();
							if (!ext)
								continue;
							const float d1 = (ext->worldX + 2048.0f) - g_posAdjust.x;
							const float d2 = (ext->worldY + 2048.0f) - g_posAdjust.y;
							if (((d1 * d1) + (d2 * d2)) >= (OccluderMaxDistance * OccluderMaxDistance))
								continue;
							const std::uint64_t key =
								(static_cast<std::uint64_t>(static_cast<std::uint32_t>(ext->cellX)) << 32) |
								static_cast<std::uint32_t>(ext->cellY);
							auto it = g_landCache.find(key);
							if (it == g_landCache.end()) {
								// One-shot decode diagnostic: height samples vs the camera and
								// the engine's own extents -- catches wrong scale/offset fast.
								static std::atomic<int> s_hdec{ 4 };
								if (s_hdec.fetch_sub(1, std::memory_order_relaxed) > 0)
									logger::info("[MOC][hdec] cell=({},{}) world=({:.0f},{:.0f}) h[0][0]={:.1f} h[0][144]={:.1f} h[3][288]={:.1f} extents=({:.1f},{:.1f}) camZ={:.1f}",
										ext->cellX, ext->cellY, ext->worldX, ext->worldY,
										landForm->loadedData->heights[0][0], landForm->loadedData->heights[0][144],
										landForm->loadedData->heights[3][288],
										landForm->loadedData->heightExtents.x, landForm->loadedData->heightExtents.y,
										g_posAdjust.z);
								it = g_landCache.emplace(key, BuildLandMesh(landForm->loadedData, ext->worldX, ext->worldY)).first;
							}
							g_landReady.push_back(&it->second);
						}
					}
				}
			}
			// Distant terrain LOD (the ground at vista distances). OFF by default: the
			// coarse LOD mesh is not a conservative occluder (it can rise above the real
			// terrain and over-occlude). Heightmap land above always runs.
			if (TerrainLODOccluders) {
				if (auto* lodRoot = ChildNodeAt(shadowScene, 2)) {  // "LODRoot"
					for (std::uint16_t i = 0; i < lodRoot->GetChildren().capacity(); ++i) {
						auto* child = ChildAt(lodRoot, i);
						if (child && std::string_view{ child->name.c_str() } == "LandLOD")
							RenderLandLOD(child);
					}
				}
			}

		}

		bool GetCachedGeometry(RE::BSGeometry* a_geom, IndexPair& a_outIndices, float*& a_outVerts);
		std::uint32_t RasterizeOccluder(RE::BSGeometry* a_geom, const XMMATRIX& a_viewProj, const RE::NiPoint3& a_posAdjust);

		// Submit the prepared occluder set (statics with LOD selection + land)
		// under an arbitrary view-projection -- used for the main camera and the
		// sun shadow-gather view. Builder thread only.
		std::uint32_t SubmitOccluderPass(const XMMATRIX& a_viewProj, const RE::NiPoint3& a_posAdjust)
		{
			std::uint32_t enqueued = 0;
			// px-per-world-unit at unit distance: proj[1][1] = cot(fovY/2).
			const float pxScale = g_proj.r[1].m128_f32[1] * (static_cast<float>(MOC_HEIGHT) * 0.5f);
			for (auto& e : g_readyList) {
				if (!e.verts || !e.idxData || !e.geometry)
					continue;
				const float          dist = std::sqrt(std::max(e.distanceSquared, 1.0f));
				const std::uint32_t* idxSel = e.idxData;
				std::uint32_t        cntSel = e.idxCount;
				if (e.lod2Data && (e.lod2ErrAbs * pxScale) / dist < 0.5f) {
					idxSel = e.lod2Data;
					cntSel = e.lod2Count;
				} else if (e.lod1Data && (e.lod1ErrAbs * pxScale) / dist < 0.5f) {
					idxSel = e.lod1Data;
					cntSel = e.lod1Count;
				}
				const XMMATRIX worldRel = GetXMFromNiPosAdjust(e.geometry->world, a_posAdjust);
				const XMMATRIX worldViewProj = XMMatrixMultiply(worldRel, a_viewProj);
				g_pool->SetMatrix(reinterpret_cast<const float*>(&worldViewProj));
				g_pool->RenderTriangles(
					e.verts, idxSel,
					static_cast<int>(cntSel / 3),
					e.twoSided ? MaskedOcclusionCulling::BACKFACE_NONE : MaskedOcclusionCulling::BACKFACE_CW,
					MaskedOcclusionCulling::CLIP_PLANE_ALL);
				++enqueued;
			}
			if (!g_landReady.empty()) {
				const XMMATRIX rel = XMMatrixTranslation(-a_posAdjust.x, -a_posAdjust.y, -a_posAdjust.z);
				const XMMATRIX mvp = XMMatrixMultiply(rel, a_viewProj);
				g_pool->SetMatrix(reinterpret_cast<const float*>(&mvp));
				for (const auto* lm : g_landReady) {
					g_pool->RenderTriangles(
						lm->verts.data(), lm->indices.data(),
						static_cast<int>(lm->indices.size() / 3u),
						MaskedOcclusionCulling::BACKFACE_NONE,
						MaskedOcclusionCulling::CLIP_PLANE_ALL);
					++enqueued;
				}
			}
			return enqueued;
		}

		// Builder-thread main: the pool's SINGLE PRODUCER. Each kick: (1) rasterize the
		// list prepared last kick using the CURRENT frame's matrices (published by the
		// claim thread before kicking), (2) gather + select + sort + pre-warm NEXT frame's
		// list. Scene-graph reads race the game by design (Nukem's model) -- the walk is
		// null-guarded throughout and only ever READS.
		void BuilderLoop()
		{
			for (;;) {
				{
					std::unique_lock lk(g_builderMtx);
					g_builderCV.wait(lk, [] { return g_builderKick || g_builderQuit; });
					if (g_builderQuit)
						return;
					g_builderKick = false;
				}
				g_builderBusy.store(true, std::memory_order_release);

				// Live mesh-cache rebuild: a simplification setting changed, so free every
				// cached converted mesh (base + LODs) and drop the prepared/pending lists
				// (they point INTO the freed cache). This kick then rasterizes nothing and
				// re-gathers -- the next kick rebuilds with the new settings. Done here on
				// the builder thread so nothing else is touching the lists.
				if (g_rebuildMeshCache.exchange(false, std::memory_order_acq_rel)) {
					{
						std::scoped_lock lock(g_cacheMutex);
						for (auto& [key, verts] : g_vertMap)
							delete[] verts;
						for (auto& [key, idx] : g_indexMap)
							FreeIndexPair(idx);
						g_vertMap.clear();
						g_indexMap.clear();
					}
					g_readyList.clear();
					g_geoList.clear();
					logger::info("[MOC] occluder mesh cache invalidated; rebuilding with new simplification settings");
				}

				const auto t0 = std::chrono::high_resolution_clock::now();

				// Phase 1: rasterize last kick's prepared list. All pool calls live here.
				const auto rasterT0 = std::chrono::high_resolution_clock::now();
				static bool s_workersAwake = false;
				if (!g_hotWorkers || !s_workersAwake) {
					g_pool->WakeThreads();
					s_workersAwake = true;
				}
				g_pool->ClearBuffer();
				if (EnableOccluderRendering) {
					// No triangle budget: the producer is this async builder, so job-queue
					// backpressure only stretches this thread's schedule -- the game never
					// waits. A complete buffer (no missing floors/walls) culls strictly
					// better; partial fills mid-frame are conservative by contract.
					std::uint32_t enqueued = 0;
					enqueued = SubmitOccluderPass(g_viewProj, g_posAdjust);
					g_lastOccluderCount = enqueued;
				} else {
					g_lastOccluderCount = 0;
				}
				// TRUE drain before parking: SuspendThreads only SETS A FLAG and
				// returns immediately -- it never waits for outstanding jobs. Without
				// Flush(): (a) g_rasterFrame was published before the raster actually
				// finished, so tests still ran against an incomplete buffer; (b) the
				// next kick's chunk recycling freed memory that undrained jobs still
				// referenced (AV in GatherTransformClip, garbage indices); (c) the
				// raster timing measured enqueue only. Flush waits for pipeline-empty
				// with the workers awake, then the park + publish are both safe.
				g_pool->Flush();
				g_lastRasterMs = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - rasterT0).count();
				// MAIN raster complete: release the waiting main-view tests. MOC
				// never rasterizes a shadow/sun view -- shadow culling is disabled.
				g_rasterFrame.store(g_kickFrame.load(std::memory_order_acquire), std::memory_order_release);

				// ASYNC TEST: the buffer is complete, so test last frame's candidate
				// snapshot against it and publish the verdict table here on the builder
				// thread -- off the scene-list cull barrier entirely. No-op unless enabled.
				RunAsyncTest();

				if (!g_hotWorkers)
					g_pool->SuspendThreads();

				// Publish a debug-view snapshot of the now-complete buffer (menu open).
				if (g_debugViewFrames.load(std::memory_order_relaxed) > 0) {
					g_debugViewFrames.fetch_sub(1, std::memory_order_relaxed);
					std::scoped_lock lk2(g_debugSnapMtx);
					g_debugSnapshot.resize(static_cast<std::size_t>(MOC_WIDTH) * MOC_HEIGHT);
					g_moc->ComputePixelDepthBuffer(g_debugSnapshot.data(), false);
					g_debugSnapValid = true;
				}

				// Phase 2: gather NEXT frame's candidates. CACHED: the occluder
				// GEOMETRY set is stable frame to frame (large statics); only the
				// view matrices change, and those are applied at RASTER time, not
				// here. So re-walk the scene graph only every kGatherPeriod kicks or
				// when the camera has moved past kGatherMoveSq -- otherwise reuse the
				// prepared list (its cached vert/index pointers persist). Conservative
				// when stale: a just-appeared occluder is missing for a few frames =
				// slightly less culling, never a wrong cull. Cuts ~1.3ms of builder
				// CPU on the reused frames.
				constexpr std::uint32_t kGatherPeriod = 8;
				constexpr float         kGatherMoveSq = 512.0f * 512.0f;
				static std::uint32_t    s_sinceGather = kGatherPeriod;
				static RE::NiPoint3     s_lastGatherPos{ 1e30f, 1e30f, 1e30f };
				const float             mdx = g_posAdjust.x - s_lastGatherPos.x;
				const float             mdy = g_posAdjust.y - s_lastGatherPos.y;
				const float             mdz = g_posAdjust.z - s_lastGatherPos.z;
				const bool              needGather = (++s_sinceGather >= kGatherPeriod) ||
				                        ((mdx * mdx + mdy * mdy + mdz * mdz) > kGatherMoveSq) ||
				                        g_readyList.empty();
				if (!needGather) {
					g_lastGatherMs = 0.0;
				} else {
				s_sinceGather = 0;
				s_lastGatherPos = g_posAdjust;
				g_geoList.clear();
				GatherOccluders();

				const std::size_t budget = std::min<std::size_t>(g_geoList.size(), MaxOccludersPerFrame);
				if (budget < g_geoList.size())
					// Pointer tiebreakers: with a budget below the candidate count, equal
					// scores must not let the SELECTED SUBSET vary frame to frame (an
					// oscillating occluder set oscillates boundary verdicts).
					std::nth_element(g_geoList.begin(), g_geoList.begin() + budget, g_geoList.end(),
						[](const GeoEntry& a, const GeoEntry& b) {
							if (a.coverageScore != b.coverageScore)
								return a.coverageScore > b.coverageScore;
							return a.geometry < b.geometry;
						});
				std::sort(g_geoList.begin(), g_geoList.begin() + budget,
					[](const GeoEntry& a, const GeoEntry& b) {
						if (a.distanceSquared != b.distanceSquared)
							return a.distanceSquared < b.distanceSquared;
						return a.geometry < b.geometry;
					});
				g_geoList.resize(budget);

				// Pre-warm the conversion cache so the claim thread's enqueue never pays
				// for vertex/index conversion or meshopt simplification.
				for (auto& e : g_geoList) {
					IndexPair idx;
					float*    verts = nullptr;
					if (e.geometry && GetCachedGeometry(e.geometry, idx, verts) && verts && idx.Data) {
						e.idxData = idx.Data;
						e.idxCount = idx.Count;
						e.verts = verts;
						e.radius = e.geometry->worldBound.radius;
						e.lod1Data = idx.Lod1Data;
						e.lod1Count = idx.Lod1Count;
						e.lod1ErrAbs = idx.Lod1ErrRel * e.radius;
						e.lod2Data = idx.Lod2Data;
						e.lod2Count = idx.Lod2Count;
						e.lod2ErrAbs = idx.Lod2ErrRel * e.radius;
						auto* sp = e.geometry->lightingShaderProp_cast();
						e.twoSided = sp && sp->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
					}
				}

				g_lastGatherMs = std::chrono::duration<double, std::milli>(
					std::chrono::high_resolution_clock::now() - t0)
				                     .count();
				} // needGather

				if (needGather)
					g_readyList.swap(g_geoList);  // builder-owned; no other consumer
				g_builderBusy.store(false, std::memory_order_release);
			}
		}

		std::uint32_t RasterizeOccluder(RE::BSGeometry* a_geom, const XMMATRIX& a_viewProj, const RE::NiPoint3& a_posAdjust)
		{
			IndexPair indices;
			float*    verts = nullptr;
			if (!a_geom || !GetCachedGeometry(a_geom, indices, verts) || !verts || !indices.Data)
				return 0;

			auto*      sp = a_geom->lightingShaderProp_cast();
			const bool twoSided = sp && sp->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);

			const XMMATRIX worldRel = GetXMFromNiPosAdjust(a_geom->world, a_posAdjust);
			const XMMATRIX worldViewProj = XMMatrixMultiply(worldRel, a_viewProj);

			// Per-mesh submission, transform on the WORKERS (SetMatrix copies into
			// per-job state; cached verts/indices are persistent = keep-alive safe).
			// CLIP_PLANE_ALL: near-crossing triangles must clip, not drop.
			g_pool->SetMatrix(reinterpret_cast<const float*>(&worldViewProj));
			g_pool->RenderTriangles(
				verts,
				indices.Data,
				static_cast<int>(indices.Count / 3),
				twoSided ? MaskedOcclusionCulling::BACKFACE_NONE : MaskedOcclusionCulling::BACKFACE_CW,
				MaskedOcclusionCulling::CLIP_PLANE_ALL);
			return indices.Count / 3;
		}
	}  // namespace

	// -------------------------------------------------------------------------
	// Public API.
	// -------------------------------------------------------------------------
	void Init()
	{
		if (g_init)
			return;

		// Request AVX2 (the shipping build targets /arch:AVX2). MOC caps the request
		// to the best implementation the CPU actually supports.
		auto simd = MaskedOcclusionCulling::AVX2;
		{
			// CS_MOC_SIMD=sse2|sse41|avx2: probe for AVX<->SSE transition penalties.
			// SkyrimSE.exe is non-AVX (legacy SSE encodings); our AVX2 raster runs
			// 256-bit ops, and TestRect executes ON ENGINE CULL THREADS -- dirty
			// upper-YMM state leaking across the detour would put the engine's SSE
			// code into the transition-penalty regime. SSE41 removes all 256-bit
			// state from engine threads (raster itself ~2x slower per pixel).
			char simdBuf[8] = {};
			if (GetEnvironmentVariableA("CS_MOC_SIMD", simdBuf, sizeof(simdBuf)) && simdBuf[0]) {
				if (simdBuf[0] == 's' && simdBuf[3] == '4')
					simd = MaskedOcclusionCulling::SSE41;
				else if (simdBuf[0] == 's')
					simd = MaskedOcclusionCulling::SSE2;
				logger::info("[MOC] SIMD override: {}", simdBuf);
			}
		}
		g_moc = MaskedOcclusionCulling::Create(simd);
		if (!g_moc) {
			logger::warn("[MOC] MaskedOcclusionCulling::Create failed; occlusion culling disabled");
			return;
		}
		// CS_MOC_RES=WxH (e.g. 1280x720): buffer-resolution override for A/B sweeps.
		{
			char resBuf[24] = {};
			if (GetEnvironmentVariableA("CS_MOC_RES", resBuf, sizeof(resBuf)) && resBuf[0]) {
				unsigned int w = 0, h = 0;
				if (sscanf_s(resBuf, "%ux%u", &w, &h) == 2 && w >= 256 && h >= 144 && w <= 1920 && h <= 1080) {
					MOC_WIDTH = w & ~7u;   // multiple of 8
					MOC_HEIGHT = h & ~3u;  // multiple of 4
				}
			}
		}
		{
			char nwBuf[8] = {};
			g_noWaitProbe = GetEnvironmentVariableA("CS_MOC_NO_WAIT", nwBuf, sizeof(nwBuf)) && nwBuf[0] == '1';
			if (g_noWaitProbe)
				logger::warn("[MOC] NO_WAIT probe active: tests never wait (perf probe, image may flicker)");
		}
		{
			// CS_MOC_HOT_WORKERS=1: keep the pool workers awake across kicks
			// (skip the per-kick Suspend/Wake) -- probes whether worker WAKE
			// latency is the invariant ~2.3ms raster-completion overhead.
			char hwBuf[8] = {};
			g_hotWorkers = GetEnvironmentVariableA("CS_MOC_HOT_WORKERS", hwBuf, sizeof(hwBuf)) && hwBuf[0] == '1';
			if (g_hotWorkers)
				logger::info("[MOC] HOT_WORKERS probe: workers stay awake across kicks");
		}
		g_moc->SetResolution(MOC_WIDTH, MOC_HEIGHT);
		g_moc->ClearBuffer();

		// One-frame-behind ping-pong (CS_MOC_PREV_FRAME=1): a SECOND instance so the builder rasterizes
		// this frame's occluders into the BACK buffer while the cull tests the PREVIOUS frame's completed
		// FRONT buffer -- WaitForRasterComplete never spins. NOTE (fast version): tests project with THIS
		// frame's camera against last frame's buffer, so a fast camera move can misalign by a few low-res
		// pixels (TestRect's conservative margin absorbs small motion). If that pops, matched per-buffer
		// matrices are the fix. When off, g_mocFront stays null and everything tests g_moc as before.
		{
			char pfBuf[8] = {};
			g_prevFrameOcclusion = GetEnvironmentVariableA("CS_MOC_PREV_FRAME", pfBuf, sizeof(pfBuf)) && pfBuf[0] == '1';
			char ftBuf[8] = {};
			g_mocFlagTest = GetEnvironmentVariableA("CS_MOC_FLAGTEST", ftBuf, sizeof(ftBuf)) && ftBuf[0] == '1';
			char hzBuf[8] = {};
			g_hizMode = GetEnvironmentVariableA("CS_MOC_HIZ", hzBuf, sizeof(hzBuf)) && hzBuf[0] == '1';
			if (char hbBuf[32] = {}; GetEnvironmentVariableA("CS_MOC_HIZ_BIAS", hbBuf, sizeof(hbBuf)) && hbBuf[0])
				g_hizBias = static_cast<float>(atof(hbBuf));
			if (g_hizMode)
				logger::info("[MOC][HiZ] GPU Hi-Z occlusion enabled (bias={})", g_hizBias);
		}
		// Create the ping-pong spare whenever one-frame-behind may be used (boot env or the runtime
		// flag-test A/B). Cheap: one extra depth buffer.
		if (g_prevFrameOcclusion || g_mocFlagTest) {
			g_mocSpare = MaskedOcclusionCulling::Create(simd);
			if (g_mocSpare) {
				g_mocSpare->SetResolution(MOC_WIDTH, MOC_HEIGHT);
				g_mocSpare->ClearBuffer();
				logger::info("[MOC] one-frame-behind buffer ready (prevFrame={} flagTest={})", g_prevFrameOcclusion, g_mocFlagTest);
			} else {
				g_prevFrameOcclusion = false;
				logger::warn("[MOC] one-frame-behind: spare Create failed; using synchronous wait");
			}
		}

		// Intel's CullingThreadpool: occluder rasterization is queued by the (single)
		// build thread and executed by worker threads binning the screen. Bins must be
		// tile-aligned and >= thread count: 4x4 bins of 128x72 over 512x288. maxJobs=32
		// is Intel's recommended queue depth. Workers are woken per build and suspended
		// right after the flush, so the pool costs nothing outside the build window.
		// Worker count = ALL hardware threads, capped at the pool's 16-bin limit
		// (4x4 bins of 128x72 over 512x288; more workers than bins just idle). The
		// raster must win the frame-start scheduler race against the engine's job
		// threads or every cull job stalls on its completion, so use the whole CPU.
		// Deep job queue (512): with ~650 per-mesh submissions the default 32-deep
		// queue blocked the PRODUCER on CanWrite() every 32 jobs -- 512 lets the
		// builder enqueue the whole frame without stalling while the workers
		// transform+bin+rasterize in parallel (transform belongs on the workers,
		// as in the Intel sample -- not on the single builder thread).
		unsigned int threads = std::min(std::max(4u, std::thread::hardware_concurrency()), 16u);
		// Snapshot thread ids before/after pool creation to identify the workers
		// (CullingThreadpool exposes no handles) and raise them ABOVE_NORMAL: the
		// raster is the frame's critical path in the standard model -- every cull
		// job waits on its completion -- yet at frame start the CPU is saturated
		// with engine job threads, so normal-priority workers get time-sliced and
		// completion latency sat at an invariant ~2.3ms regardless of workload
		// (falsified levers: resolution, tri count, worker count, job count, SIMD
		// backend, hot-vs-suspended workers). They only hold work ~1-2ms per frame
		// and park after, so the elevated priority cannot starve the engine.
		// CS_MOC_WORKER_PRIO=0 disables for A/B.
		std::vector<DWORD> preThreads;
		{
			HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
			if (snap != INVALID_HANDLE_VALUE) {
				THREADENTRY32 te{ sizeof(te) };
				const DWORD pid = GetCurrentProcessId();
				for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
					if (te.th32OwnerProcessID == pid)
						preThreads.push_back(te.th32ThreadID);
				CloseHandle(snap);
			}
		}
		g_pool = new CullingThreadpool(threads, 4, 4, 512);
		{
			char prioBuf[8] = {};
			const bool boost = !(GetEnvironmentVariableA("CS_MOC_WORKER_PRIO", prioBuf, sizeof(prioBuf)) && prioBuf[0] == '0');
			if (boost) {
				HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
				if (snap != INVALID_HANDLE_VALUE) {
					THREADENTRY32 te{ sizeof(te) };
					const DWORD pid = GetCurrentProcessId();
					int boosted = 0;
					for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
						if (te.th32OwnerProcessID != pid)
							continue;
						if (std::find(preThreads.begin(), preThreads.end(), te.th32ThreadID) != preThreads.end())
							continue;
						if (HANDLE h = OpenThread(THREAD_SET_INFORMATION, FALSE, te.th32ThreadID)) {
							SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
							CloseHandle(h);
							++boosted;
						}
					}
					CloseHandle(snap);
					logger::info("[MOC] raster workers boosted to ABOVE_NORMAL: {}", boosted);
				}
			}
		}
		g_pool->SetBuffer(g_moc);
		g_pool->SetVertexLayout(MaskedOcclusionCulling::VertexLayout(16, 4, 12));
		g_pool->SuspendThreads();

		// Drop the raster workers below normal priority: in CPU-bound scenes they must
		// yield to the engine's own job threads or the raster wins cost more than the
		// culling saves. (The pool spawned `threads` workers followed by nothing else on
		// this process yet at PostPostLoad -- identify them by enumeration snapshot.)
		// CullingThreadpool offers no handle access; adjust via the builder-side knob
		// instead: fewer workers is the honest lever, so the default is now 2.

		g_builderQuit = false;
		g_builder = std::thread(BuilderLoop);
		SetThreadPriority(g_builder.native_handle(), THREAD_PRIORITY_ABOVE_NORMAL);

		g_init = true;
		logger::info("[MOC] initialized {}x{} rasterThreads={}", MOC_WIDTH, MOC_HEIGHT, threads);
	}

	// Create the debug texture lazily: MOC::Init runs at PostPostLoad, BEFORE the game
	// creates the D3D11 device, so globals::d3d::device is null there. This is called
	// from UpdateDebugView (menu open) by which point the device exists.
	void EnsureDebugTexture()
	{
		if (g_debugSRV)
			return;
		auto* device = globals::d3d::device;
		if (!device)
			return;
		D3D11_TEXTURE2D_DESC td{};
		td.Width = MOC_WIDTH;
		td.Height = MOC_HEIGHT;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DYNAMIC;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, g_debugTex.put())))
			device->CreateShaderResourceView(g_debugTex.get(), nullptr, g_debugSRV.put());
		g_debugDepth.resize(static_cast<std::size_t>(MOC_WIDTH) * MOC_HEIGHT);
	}

	// Rasterize the current MOC depth buffer into the debug texture as grayscale
	// (near = bright). Called from the settings UI; returns the SRV to ImGui::Image.
	void UpdateDebugView()
	{
		if (!g_init || !g_moc)
			return;
		EnsureDebugTexture();
		if (!g_debugTex || !g_debugSRV)
			return;
		auto* ctx = globals::d3d::context;
		if (!ctx)
			return;

		// Ask the builder to keep publishing complete-buffer snapshots (~1s worth per
		// request) and display the latest one. Never read the live buffer from this
		// thread: the async builder clears+refills it mid-frame, so a UI-time read
		// catches a random fill state (flicker; late-rasterized far occluders like
		// terrain mostly missing).
		g_debugViewFrames.store(60, std::memory_order_relaxed);
		{
			std::scoped_lock lk(g_debugSnapMtx);
			if (!g_debugSnapValid)
				return;  // no complete snapshot yet (first frames after menu open)
			g_debugDepth = g_debugSnapshot;
		}

		// MOC returns FLT_MAX (or ~0) for uncovered pixels; normalize only over real occluders
		// (0 < d < kBackground) so their silhouettes are visible (see DumpDepthImage).
		constexpr float kBackground = 1e30f;
		float           mn = FLT_MAX, mx = 0.0f;
		for (float d : g_debugDepth) {
			if (d > 0.0f && d < kBackground) {
				mn = std::min(mn, d);
				mx = std::max(mx, d);
			}
		}
		const float range = (mx > mn) ? (mx - mn) : 1.0f;

		D3D11_MAPPED_SUBRESOURCE ms{};
		if (FAILED(ctx->Map(g_debugTex.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
			return;
		for (unsigned int y = 0; y < MOC_HEIGHT; ++y) {
			auto* row = static_cast<std::uint8_t*>(ms.pData) + static_cast<std::size_t>(y) * ms.RowPitch;
			for (unsigned int x = 0; x < MOC_WIDTH; ++x) {
				const float        d = g_debugDepth[static_cast<std::size_t>(y) * MOC_WIDTH + x];
				const bool         occluder = d > 0.0f && d < kBackground;
				const std::uint8_t g = occluder ? static_cast<std::uint8_t>(40.0f + 215.0f * ((d - mn) / range)) : 0;
				row[x * 4 + 0] = g;
				row[x * 4 + 1] = g;
				row[x * 4 + 2] = g;
				row[x * 4 + 3] = 255;
			}
		}
		ctx->Unmap(g_debugTex.get(), 0);
	}

	void* GetDebugSRV()
	{
		return g_debugSRV.get();
	}

	// DIAG: save the current depth buffer as a grayscale PNG so its content can be verified
	// directly (Read the image) without opening the ImGui menu. Same data + mapping the UI shows.
	void DumpDepthImage()
	{
		if (!g_init || !g_moc)
			return;
		const std::size_t n = static_cast<std::size_t>(MOC_WIDTH) * MOC_HEIGHT;
		if (g_debugDepth.size() != n)
			g_debugDepth.resize(n);
		g_moc->ComputePixelDepthBuffer(g_debugDepth.data(), false);  // flipY=false is empirically top-down (matches Nukem)

		// MOC returns FLT_MAX (or ~0) for uncovered pixels; only 0 < d < kBackground is a real
		// rasterized occluder. Normalize over those so the silhouettes are visible.
		constexpr float kBackground = 1e30f;
		float           mn = FLT_MAX, mx = 0.0f;
		std::size_t     nonzero = 0;
		for (float d : g_debugDepth) {
			if (d > 0.0f && d < kBackground) {
				mn = std::min(mn, d);
				mx = std::max(mx, d);
				++nonzero;
			}
		}
		const float range = (mx > mn) ? (mx - mn) : 1.0f;

		static std::vector<std::uint8_t> rgba;
		rgba.resize(n * 4);
		for (std::size_t i = 0; i < n; ++i) {
			const float        d = g_debugDepth[i];
			const bool         occluder = d > 0.0f && d < kBackground;
			// Occluders: bright→near via normalized depth (min gray 40 so they're always visible).
			const std::uint8_t g = occluder ? static_cast<std::uint8_t>(40.0f + 215.0f * ((d - mn) / range)) : 0;
			rgba[i * 4 + 0] = g;
			rgba[i * 4 + 1] = g;
			rgba[i * 4 + 2] = g;
			rgba[i * 4 + 3] = 255;
		}

		const DirectX::Image img{ MOC_WIDTH, MOC_HEIGHT, DXGI_FORMAT_R8G8B8A8_UNORM,
			static_cast<std::size_t>(MOC_WIDTH) * 4, n * 4, rgba.data() };
		const HRESULT hr = DirectX::SaveToWICFile(img, DirectX::WIC_FLAGS_NONE,
			DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), L"F:\\claudetmp\\moc_depth.png");
		logger::info("[MOC][depth] saved moc_depth.png nonzero={}/{} range=[{:.4g},{:.4g}] hr=0x{:08X}",
			nonzero, n, mn, mx, static_cast<std::uint32_t>(hr));

		// Raw float dump for offline numeric comparison against the game depth buffer:
		// header = uint32 W, uint32 H, then the 16 floats of g_proj (row-vector layout, to
		// recover near/far), then W*H floats of MOC depth (= 1/viewW; FLT_MAX where empty),
		// row-major top-down (matches the flipY'd PNG).
		if (FILE* f = nullptr; _wfopen_s(&f, L"F:\\claudetmp\\moc_depth.bin", L"wb") == 0 && f) {
			const std::uint32_t wh[2] = { MOC_WIDTH, MOC_HEIGHT };
			fwrite(wh, sizeof(wh), 1, f);
			XMFLOAT4X4 pm;
			XMStoreFloat4x4(&pm, g_proj);
			fwrite(&pm, sizeof(pm), 1, f);
			fwrite(g_debugDepth.data(), sizeof(float), n, f);
			fclose(f);
		}
	}

	void Shutdown()
	{
		std::scoped_lock lock(g_cacheMutex);
		for (auto& [key, verts] : g_vertMap)
			delete[] verts;
		for (auto& [key, idx] : g_indexMap)
			FreeIndexPair(idx);
		g_vertMap.clear();
		g_indexMap.clear();
		g_geoList.clear();

		// Stop the builder before tearing anything down (it walks the scene graph and
		// touches the conversion cache).
		if (g_builder.joinable()) {
			{
				std::scoped_lock lk(g_builderMtx);
				g_builderQuit = true;
			}
			g_builderCV.notify_one();
			g_builder.join();
		}

		// The pool references the MOC instances and joins its workers in the destructor -- destroy it first.
		delete g_pool;
		g_pool = nullptr;

		if (g_moc) {
			MaskedOcclusionCulling::Destroy(g_moc);
			g_moc = nullptr;
		}
		if (g_mocSpare) {
			MaskedOcclusionCulling::Destroy(g_mocSpare);
			g_mocSpare = nullptr;
		}
		g_mocFront.store(nullptr, std::memory_order_release);
		g_hizFront.store(nullptr, std::memory_order_release);
		g_hizCS = nullptr;
		g_hizGridUAV = nullptr;
		g_hizGridTex = nullptr;
		for (auto& slot : g_hizRing) {
			slot.staging = nullptr;
			slot.pending = false;
		}
		g_hizReady = false;
		g_hizInitTried = false;
		g_init = false;
	}

	bool IsInitialized()
	{
		return g_init && g_moc != nullptr;
	}

	RE::NiCamera* GetMainCamera()
	{
		// The engine's own main-render camera slot: Main::spWorldRoot (REL::ID 517006, ==
		// g_worldScenegraph) is a BSSceneGraph whose runtime camera (+0x128 SE) is the exact
		// pointer DrawWorld_PreRender (0x1405B1860 in 1.5.97) loads for CacheCameraData /
		// SetCameraData and every main-scene cull. NOT a scene-graph child walk, which can
		// find a different (stale) camera under CameraRoot.
		RE::NiNode* root = *g_worldScenegraph;
		if (!root)
			return nullptr;
		return static_cast<RE::BSSceneGraph*>(root)->GetRuntimeData().camera.get();
	}

	RE::NiCamera* GetCullCamera()
	{
		return *g_cullCamera;
	}

	void QuiesceBuilder()
	{
		// Refuse-then-drain: the claim path already refuses kicks while the loading menu
		// is open; here we wait out any gather already in flight. Bounded (~200ms) so a
		// wedged walk can't hang the UI thread -- by then the walk has either finished or
		// crashed anyway.
		for (int i = 0; i < 2000 && g_builderBusy.load(std::memory_order_acquire); ++i)
			std::this_thread::sleep_for(std::chrono::microseconds(100));
	}

	void RemoveCachedGeometry(void* a_rendererData)
	{
		IndexPair idx{};
		float*    verts = nullptr;
		{
			std::scoped_lock lock(g_cacheMutex);
			if (auto it = g_indexMap.find(a_rendererData); it != g_indexMap.end()) {
				idx = it->second;
				g_indexMap.erase(it);
			}
			if (auto it = g_vertMap.find(a_rendererData); it != g_vertMap.end()) {
				verts = it->second;
				g_vertMap.erase(it);
			}
		}
		FreeIndexPair(idx);  // base + both LODs (the old path leaked the LODs)
		delete[] verts;
	}

	void RebuildOccluderMeshCache()
	{
		// Ask the builder to invalidate + rebuild its converted-mesh cache on its next
		// kick (see the flag handler in BuilderLoop). Cheap and thread-safe -- just a flag.
		g_rebuildMeshCache.store(true, std::memory_order_release);
	}

	// Returns true iff this cull pass is the MAIN world view and the occluder buffer is
	// current for the frame -- i.e. the caller should enable per-object occlusion testing.
	// All other passes (shadow cascades, water/cubemap reflections, first-person, sky)
	// return false and are left untouched.
	bool BuildOccluders(RE::NiCamera* a_camera)
	{
		if (!g_init || !g_moc)
			return false;

		// MAIN-pass gate by pointer identity with the engine's two main-scene cameras.
		// The main culls carry one of TWO NiCamera objects depending on phase (verified by
		// hook counters): the BuildSceneLists CELL culls get the RENDER camera directly
		// (DrawWorld_PreRender seeds the job cull processes with it), while the MainAccum /
		// depth-prepass / world subtree culls get the frozen CULL camera (REL::ID 528062,
		// normalized frustum). Auxiliary passes (water reflection, first-person, sky,
		// shadows, cubemap) each carry their own camera and match neither.
		RE::NiCamera* renderCam = GetMainCamera();
		if (!a_camera || !renderCam)
			return false;
		if (a_camera != renderCam && a_camera != *g_cullCamera)
			return false;

		// Quiesce during loads: the BUILDER thread walks the scene graph, and a scene
		// teardown (coc / fast travel / load) mid-walk is a use-after-free (crashed on a
		// worldspace transition). The loading screen covers the teardown window; skipping
		// builds there costs nothing (nothing worth culling renders) and starves the
		// builder before the graph is torn down.
		// Feature toggled off => no builder work at all (gather + raster used to
		// keep running with testing disabled, contaminating every OFF baseline by
		// ~1ms/frame of MOC infrastructure and costing real users who toggle off).
		if (!EnableOcclusionTesting)
			return false;

		if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
			g_settleUntilKick.store(g_kickCounter.load(std::memory_order_relaxed) + 240, std::memory_order_relaxed);
			return false;
		}
		// Post-load settle window: the LoadingMenu closes BEFORE the scene finishes
		// attaching (cells stream in for a couple of seconds); a gather in that window
		// walks half-built cells -- two load-time crashes (16:11 AV in the land pass,
		// 16:20 silent death mid cell-census). ~240 kicks =~ 1-2s of no culling after
		// every load, invisible to the player.
		if (g_kickCounter.fetch_add(1, std::memory_order_relaxed) < g_settleUntilKick.load(std::memory_order_relaxed))
			return false;

		// The matrices always come from the RENDER camera: the cull camera's frustum is
		// normalized to (-1,1,1,-1), and the engine's per-pass cameraData is still stale
		// from the previous frame during the earliest main cull (BuildSceneLists runs as a
		// job BEFORE SetCameraData). The render camera's world transform + real frustum
		// are already updated for this frame -- DrawWorld itself reads them at the same
		// point. Same recipe as Nukem.

		// Once per frame: CAS-claim the frame so exactly ONE of the concurrent main-scene
		// culls builds. Losers report whether a completed build for this frame is already
		// published -- if the winner is still mid-build they return false and their pass
		// simply isn't occlusion-tested (conservative).
		auto*               gfxState = RE::BSGraphics::State::GetSingleton();
		const std::uint32_t frame = gfxState ? gfxState->frameCount : 0;
		std::uint32_t       claimed = g_buildClaim.load(std::memory_order_relaxed);
		if (claimed == frame || !g_buildClaim.compare_exchange_strong(claimed, frame, std::memory_order_relaxed))
			return g_buildDone.load(std::memory_order_acquire) == frame;
		g_buildStart = std::chrono::high_resolution_clock::now();

		// Hi-Z mode: no occluder gather, no raster kick -- the game's own depth IS the occluder
		// set (built + read back on the render thread in HiZPrepass). Load the published
		// snapshot's matrices into the shared test globals so every projection in TestAABB /
		// TestSphereBound matches the buffer it tests against, then declare the frame built.
		// No snapshot yet (first frames / readback still in flight) => no occlusion this frame.
		if (g_hizMode) {
			const HiZSnapshot* snap = g_hizFront.load(std::memory_order_acquire);
			if (!snap)
				return false;
			g_view = snap->view;
			g_viewProj = snap->viewProj;
			g_posAdjust = snap->posAdjust;
			g_posAdjustV = snap->posAdjustV;
			g_frustum.CreateFromViewProjMatrix(g_viewProj);
			if ((frame % 120u) == 0u)
				logger::info("[MOC][HiZ] frame={} grid={}x{} tested={} culled={}",
					frame, snap->gridW, snap->gridH, g_tested.load(), g_culled.load());
			g_buildDone.store(frame, std::memory_order_release);
			return true;
		}

		CalculateViewProjection(renderCam, g_view, g_proj, g_viewProj);
		const RE::NiPoint3 posAdj = renderCam->world.translate;
		g_posAdjust = posAdj;
		g_posAdjustV = _mm_setr_ps(posAdj.x, posAdj.y, posAdj.z, 0.0f);

		// Coarse occluder-gather frustum. Sphere test uses the view-proj clip planes; AABB test
		// uses the reconstructed frustum. The scalar derivation below (X-based fov, proj-ratio
		// aspect, and the deliberately odd near/far extraction) is VERBATIM Nukem -- his values
		// are unusual but shipped and conservative; do not "clean up". Position is zero because
		// all our tests feed camera-relative centers (posAdjust already subtracted).
		g_frustum.CreateFromViewProjMatrix(g_viewProj);

		// Identify the sun shadow-gather camera (dirLight = ShadowSceneNode+0x210,
		// gather cam +0x578; IDA 2026-07-11) so the small-caster SHADOW cull runs on
		// that pass and nowhere else. No matrices/buffer -- the cull is pure size math,
		// NOT a MOC occlusion test (MOC never rasterizes a shadow view).
		if (CullSmallShadows) {
			static REL::Relocation<std::uint8_t**> ssnGlobal{ REL::ID(513211) };
			const RE::NiCamera*                    sunCam = nullptr;
			if (auto* ssn = *ssnGlobal) {
				if (auto* dirLight = *reinterpret_cast<std::uint8_t**>(ssn + 0x210))
					sunCam = *reinterpret_cast<const RE::NiCamera**>(dirLight + 0x578);
			}
			g_sunGatherCam.store(sunCam, std::memory_order_release);
		} else {
			g_sunGatherCam.store(nullptr, std::memory_order_release);
		}

		const float          fov = atanf(1.0f / g_proj.r[0].m128_f32[0]) * 2.0f * (180.0f / 3.14159265359f);
		const float          aspect = g_proj.r[1].m128_f32[1] / g_proj.r[0].m128_f32[0];
		const float          mynear = g_proj.r[2].m128_f32[3] / (g_proj.r[2].m128_f32[2] - 1.0f);
		const float          myfar = g_proj.r[2].m128_f32[3] / (g_proj.r[2].m128_f32[2] + 1.0f);
		const RE::NiMatrix3& camR = renderCam->world.rotate;
		const XMVECTOR       look = XMVectorSet(camR.entry[0][0], camR.entry[1][0], camR.entry[2][0], 0.0f);  // dir = col 0
		const XMVECTOR       up = XMVectorSet(camR.entry[0][1], camR.entry[1][1], camR.entry[2][1], 0.0f);    // up = col 1
		g_frustum.InitializeFrustumAABB(myfar, mynear, aspect, fov, _mm_setzero_ps(), look, up);

		// Kick the BUILDER: it is the pool's single producer and does EVERYTHING -- wake,
		// clear, enqueue LAST frame's prepared list with THIS frame's matrices (published
		// above), suspend, then gather NEXT frame's candidates. The claim thread pays ~0;
		// tests may start immediately against the filling buffer (conservatively correct
		// per the library contract). When occluder rendering is off the builder clears.
		// One-frame-behind flip (build-claim winner, once per frame, at the frame boundary before the
		// scene-list cull runs -- the builder is provably idle since raster==last kick, so SetBuffer is
		// safe): publish the buffer the builder JUST finished as the FRONT this frame's cull tests, and
		// hand the builder the OTHER instance for this frame's raster. If the previous kick has not
		// completed, keep the current front (this frame tests a 2-frame-old buffer -- one consistent
		// buffer per frame, so still no per-walk flicker) and do NOT swap under the busy builder.
		if (g_prevFrameOcclusion && g_mocSpare &&
			g_rasterFrame.load(std::memory_order_acquire) == g_lastKicked) {
			g_mocFront.store(g_moc, std::memory_order_release);
			std::swap(g_moc, g_mocSpare);
			g_pool->SetBuffer(g_moc);
		}

		{
			std::scoped_lock lk(g_builderMtx);
			g_builderKick = true;
		}
		g_kickFrame.store(frame, std::memory_order_release);
		g_lastKicked = frame;
		g_builderCV.notify_one();

		// DIAG (rate-limited): build time (gather + raster, on whichever cull thread won
		// the claim) + cull tally. Verification dumps are NOT done here -- BuildOccluders
		// runs on BuildSceneLists WORKER threads and must never touch the D3D context (a
		// worker-thread CopyResource/Map killed the process); see DumpDebugImages, called
		// from the render thread (Feature::Prepass).
		if ((frame % 120u) == 0u) {
			logger::info("[MOC][diag] frame={} enq={:.2f}ms raster={:.2f}ms gather={:.2f}ms occluders={} cells={}/culled={} | tested={} culled={} (aabb {}/{} sphere {}/{}) tree {}/{} waitMs={:.2f}/n={}",
				frame, g_lastEnqueueMs, g_lastRasterMs, g_lastGatherMs, g_lastOccluderCount,
				g_cellsSeen, g_cellsCulled,
				g_tested.load(), g_culled.load(),
				g_culledAABB.load(), g_testedAABB.load(), g_culledSphere.load(), g_testedSphere.load(),
				g_treeCulled.load(), g_treeTested.load(),
				g_waitNs.exchange(0, std::memory_order_relaxed) / 1e6, g_waitCount.exchange(0, std::memory_order_relaxed));
			logger::info("[MOC][small] visible tested={} culled={} | shadow tested={} culled={}",
				g_visTested.load(std::memory_order_relaxed), g_visCulled.load(std::memory_order_relaxed),
				g_shadowTested.load(std::memory_order_relaxed), g_shadowCulled.load(std::memory_order_relaxed));

		}
		g_buildDone.store(frame, std::memory_order_release);
		return true;
	}

	void KickBuild()
	{
		// Flag-file A/B (CS_MOC_FLAGTEST): flip MOC-enable + one-frame-behind at runtime so a frozen-camera
		// comparison can interleave off / sync / prev-frame in ONE thermal session (cross-boot is warm-drift
		// unreliable). Render thread, once per frame, before the cull. moc_on.flag => MOC on; moc_pf.flag =>
		// one-frame-behind. When off, clear the front so a stale buffer can't keep culling.
		if (g_mocFlagTest) {
			const bool on = GetFileAttributesA("F:\\claudetmp\\moc_on.flag") != INVALID_FILE_ATTRIBUTES;
			EnableOcclusionTesting = on;
			g_prevFrameOcclusion = on && g_mocSpare &&
				GetFileAttributesA("F:\\claudetmp\\moc_pf.flag") != INVALID_FILE_ATTRIBUTES;
			g_hizMode = on && GetFileAttributesA("F:\\claudetmp\\moc_hiz.flag") != INVALID_FILE_ATTRIBUTES;
			if (!on)
				g_mocFront.store(nullptr, std::memory_order_release);
		}

		// Flip the async snapshot before the scene-list cull runs (this frame's kick =
		// the frame boundary). Ordered before BuildOccluders so the builder, once signaled,
		// sees the frozen test buffer.
		BeginAsyncTestFrame();
		if (auto* cam = GetMainCamera())
			BuildOccluders(cam);
	}

	namespace
	{
		// 16x block max-reduce: one 8x8 group covers a 16x16 depth block (each thread reads a
		// 2x2 quad), groupshared-reduces, and writes ONE grid texel = the FARTHEST (max, standard
		// Z) depth in the block. Out-of-bounds Loads return 0 (= near), which a max ignores, so
		// partial edge blocks are handled for free. Compiled from this string at init -- no
		// shader-file deployment to forget.
		constexpr const char* kHiZReduceSrc = R"(
Texture2D<float> SrcDepth : register(t0);
RWTexture2D<float> OutGrid : register(u0);
groupshared float gs[8][8];
[numthreads(8, 8, 1)]
void main(uint2 gid : SV_GroupID, uint2 tid : SV_GroupThreadID)
{
    uint2 p = gid * 16 + tid * 2;
    float d0 = SrcDepth[p];
    float d1 = SrcDepth[p + uint2(1, 0)];
    float d2 = SrcDepth[p + uint2(0, 1)];
    float d3 = SrcDepth[p + uint2(1, 1)];
    gs[tid.y][tid.x] = max(max(d0, d1), max(d2, d3));
    GroupMemoryBarrierWithGroupSync();
    if (((tid.x | tid.y) & 1) == 0)
        gs[tid.y][tid.x] = max(max(gs[tid.y][tid.x], gs[tid.y][tid.x + 1]),
                               max(gs[tid.y + 1][tid.x], gs[tid.y + 1][tid.x + 1]));
    GroupMemoryBarrierWithGroupSync();
    if (((tid.x | tid.y) & 3) == 0)
        gs[tid.y][tid.x] = max(max(gs[tid.y][tid.x], gs[tid.y][tid.x + 2]),
                               max(gs[tid.y + 2][tid.x], gs[tid.y + 2][tid.x + 2]));
    GroupMemoryBarrierWithGroupSync();
    if (tid.x == 0 && tid.y == 0)
        OutGrid[gid] = max(max(gs[0][0], gs[0][4]), max(gs[4][0], gs[4][4]));
}
)";

		bool HiZEnsureInit()
		{
			if (g_hizReady)
				return true;
			if (g_hizInitTried)
				return false;
			g_hizInitTried = true;
			auto* device = globals::d3d::device;
			auto* srv = Util::GetCurrentSceneDepthSRV(false);
			if (!device || !srv) {
				g_hizInitTried = false;  // renderer not up yet -- retry next frame
				return false;
			}
			winrt::com_ptr<ID3D11Resource> res;
			srv->GetResource(res.put());
			auto tex = res.try_as<ID3D11Texture2D>();
			if (!tex)
				return false;
			D3D11_TEXTURE2D_DESC dd{};
			tex->GetDesc(&dd);
			g_hizFullW = static_cast<int>(dd.Width);
			g_hizFullH = static_cast<int>(dd.Height);
			g_hizGridW = (g_hizFullW + 15) / 16;
			g_hizGridH = (g_hizFullH + 15) / 16;

			D3D11_TEXTURE2D_DESC gd{};
			gd.Width = static_cast<UINT>(g_hizGridW);
			gd.Height = static_cast<UINT>(g_hizGridH);
			gd.MipLevels = 1;
			gd.ArraySize = 1;
			gd.Format = DXGI_FORMAT_R32_FLOAT;
			gd.SampleDesc = { 1, 0 };
			gd.Usage = D3D11_USAGE_DEFAULT;
			gd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			if (FAILED(device->CreateTexture2D(&gd, nullptr, g_hizGridTex.put())))
				return false;
			Util::SetResourceName(g_hizGridTex.get(), "OcclusionCulling::HiZGrid");
			if (FAILED(device->CreateUnorderedAccessView(g_hizGridTex.get(), nullptr, g_hizGridUAV.put())))
				return false;
			Util::SetResourceName(g_hizGridUAV.get(), "OcclusionCulling::HiZGrid UAV");

			gd.Usage = D3D11_USAGE_STAGING;
			gd.BindFlags = 0;
			gd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			for (int i = 0; i < kHizRing; ++i) {
				if (FAILED(device->CreateTexture2D(&gd, nullptr, g_hizRing[i].staging.put())))
					return false;
				Util::SetResourceName(g_hizRing[i].staging.get(), "OcclusionCulling::HiZStaging%d", i);
			}

			winrt::com_ptr<ID3DBlob> blob, errs;
			if (FAILED(D3DCompile(kHiZReduceSrc, strlen(kHiZReduceSrc), "HiZReduce", nullptr, nullptr,
					"main", "cs_5_0", 0, 0, blob.put(), errs.put()))) {
				logger::error("[MOC][HiZ] reduce shader compile failed: {}",
					errs ? static_cast<const char*>(errs->GetBufferPointer()) : "(no log)");
				return false;
			}
			if (FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, g_hizCS.put())))
				return false;

			g_hizReady = true;
			logger::info("[MOC][HiZ] initialized: depth {}x{} -> grid {}x{}, ring={}",
				g_hizFullW, g_hizFullH, g_hizGridW, g_hizGridH, kHizRing);
			return true;
		}
	}

	void HiZPrepass()
	{
		// RENDER THREAD ONLY (immediate context) -- called from Deferred::PrepassPasses, where
		// this frame's depth is fully populated and the camera matrices are final. Never touch
		// the context from cull/job threads (a worker-thread CopyResource killed the process).
		if (!g_hizMode)
			return;
		auto* ctx = globals::d3d::context;
		if (!ctx || !HiZEnsureInit())
			return;

		// 1. Harvest the OLDEST ring slot (written kHizRing frames ago -> its copy has long
		//    retired; DO_NOT_WAIT will practically always succeed). Publish it as the snapshot
		//    the next cull tests against, paired with the matrices its depth was rendered with.
		auto& slot = g_hizRing[g_hizWrite];
		if (slot.pending) {
			D3D11_MAPPED_SUBRESOURCE m{};
			if (SUCCEEDED(ctx->Map(slot.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
				HiZSnapshot& snap = g_hizSnaps[g_hizWriteSnap];
				snap.grid.resize(static_cast<std::size_t>(g_hizGridW) * g_hizGridH);
				for (int y = 0; y < g_hizGridH; ++y)
					std::memcpy(&snap.grid[static_cast<std::size_t>(y) * g_hizGridW],
						static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch,
						static_cast<std::size_t>(g_hizGridW) * sizeof(float));
				ctx->Unmap(slot.staging.get(), 0);
				snap.gridW = g_hizGridW;
				snap.gridH = g_hizGridH;
				snap.fullW = g_hizFullW;
				snap.fullH = g_hizFullH;
				snap.view = slot.view;
				snap.viewProj = slot.viewProj;
				snap.posAdjust = slot.posAdjust;
				snap.posAdjustV = _mm_setr_ps(slot.posAdjust.x, slot.posAdjust.y, slot.posAdjust.z, 0.0f);
				g_hizFront.store(&snap, std::memory_order_release);
				g_hizWriteSnap ^= 1;
				slot.pending = false;
			}
			// WAS_STILL_DRAWING: keep the slot pending and skip this frame's capture (never
			// overwrite an in-flight copy); the ring self-heals next frame.
		}

		// 2. Capture this frame: reduce the depth into the grid, queue the copy into the slot,
		//    and tag it with THIS frame's camera (the one that rendered this depth).
		if (!slot.pending) {
			auto* cam = GetMainCamera();
			auto* depthSRV = Util::GetCurrentSceneDepthSRV(false);
			if (cam && depthSRV) {
				ctx->CSSetShaderResources(0, 1, &depthSRV);
				ID3D11UnorderedAccessView* uav = g_hizGridUAV.get();
				ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
				ctx->CSSetShader(g_hizCS.get(), nullptr, 0);
				ctx->Dispatch(static_cast<UINT>(g_hizGridW), static_cast<UINT>(g_hizGridH), 1);
				ID3D11ShaderResourceView* nullSRV = nullptr;
				ID3D11UnorderedAccessView* nullUAV = nullptr;
				ctx->CSSetShaderResources(0, 1, &nullSRV);
				ctx->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
				ctx->CSSetShader(nullptr, nullptr, 0);
				ctx->CopyResource(slot.staging.get(), g_hizGridTex.get());

				XMMATRIX v{}, p{}, vp{};
				CalculateViewProjection(cam, v, p, vp);
				slot.view = v;
				slot.viewProj = vp;
				slot.posAdjust = cam->world.translate;
				slot.pending = true;
				g_hizWrite = (g_hizWrite + 1) % kHizRing;
			}
		}
	}

	void DumpDebugImages()
	{
		// RENDER THREAD ONLY (DumpGameDepth uses the immediate context). Env-gated,
		// rate-limited verification dumps of the MOC buffer + the game's own depth.
		static const bool s_dump = [] {
			char buf[8] = {};
			return GetEnvironmentVariableA("CS_MOC_DUMP", buf, sizeof(buf)) && buf[0] == '1';
		}();
		if (!s_dump || !g_init || !g_moc)
			return;
		auto* gfxState = RE::BSGraphics::State::GetSingleton();
		if (!gfxState || (gfxState->frameCount % 120u) != 0u)
			return;
		// Deterministic dump: the buffer fills ASYNCHRONOUSLY (builder + workers, no
		// flush); reading mid-fill shows a partial buffer that varies run to run. Drain
		// the builder, then give the workers a moment to finish the queued tail.
		QuiesceBuilder();
		std::this_thread::sleep_for(std::chrono::milliseconds(8));
		DumpDepthImage();  // MOC occluder buffer -> moc_depth.png (+ raw .bin)
	}

	// -------------------------------------------------------------------------
	// Occlusion queries.
	// -------------------------------------------------------------------------
	namespace
	{
		bool TestAABB(RE::BSMultiBoundAABB* a_object)
		{
			const __m128 vCenter = _mm_sub_ps(_mm_setr_ps(a_object->center.x, a_object->center.y, a_object->center.z, 0.0f), g_posAdjustV);
			const __m128 vHalf = _mm_setr_ps(a_object->size.x, a_object->size.y, a_object->size.z, 0.0f);

			const __m128 vMin = _mm_sub_ps(vCenter, vHalf);
			const __m128 vMax = _mm_add_ps(vCenter, vHalf);

			__m128 xRow[2], yRow[2], zRow[2];
			xRow[0] = _mm_mul_ps(_mm_shuffle_ps(vMin, vMin, 0x00), g_viewProj.r[0]);
			xRow[1] = _mm_mul_ps(_mm_shuffle_ps(vMax, vMax, 0x00), g_viewProj.r[0]);
			yRow[0] = _mm_mul_ps(_mm_shuffle_ps(vMin, vMin, 0x55), g_viewProj.r[1]);
			yRow[1] = _mm_mul_ps(_mm_shuffle_ps(vMax, vMax, 0x55), g_viewProj.r[1]);
			zRow[0] = _mm_mul_ps(_mm_shuffle_ps(vMin, vMin, 0xaa), g_viewProj.r[2]);
			zRow[1] = _mm_mul_ps(_mm_shuffle_ps(vMax, vMax, 0xaa), g_viewProj.r[2]);

			const __m128 minVert = _mm_add_ps(g_viewProj.r[3],
				_mm_add_ps(_mm_add_ps(_mm_min_ps(xRow[0], xRow[1]), _mm_min_ps(yRow[0], yRow[1])), _mm_min_ps(zRow[0], zRow[1])));
			const float minW = minVert.m128_f32[3];

			if (minW < 0.00000001f) {
				// Behind/straddling the near plane: fall back to the frustum verdict so
				// MOC-exclusive culling reproduces vanilla frustum culls (see TestSphere).
				const XMVECTOR center = _mm_sub_ps(_mm_setr_ps(a_object->center.x, a_object->center.y, a_object->center.z, 0.0f), g_posAdjustV);
				const XMVECTOR halfExtents = _mm_setr_ps(a_object->size.x, a_object->size.y, a_object->size.z, 0.0f);
				return g_frustum.AABBInFrustum(center, halfExtents);
			}

			static const std::uint32_t sBBxInd[8] = { 1, 0, 0, 1, 1, 1, 0, 0 };
			static const std::uint32_t sBByInd[8] = { 1, 1, 1, 1, 0, 0, 0, 0 };
			static const std::uint32_t sBBzInd[8] = { 1, 1, 0, 0, 0, 1, 1, 0 };

			__m128       screenMin = _mm_set1_ps(FLT_MAX);
			__m128       screenMax = _mm_set1_ps(-FLT_MAX);
			const __m128 baseVert = g_viewProj.r[3];

			for (std::uint32_t i = 0; i < 8; i++) {
				__m128 vert = baseVert;
				vert = _mm_add_ps(vert, xRow[sBBxInd[i]]);
				vert = _mm_add_ps(vert, yRow[sBByInd[i]]);
				vert = _mm_add_ps(vert, zRow[sBBzInd[i]]);

				const __m128 vertW = _mm_shuffle_ps(vert, vert, 0xff);
				const __m128 xformedPos = _mm_div_ps(vert, vertW);

				screenMin = _mm_min_ps(screenMin, xformedPos);
				screenMax = _mm_max_ps(screenMax, xformedPos);
			}

			// Hi-Z seam: the object's NEAREST NDC depth (standard Z) = min z over the projected
			// corners, already present as component 2 of the per-corner min.
			if (g_hizMode)
				return HiZTestRect(screenMin.m128_f32[0], screenMin.m128_f32[1],
					screenMax.m128_f32[0], screenMax.m128_f32[1], screenMin.m128_f32[2]);
			auto* moc = TestBuf();
			if (!moc)
				return true;
			const auto r = moc->TestRect(screenMin.m128_f32[0], screenMin.m128_f32[1], screenMax.m128_f32[0], screenMax.m128_f32[1], minW);
			return r == MaskedOcclusionCulling::VISIBLE;
		}

		// Sphere occlusion test on a VALUE bound (center + radius) -- no object deref, so
		// the async path can call it with a snapshot's value-copied bound off-thread.
		bool TestSphereBound(float a_cx, float a_cy, float a_cz, float sphereRadius)
		{
			if (sphereRadius <= 5.0f)
				return true;

			const RE::NiPoint3 c{ a_cx, a_cy, a_cz };

			// Camera-relative sphere center (w = 1).
			XMVECTOR bounds = _mm_sub_ps(_mm_setr_ps(c.x, c.y, c.z, 1.0f), g_posAdjustV);

			// Never cull a sphere the camera is inside of.
			if (XMVector3Length(bounds).m128_f32[0] <= sphereRadius)
				return true;

			// Early depth-rejection point: nearest point on the sphere toward the eye.
			XMVECTOR v = XMVectorSubtract(_mm_setzero_ps(), bounds);
			XMVECTOR closestPoint = XMVectorAdd(bounds, XMVectorScale(XMVector3Normalize(v), sphereRadius));
			closestPoint = XMVector4Transform(XMVectorSetW(closestPoint, 1.0f), g_viewProj);

			const float closestSpherePointW = closestPoint.m128_f32[3];
			if (closestSpherePointW < 0.000001f) {
				// Behind/straddling the near plane: TestRect can't decide. For MOC-exclusive
				// culling this must match the ENGINE's frustum verdict, not blanket-keep:
				// a sphere fully outside the view frustum (incl. behind the camera) is
				// culled exactly like vanilla frustum culling would.
				const XMVECTOR sphere = _mm_setr_ps(
					c.x - g_posAdjust.x, c.y - g_posAdjust.y, c.z - g_posAdjust.z, sphereRadius);
				return g_frustum.SphereInFrustum(sphere);
			}

			XMVECTOR viewEye = { g_view.r[0].m128_f32[3], g_view.r[1].m128_f32[3], g_view.r[2].m128_f32[3], 0.0f };
			viewEye = XMVectorNegate(viewEye);

			XMVECTOR    viewEyeSphereDirection = XMVectorSubtract(viewEye, bounds);
			const float cameraSphereDistance = XMVector3Length(viewEyeSphereDirection).m128_f32[0];

			XMVECTOR viewUp = { g_view.r[0].m128_f32[1], g_view.r[1].m128_f32[1], g_view.r[2].m128_f32[1], 0.0f };
			XMVECTOR viewRight = XMVector3Normalize(XMVector3Cross(viewEyeSphereDirection, viewUp));

			// Perspective-distortion compensation.
			const float fRadius = cameraSphereDistance * tanf(asinf(sphereRadius / cameraSphereDistance));

			XMVECTOR vUpRadius = XMVectorScale(viewUp, fRadius);
			XMVECTOR vRightRadius = XMVectorScale(viewRight, fRadius);

			XMVECTOR vCorner0WS = XMVectorSubtract(XMVectorAdd(bounds, vUpRadius), vRightRadius);
			XMVECTOR vCorner1WS = XMVectorAdd(XMVectorAdd(bounds, vUpRadius), vRightRadius);
			XMVECTOR vCorner2WS = XMVectorSubtract(XMVectorSubtract(bounds, vUpRadius), vRightRadius);
			XMVECTOR vCorner3WS = XMVectorAdd(XMVectorSubtract(bounds, vUpRadius), vRightRadius);

			XMVECTOR vCorner0CS = XMVector4Transform(vCorner0WS, g_viewProj);
			XMVECTOR vCorner1CS = XMVector4Transform(vCorner1WS, g_viewProj);
			XMVECTOR vCorner2CS = XMVector4Transform(vCorner2WS, g_viewProj);
			XMVECTOR vCorner3CS = XMVector4Transform(vCorner3WS, g_viewProj);

			XMVECTOR vCorner0NDC = XMVectorDivide(vCorner0CS, XMVectorSplatW(vCorner0CS));
			XMVECTOR vCorner1NDC = XMVectorDivide(vCorner1CS, XMVectorSplatW(vCorner1CS));
			XMVECTOR vCorner2NDC = XMVectorDivide(vCorner2CS, XMVectorSplatW(vCorner2CS));
			XMVECTOR vCorner3NDC = XMVectorDivide(vCorner3CS, XMVectorSplatW(vCorner3CS));

			XMVECTOR xyMins = _mm_min_ps(vCorner0NDC, _mm_min_ps(vCorner1NDC, _mm_min_ps(vCorner2NDC, vCorner3NDC)));
			XMVECTOR xyMaxs = _mm_max_ps(vCorner0NDC, _mm_max_ps(vCorner1NDC, _mm_max_ps(vCorner2NDC, vCorner3NDC)));

			// Hi-Z seam: nearest sphere point's NDC depth = clip z / clip w of closestPoint.
			if (g_hizMode)
				return HiZTestRect(xyMins.m128_f32[0], xyMins.m128_f32[1],
					xyMaxs.m128_f32[0], xyMaxs.m128_f32[1],
					closestPoint.m128_f32[2] / closestSpherePointW);
			auto* moc = TestBuf();
			if (!moc)
				return true;
			const auto r = moc->TestRect(xyMins.m128_f32[0], xyMins.m128_f32[1], xyMaxs.m128_f32[0], xyMaxs.m128_f32[1], closestSpherePointW);
			return r == MaskedOcclusionCulling::VISIBLE;
		}

		bool TestSphere(RE::NiAVObject* a_object)
		{
			const auto& b = a_object->worldBound;
			return TestSphereBound(b.center.x, b.center.y, b.center.z, b.radius);
		}
	}  // namespace

	bool TestObject(RE::NiAVObject* a_object)
	{
		if (!g_init || !EnableOcclusionTesting || !a_object)
			return true;

		// CHEAP-FIRST gate order: this runs ~8k times per frame (the full main-view
		// query volume), so plain member reads come before anything virtual and all
		// RTTI stays at the very end, paid only by survivors.
		if (a_object->worldBound.radius < OccluderTestMinRadius)
			return true;

		if (a_object->GetAppCulled())
			return true;

		// DIAGNOSTIC forced culling (perf probe only; image intentionally breaks).
		if (DiagForceCullPercent > 0) {
			const std::uintptr_t h = reinterpret_cast<std::uintptr_t>(a_object);
			if (static_cast<std::int32_t>((h >> 4) * 2654435761u % 100u) < DiagForceCullPercent) {
				g_culled.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
		}

		// Never occlusion-test ACTORS: skinned world bounds lag animation, actors
		// move against our one-frame-stale occluder list, and wrongly culling an
		// NPC is the most visible artifact possible. formType is a plain member
		// read -- no As<Actor>() RTTI on the hot path.
		if (auto* ref = a_object->GetUserData(); ref && ref->formType == RE::FormType::ActorCharacter)
			return true;

		if (g_hizMode) {
			if (!g_hizFront.load(std::memory_order_acquire))
				return true;  // no Hi-Z readback published yet -- keep (conservative)
		} else if (g_prevFrameOcclusion) {
			if (!g_mocFront.load(std::memory_order_acquire))
				return true;  // no completed prev-frame buffer yet -- keep (conservative)
		} else {
			auto* gfx = RE::BSGraphics::State::GetSingleton();
			if (!gfx || !WaitForRasterComplete(gfx->frameCount))
				return true;  // buffer not ready this frame -- keep (conservative)
		}

		auto*      aabb = GetAABBNode(a_object);  // the single RTTI lookup, survivors only
		const bool visible = aabb ? TestAABB(aabb) : TestSphere(a_object);
		g_tested.fetch_add(1, std::memory_order_relaxed);
		(aabb ? g_testedAABB : g_testedSphere).fetch_add(1, std::memory_order_relaxed);
		if (!visible) {
			g_culled.fetch_add(1, std::memory_order_relaxed);
			(aabb ? g_culledAABB : g_culledSphere).fetch_add(1, std::memory_order_relaxed);
		}
		return visible;
	}

	// Async test frame boundary -- called once per frame from the DrawWorld_PreRender kick
	// (render thread, BEFORE the scene-list cull). Flips the snapshot buffer so the builder
	// tests what Process1 appended last frame, clears the new append buffer, and stamps a
	// fresh version so this frame's reads miss (keep) until the builder republishes.
	void BeginAsyncTestFrame()
	{
		if (!AsyncOcclusionTest)
			return;
		if (g_snap[0].size() != kSnapCap) {  // lazy alloc on first use (render thread, no appends yet)
			g_snap[0].resize(kSnapCap);
			g_snap[1].resize(kSnapCap);
		}
		const int prev = g_snapAppend.load(std::memory_order_relaxed);
		const int next = prev ^ 1;
		g_snapCount[next].store(0, std::memory_order_relaxed);
		g_snapAppend.store(next, std::memory_order_release);
		// Publish {new version, testBuffer=prev} atomically. The builder tests `prev` (last
		// frame's appends) at the version it reads here; Process1 appends to `next`.
		const std::uint32_t newVer = static_cast<std::uint32_t>(g_asyncStamp.load(std::memory_order_relaxed) >> 1) + 1u;
		g_asyncStamp.store((static_cast<std::uint64_t>(newVer) << 1) | static_cast<std::uint32_t>(prev), std::memory_order_release);
	}

	// Test the frozen snapshot against the just-rasterized buffer and publish the verdict
	// table -- called on the builder thread right after the raster completes (buffer ready,
	// no WaitForRasterComplete needed). Writes the SPARE table then atomically publishes it;
	// the 2-frame recycle guarantees no reader is still on the table being rebuilt.
	void RunAsyncTest()
	{
		if (!AsyncOcclusionTest || !g_moc || g_snap[0].size() != kSnapCap)
			return;
		// Read {version, testBuffer} as ONE atomic snapshot so the pair can't decouple.
		const std::uint64_t stamp = g_asyncStamp.load(std::memory_order_acquire);
		const int           tb = static_cast<int>(stamp & 1u);
		const std::uint32_t n = std::min(g_snapCount[tb].load(std::memory_order_acquire), kSnapCap);
		for (std::uint32_t i = 0; i < n; ++i) {
			const auto& e = g_snap[tb][i];
			const bool  visible = TestSphereBound(e.cx, e.cy, e.cz, e.r);
			// PROBE: write the verdict bit directly on the object -- a raw-pointer deref of a
			// LAST-frame snapshot key. Assumes the object is still alive (true for stationary
			// scenes; unsafe across streaming/cell transitions -- benchmark use only). Atomic
			// so a racing engine flag write can only lose OUR bit (-> conservative keep),
			// never corrupt the engine's other flags.
			auto* obj = static_cast<RE::NiAVObject*>(const_cast<void*>(e.key));
			std::atomic_ref<std::uint32_t> fref(*reinterpret_cast<std::uint32_t*>(&obj->GetFlags()));
			if (visible)
				fref.fetch_and(~kMocOccludedBit, std::memory_order_relaxed);
			else
				fref.fetch_or(kMocOccludedBit, std::memory_order_relaxed);
		}
	}

	// Drop-in for TestObject on the async path: records the object for next frame's test and
	// returns THIS frame's published verdict (miss -> keep). Same cheap-first guards as
	// TestObject (radius / app-culled / actor) so the snapshot only holds testable objects.
	bool TestObjectCached(RE::NiAVObject* a_object)
	{
		if (!g_init || !EnableOcclusionTesting || !a_object)
			return true;
		if (a_object->worldBound.radius < OccluderTestMinRadius)
			return true;
		if (a_object->GetAppCulled())
			return true;
		if (auto* ref = a_object->GetUserData(); ref && ref->formType == RE::FormType::ActorCharacter)
			return true;

		// Record for NEXT frame's async test (value-copy the world bound).
		const int           b = g_snapAppend.load(std::memory_order_acquire);
		const std::uint32_t i = g_snapCount[b].fetch_add(1, std::memory_order_relaxed);
		if (i < g_snap[b].size()) {  // size is 0 until BeginAsyncTestFrame allocates -> never OOB
			const auto& bd = a_object->worldBound;
			g_snap[b][i] = SnapEntry{ a_object, bd.center.x, bd.center.y, bd.center.z, bd.radius };
		}

		// Read the verdict bit the builder wrote on this object last frame. Free (no lookup)
		// -- the object is already in cache from the guards above. Bit unset (default) -> keep.
		if ((a_object->GetFlags().underlying() & kMocOccludedBit) != 0) {
			g_tested.fetch_add(1, std::memory_order_relaxed);
			g_culled.fetch_add(1, std::memory_order_relaxed);
			return false;  // occluded -> cull
		}
		return true;  // visible / not-yet-tested -> keep
	}

	bool TestMultiBound(void* a_multiBound)
	{
		if (!g_init || !EnableOcclusionTesting || !a_multiBound)
			return true;

		auto* mb = static_cast<RE::BSMultiBound*>(a_multiBound);
		auto* aabb = netimmerse_cast<RE::BSMultiBoundAABB*>(mb->data.get());
		if (!aabb || aabb->size.z <= 1.0f)
			return true;  // no AABB shape (spheres etc.) or degenerate bounds -> keep

		if (g_hizMode) {
			if (!g_hizFront.load(std::memory_order_acquire))
				return true;
		} else if (g_prevFrameOcclusion) {
			if (!g_mocFront.load(std::memory_order_acquire))
				return true;
		} else {
			auto* gfx = RE::BSGraphics::State::GetSingleton();
			if (!gfx || !WaitForRasterComplete(gfx->frameCount))
				return true;
		}
		const bool visible = TestAABB(aabb);
		g_tested.fetch_add(1, std::memory_order_relaxed);
		g_testedAABB.fetch_add(1, std::memory_order_relaxed);
		if (!visible) {
			g_culled.fetch_add(1, std::memory_order_relaxed);
			g_culledAABB.fetch_add(1, std::memory_order_relaxed);
		}
		return visible;
	}

	bool IsMainViewCamera(const RE::NiCamera* a_camera)
	{
		return a_camera && (a_camera == GetMainCamera() || a_camera == *g_cullCamera);
	}

	bool IsSceneListCamera(const RE::NiCamera* a_camera)
	{
		return a_camera && a_camera == GetMainCamera();
	}

	bool IsSunGatherCamera(const RE::NiCamera* a_camera)
	{
		return a_camera && a_camera == g_sunGatherCam.load(std::memory_order_acquire);
	}

	namespace
	{
		// Shared distance-scaled small-object test (user/HZD design): an object whose
		// bounding sphere is smaller than (min + slope*camDist) covers a negligible
		// area at that distance. Threshold GROWS with distance from the player camera
		// (g_posAdjust): distant clutter is culled, nearby objects always survive.
		// Actors (skinned-bounds lag) and kAlwaysDraw objects are never culled.
		// Pure size/distance math, no raster buffer. Returns true = keep.
		inline bool SmallCullKeeps(RE::NiAVObject* a_object, float a_minSize, float a_slope)
		{
			const auto fl = a_object->GetFlags().underlying();
			if ((fl & 0x800) || (fl & 0x1000))  // kAlwaysDraw / shortcut -> never cull
				return true;
			if (auto* ref = a_object->GetUserData(); ref && ref->formType == RE::FormType::ActorCharacter)
				return true;  // never size-cull actors (skinned bounds lag one frame)
			const auto& b = a_object->worldBound;
			if (b.radius <= 0.0f)
				return true;
			const float dx = b.center.x - g_posAdjust.x;
			const float dy = b.center.y - g_posAdjust.y;
			const float dz = b.center.z - g_posAdjust.z;
			const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			return b.radius >= a_minSize + a_slope * dist;
		}
	}

	// Small VISIBLE-object cull: drops the small object from the MAIN view (its mesh
	// stops rendering). Shrinks the opaque + z-prepass draw lists but removes on-screen
	// geometry, so it pops in motion -- off by default. Returns true = keep.
	bool TestSmallVisible(RE::NiAVObject* a_object)
	{
		if (!CullSmallVisible || !a_object)
			return true;
		const bool keep = SmallCullKeeps(a_object, SmallVisibleMinSize, SmallVisibleSlope);
		g_visTested.fetch_add(1, std::memory_order_relaxed);
		if (!keep)
			g_visCulled.fetch_add(1, std::memory_order_relaxed);
		return keep;
	}

	// Small-caster SHADOW cull: drops the small object from the SHADOW MAPS only (its
	// mesh still renders in the main view; only its shadow disappears). Runs on the
	// sun shadow-gather pass, so a rejection removes the caster from every cascade,
	// cutting shadow-map draws -- the bulk of the Utility shader time -- with no
	// visible change. Returns true = keep.
	bool TestShadowCasterSmall(RE::NiAVObject* a_object)
	{
		if (!CullSmallShadows || !a_object)
			return true;
		const bool keep = SmallCullKeeps(a_object, SmallShadowMinSize, SmallShadowSlope);
		g_shadowTested.fetch_add(1, std::memory_order_relaxed);
		if (!keep)
			g_shadowCulled.fetch_add(1, std::memory_order_relaxed);
		return keep;
	}

	bool TestInstanceGroup(RE::BSMultiBoundAABB* a_aabb)
	{
		if (!g_init || !EnableOcclusionTesting || !CullTreeLODGroups || !a_aabb)
			return true;

		{
			auto* gfx = RE::BSGraphics::State::GetSingleton();
			if (!gfx || !WaitForRasterComplete(gfx->frameCount))
				return true;
		}
		const bool visible = TestAABB(a_aabb);
		g_treeTested.fetch_add(1, std::memory_order_relaxed);
		if (!visible)
			g_treeCulled.fetch_add(1, std::memory_order_relaxed);
		return visible;
	}
}
