#include "MOC.h"

#include "Globals.h"
#include "State.h"
#include "Utils/D3D.h"

#include <d3dcompiler.h>
#include <d3d11.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstring>
#include <vector>

#include <RE/A/Actor.h>
#include <RE/B/BSGeometry.h>
#include <RE/B/BSMultiBound.h>
#include <RE/B/BSMultiBoundAABB.h>
#include <RE/B/BSMultiBoundNode.h>
#include <RE/B/BSSceneGraph.h>
#include <RE/L/LoadingMenu.h>
#include <RE/N/NiCamera.h>
#include <RE/N/NiMatrix3.h>
#include <RE/N/NiNode.h>
#include <RE/N/NiPoint3.h>
#include <RE/N/NiRTTI.h>
#include <RE/N/NiTransform.h>
#include <RE/R/Renderer.h>
#include <RE/S/State.h>
#include <RE/U/UI.h>

using namespace DirectX;

namespace MOC
{
	// ---- Settings consumed by the Hi-Z occlusion path (written by the OcclusionCulling
	//      Feature from its serialized settings; read here). ----
	// Master gate for the per-object / container occlusion tests.
	bool  EnableOcclusionTesting = true;
	// Only objects/subtrees with at least this world-bound radius are occlusion-tested.
	float OccluderTestMinRadius = 0.0f;
	// Distance-scaled small shadow-caster cull (shadow map only; on by default). Pure
	// size/distance math vs the player camera; never culls actors or kAlwaysDraw objects.
	bool  CullSmallShadows = true;
	float SmallShadowMinSize = 0.0f;
	float SmallShadowSlope = 0.03f;  // measured to net Utility down vs occlusion-off

	namespace
	{
		// =================================================================================
		// GPU Hi-Z occlusion is the SOLE occlusion path. A compute pass max-reduces the
		// post-zprepass depth (STANDARD Z: near=0, far=1) into a (w/16, h/16) grid of the
		// farthest depth per 16x16 block; a 3-deep staging ring reads it back without
		// stalling; the cull tests object screen rects against the CPU grid (HiZTestRect).
		bool  g_hizMode = true;   // sole occlusion path (unconditional)
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
		winrt::com_ptr<ID3D11ShaderResourceView>  g_hizGridSRV;  // read-side for GPU consumers (grass cull)
		int                                       g_hizGridW = 0, g_hizGridH = 0, g_hizFullW = 0, g_hizFullH = 0;
		bool                                      g_hizInitTried = false, g_hizReady = false;
		// Frame index of the most recent reduce into g_hizGridTex. GPU consumers (grass cull) read the
		// GPU grid a frame after it was built (the texture is overwritten later in the same frame at
		// HiZPrepass), so they require an exact one-frame lag -- (currentFrame - g_hizGridFrame == 1).
		std::atomic<std::uint64_t>                g_hizGridFrame{ 0 };
		// Camera near/far the grid depth was projected with (published under g_hizGridFrame's release).
		// A GPU consumer reconstructs the grid's NDC-z from its own reliable view-space depth (clip.w) as
		// far/(far-near)*(1-near/w) -- matching the depth buffer exactly without sharing the projection.
		float                                     g_hizNear = 0.0f, g_hizFar = 0.0f;

		// Sun shadow-gather camera (the small-caster SHADOW cull runs only on that pass).
		std::atomic<const RE::NiCamera*> g_sunGatherCam{ nullptr };
		bool                             g_init = false;

		// Camera matrices for the current build (camera-relative, row-vector layout), loaded
		// from the published Hi-Z snapshot so every projection matches the buffer it tests.
		XMMATRIX     g_view = XMMatrixIdentity();
		XMMATRIX     g_viewProj = XMMatrixIdentity();
		RE::NiPoint3 g_posAdjust{ 0.0f, 0.0f, 0.0f };
		XMVECTOR     g_posAdjustV = _mm_setzero_ps();  // (x, y, z, 0)

		// Once-per-frame build coordination: one of the concurrent main-scene culls CAS-claims
		// the frame and publishes the snapshot load; losers skip testing until published.
		std::atomic<std::uint32_t> g_buildClaim{ 0xFFFFFFFFu };
		std::atomic<std::uint32_t> g_buildDone{ 0xFFFFFFFFu };

		// Post-load settle gate: skip builds for ~240 kicks after a load (cells still stream in
		// after the LoadingMenu closes; testing half-built cells is a use-after-free risk).
		std::atomic<std::uint64_t> g_kickCounter{ 0 };
		std::atomic<std::uint64_t> g_settleUntilKick{ 0 };

