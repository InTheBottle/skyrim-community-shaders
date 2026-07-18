// ShadowDepthComposite.hlsl — composite cached STATIC shadow depth into a shadow-atlas slice.
//
// Phase 1 primitive for the static/dynamic local-light shadow cache: instead of a raw whole-port CopyPort blit
// (which overwrites live dynamic depth), this pass reads the cached static depth (R16 SRV of the unit's cache
// layer) and re-emits it as SV_Depth over the map's port rect. With a min-depth PSO (LESS_EQUAL standard /
// GREATER reversed-Z) the cached static depth wins only where it is nearer than the dynamic depth already in the
// slice -- so statics render once + blit while actors/trees render live on top every frame.
//
// PROTOTYPE NOTE: validated first with a DepthFunc=ALWAYS PSO on a static-only slice, where it must reproduce the
// CopyPort blit byte-for-byte -- proving the DXVK D16-depth-as-SRV + SV_Depth path is lossless before it is used
// to merge dynamics.

Texture2D<float> CacheDepth : register(t0);  // the unit's cached static depth layer (R16_UNORM view of R16_TYPELESS)

struct VOut
{
	float4 pos : SV_POSITION;
};

// Fullscreen triangle; the viewport is set to the map's port rect so SV_Position.xy lands on atlas pixel coords
// within that port -- the same coords CopyPort wrote the cache at, so the read is 1:1 (no offset math).
VOut vsmain(uint id : SV_VertexID)
{
	VOut o;
	const float2 uv = float2((id << 1) & 2, id & 2);  // (0,0) (2,0) (0,2)
	o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
	return o;
}

float psmain(VOut i) : SV_Depth
{
	return CacheDepth.Load(int3(int2(i.pos.xy), 0));
}
