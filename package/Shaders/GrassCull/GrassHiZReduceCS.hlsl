// GrassHiZReduceCS.hlsl -- max-reduce a full-res scene depth into a coarse (1 texel / 16x16 block) grid.
//
// Run at the FIRST grass draw of the z-prepass, on a COPY of the scene depth taken at that moment: opaque
// geometry (terrain, rocks, tree trunks) is already drawn but grass is NOT, so the grid is an OPAQUE-ONLY
// occluder set. Grass is then culled only when it is behind a SOLID surface -- never behind neighbouring
// grass (which is not in the grid) -- so there is no grass-vs-grass over-cull and no frame-lag.
//
// STANDARD Z (near=0 / far=1): the grid stores the FARTHEST (max) depth per block, matching the grass cull's
// tip-cell occlusion test. One 8x8 group covers a 16x16 block (each thread reads a 2x2 quad).

Texture2D<float>   SrcDepth : register(t0);
RWTexture2D<float> OutGrid  : register(u0);
groupshared float  gs[8][8];

[numthreads(8, 8, 1)] void main(uint2 gid
								: SV_GroupID, uint2 tid
								: SV_GroupThreadID) {
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