		// WorldScenegraph global (holds a NiNode*). Nukem 0x2F4CE30 -> SE REL::ID(517006).
		REL::Relocation<RE::NiNode**> g_worldScenegraph{ REL::ID(517006) };
		// The engine's frozen CULL camera (identifies the main-scene cull passes).
		REL::Relocation<RE::NiCamera**> g_cullCamera{ REL::ID(528062) };

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
			if (a_objNdcZMin > 0.9995f)
				return true;  // sky-dome / far-plane distance: the depth range is compressed to ~1.0 there
				              // so the coarse grid can't reliably occlude, and sky/cloud objects are never
				              // occluded -- keep (prevents distant clouds/atmosphere vanishing).
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
			gd.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;  // SRV for GPU consumers (grass cull)
			if (FAILED(device->CreateTexture2D(&gd, nullptr, g_hizGridTex.put())))
				return false;
			Util::SetResourceName(g_hizGridTex.get(), "OcclusionCulling::HiZGrid");
			if (FAILED(device->CreateUnorderedAccessView(g_hizGridTex.get(), nullptr, g_hizGridUAV.put())))
				return false;
			Util::SetResourceName(g_hizGridUAV.get(), "OcclusionCulling::HiZGrid UAV");
			if (FAILED(device->CreateShaderResourceView(g_hizGridTex.get(), nullptr, g_hizGridSRV.put())))
				return false;
			Util::SetResourceName(g_hizGridSRV.get(), "OcclusionCulling::HiZGrid SRV");

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
				// Straddling the near plane: this object is very close to the camera. Hi-Z occlusion
				// is ADDITIVE (the engine's own frustum cull already ran this frame), so KEEP it --
				// never re-test a near object against the 1-frame-stale snapshot frustum, which
				// over-culls close geometry (the ground right in front of you, a nearby tree) during
				// camera motion. Close objects are almost never occluded and catastrophic if dropped.
				return true;
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
			return true;
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
				// Straddling the near plane: keep. Hi-Z occlusion is additive (the engine's frustum
				// cull already ran), so never re-test a near object against the 1-frame-stale snapshot
				// frustum -- that over-culls close geometry (ground/nearby trees) during camera motion.
				return true;
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
			return true;
		}

