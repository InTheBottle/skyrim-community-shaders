// GrassCullArgsCS.hlsl — fill the per-batch DrawIndexedInstancedIndirect args from the survivor counters.
// One thread per batch. Args layout: {IndexCountPerInstance, InstanceCount, StartIndexLocation,
// BaseVertexLocation, StartInstanceLocation}. The per-batch survivor stream is addressed by the vertex-buffer
// bind offset on the CPU side, so StartInstanceLocation stays 0 here.

struct BatchDesc
{
	uint srcOffset;
	uint count;
	uint dstOffset;
	uint triCount;
};

cbuffer ArgsParams : register(b0)
{
	uint g_BatchCount;
	uint3 g_pad;
};

StructuredBuffer<BatchDesc> g_Batches : register(t0);
StructuredBuffer<uint>      g_Counters : register(t1);
RWByteAddressBuffer         g_Args : register(u0);  // g_BatchCount * 5 uints

[numthreads(64, 1, 1)] void main(uint3 tid
								 : SV_DispatchThreadID) {
	const uint b = tid.x;
	if (b >= g_BatchCount)
		return;
	const uint base = b * 20u;  // 5 uints * 4 bytes
	g_Args.Store(base + 0u, g_Batches[b].triCount * 3u);  // IndexCountPerInstance
	g_Args.Store(base + 4u, g_Counters[b]);               // InstanceCount
	g_Args.Store(base + 8u, 0u);                          // StartIndexLocation
	g_Args.Store(base + 12u, 0u);                         // BaseVertexLocation
	g_Args.Store(base + 16u, 0u);                         // StartInstanceLocation
}