		bool TestSphere(RE::NiAVObject* a_object)
		{
			const auto& b = a_object->worldBound;
			return TestSphereBound(b.center.x, b.center.y, b.center.z, b.radius);
		}

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
	}  // namespace

	void Init()
	{
		// GPU Hi-Z resources are created lazily in HiZEnsureInit (the renderer is not up at
		// PostPostLoad); Init only marks the feature live so the seams start testing.
		if (g_init)
			return;
		g_init = true;
		logger::info("[MOC] initialized (GPU Hi-Z camera occlusion)");
	}

	void Shutdown()
	{
		// Release only the Hi-Z GPU resources; there is no builder thread or MOC library.
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
		return g_init;
	}

	bool GetHiZGridForCompute(ID3D11ShaderResourceView*& a_srv, int& a_gridW, int& a_gridH,
		int& a_fullW, int& a_fullH, std::uint64_t& a_buildFrame, float& a_near, float& a_far)
	{
		if (!g_hizReady || !g_hizGridSRV)
			return false;
		a_srv = g_hizGridSRV.get();
		a_gridW = g_hizGridW;
		a_gridH = g_hizGridH;
		a_fullW = g_hizFullW;
		a_fullH = g_hizFullH;
		a_buildFrame = g_hizGridFrame.load(std::memory_order_acquire);  // acquire pairs with the release store
		a_near = g_hizNear;
		a_far = g_hizFar;
		return true;
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

	bool BuildOccluders(RE::NiCamera* a_camera)
	{
		if (!g_init)
			return false;

		// MAIN-pass gate by pointer identity with the engine's two main-scene cameras: the
		// BuildSceneLists cell culls carry the RENDER camera, the MainAccum / depth-prepass
		// subtree culls carry the frozen CULL camera (REL::ID 528062). Auxiliary passes
		// (water reflection, first-person, sky, shadows, cubemap) each match neither.
		RE::NiCamera* renderCam = GetMainCamera();
		if (!a_camera || !renderCam)
			return false;
		if (a_camera != renderCam && a_camera != *g_cullCamera)
			return false;

		// Feature toggled off => no occlusion work at all.
		if (!EnableOcclusionTesting)
			return false;

		// Quiesce during loads + a post-load settle window (cells stream in for a couple of
		// seconds after the LoadingMenu closes; testing half-built cells is unsafe).
		if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME)) {
			g_settleUntilKick.store(g_kickCounter.load(std::memory_order_relaxed) + 240, std::memory_order_relaxed);
			return false;
		}
		if (g_kickCounter.fetch_add(1, std::memory_order_relaxed) < g_settleUntilKick.load(std::memory_order_relaxed))
			return false;

		// Once per frame: CAS-claim the frame so exactly ONE of the concurrent main-scene culls
		// loads the snapshot. Losers report whether a completed load for this frame is published.
		auto*               gfxState = RE::BSGraphics::State::GetSingleton();
		const std::uint32_t frame = gfxState ? gfxState->frameCount : 0;
		std::uint32_t       claimed = g_buildClaim.load(std::memory_order_relaxed);
		if (claimed == frame || !g_buildClaim.compare_exchange_strong(claimed, frame, std::memory_order_relaxed))
			return g_buildDone.load(std::memory_order_acquire) == frame;

		// Hi-Z mode: the game's own depth IS the occluder set (built + read back on the render
		// thread in HiZPrepass). Load the published snapshot's matrices into the shared test
		// globals so every projection in TestAABB / TestSphereBound matches the buffer it tests
		// against, then declare the frame built. No snapshot yet => no occlusion this frame.
		const HiZSnapshot* snap = g_hizFront.load(std::memory_order_acquire);
		if (!snap)
			return false;
		g_view = snap->view;
		g_viewProj = snap->viewProj;
		g_posAdjust = snap->posAdjust;
		g_posAdjustV = snap->posAdjustV;

		// Identify the sun shadow-gather camera (dirLight = ShadowSceneNode+0x210, gather cam
		// +0x578; IDA 2026-07-11) so the small-caster SHADOW cull runs on that pass and nowhere
		// else. Pure size math -- NOT a MOC occlusion test (there is no shadow-view buffer).
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

		g_buildDone.store(frame, std::memory_order_release);
		return true;
	}

	void HiZPrepass()
	{
		// RENDER THREAD ONLY (immediate context) -- called from Deferred::PrepassPasses, where
		// this frame's depth is fully populated and the camera matrices are final. Never touch
		// the context from cull/job threads (a worker-thread CopyResource killed the process).
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
				// Publish the camera near/far the depth was projected with (for GPU-side NDC-z reconstruction),
				// then stamp the build frame (release) -- GPU consumers gate on a one-frame lag.
				const RE::NiFrustum& fr = cam->GetRuntimeData2().viewFrustum;
				g_hizNear = fr.fNear;
				g_hizFar = fr.fFar;
				g_hizGridFrame.store(globals::state->frameCountAtomic.load(std::memory_order_relaxed),
					std::memory_order_release);
			}
		}
	}

	bool TestObject(RE::NiAVObject* a_object)
	{
		if (!g_init || !EnableOcclusionTesting || !a_object)
			return true;

		// CHEAP-FIRST gate order: this runs ~8k times per frame, so plain member reads come
		// before anything virtual and all RTTI stays at the very end, paid only by survivors.
		if (a_object->worldBound.radius < OccluderTestMinRadius)
			return true;

		if (a_object->GetAppCulled())
			return true;

		// Never occlusion-test ACTORS: skinned world bounds lag animation and wrongly culling
		// an NPC is the most visible artifact possible. formType is a plain member read.
		if (auto* ref = a_object->GetUserData(); ref && ref->formType == RE::FormType::ActorCharacter)
			return true;

		if (g_hizMode) {
			if (!g_hizFront.load(std::memory_order_acquire))
				return true;  // no Hi-Z readback published yet -- keep (conservative)
		}

		auto* aabb = GetAABBNode(a_object);  // the single RTTI lookup, survivors only
		return aabb ? TestAABB(aabb) : TestSphere(a_object);
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
		}
		return TestAABB(aabb);
	}

	bool IsSceneListCamera(const RE::NiCamera* a_camera)
	{
		return a_camera && a_camera == GetMainCamera();
	}

	bool IsSunGatherCamera(const RE::NiCamera* a_camera)
	{
		return a_camera && a_camera == g_sunGatherCam.load(std::memory_order_acquire);
	}

	bool TestShadowCasterSmall(RE::NiAVObject* a_object)
	{
		if (!CullSmallShadows || !a_object)
			return true;
		return SmallCullKeeps(a_object, SmallShadowMinSize, SmallShadowSlope);
	}
}
