// Separable Gaussian blur for shadow map moments
// BLUR_HORIZONTAL - horizontal pass
// BLUR_VERTICAL - vertical pass

Texture2D<float4> InputTexture : register(t0);
RWTexture2D<float4> OutputTexture : register(u0);

cbuffer BlurCB : register(b0)
{
	uint BlurRadius;
	uint _pad[3];
};

#define MAX_KERNEL_RADIUS 32
#define GROUP_SIZE 128

// Shared memory for efficient loading
// We need GROUP_SIZE + 2 * MAX_KERNEL_RADIUS elements
groupshared float4 g_cache[GROUP_SIZE + 2 * MAX_KERNEL_RADIUS];

#if defined(BLUR_HORIZONTAL)
[numthreads(GROUP_SIZE, 1, 1)] void main(uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID, uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint width, height;
	InputTexture.GetDimensions(width, height);

	int2 baseCoord = int2(groupID.x * GROUP_SIZE - MAX_KERNEL_RADIUS, groupID.y);
	int localIdx = groupThreadID.x;

	// Load main data
	int2 coord = baseCoord + int2(localIdx, 0);
	coord.x = clamp(coord.x, 0, (int)width - 1);
	g_cache[localIdx] = InputTexture[coord];

	// Load extra data for kernel overlap
	if (localIdx < 2 * MAX_KERNEL_RADIUS) {
		coord = baseCoord + int2(GROUP_SIZE + localIdx, 0);
		coord.x = clamp(coord.x, 0, (int)width - 1);
		g_cache[GROUP_SIZE + localIdx] = InputTexture[coord];
	}

	GroupMemoryBarrierWithGroupSync();

	// Only process valid pixels
	if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
		return;

	// Apply horizontal blur with dynamic radius
	uint radius = min(BlurRadius, (uint)MAX_KERNEL_RADIUS);
	float sigma = max(float(radius) * 0.5, 0.5);
	float rcpTwoSigma2 = rcp(2.0 * sigma * sigma);

	float4 result = g_cache[localIdx + MAX_KERNEL_RADIUS];
	float totalWeight = 1.0;

	for (uint i = 1; i <= radius; i++) {
		float w = exp(-float(i * i) * rcpTwoSigma2);
		result += (g_cache[localIdx + MAX_KERNEL_RADIUS - i] + g_cache[localIdx + MAX_KERNEL_RADIUS + i]) * w;
		totalWeight += 2.0 * w;
	}

	OutputTexture[dispatchThreadID.xy] = result * rcp(totalWeight);
}

#elif defined(BLUR_VERTICAL)
[numthreads(1, GROUP_SIZE, 1)] void main(uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID, uint3 dispatchThreadID : SV_DispatchThreadID) {
	uint width, height;
	InputTexture.GetDimensions(width, height);

	int2 baseCoord = int2(groupID.x, groupID.y * GROUP_SIZE - MAX_KERNEL_RADIUS);
	int localIdx = groupThreadID.y;

	// Load main data
	int2 coord = baseCoord + int2(0, localIdx);
	coord.y = clamp(coord.y, 0, (int)height - 1);
	g_cache[localIdx] = InputTexture[coord];

	// Load extra data for kernel overlap
	if (localIdx < 2 * MAX_KERNEL_RADIUS) {
		coord = baseCoord + int2(0, GROUP_SIZE + localIdx);
		coord.y = clamp(coord.y, 0, (int)height - 1);
		g_cache[GROUP_SIZE + localIdx] = InputTexture[coord];
	}

	GroupMemoryBarrierWithGroupSync();

	// Only process valid pixels
	if (dispatchThreadID.x >= width || dispatchThreadID.y >= height)
		return;

	// Apply vertical blur with dynamic radius
	uint radius = min(BlurRadius, (uint)MAX_KERNEL_RADIUS);
	float sigma = max(float(radius) * 0.5, 0.5);
	float rcpTwoSigma2 = rcp(2.0 * sigma * sigma);

	float4 result = g_cache[localIdx + MAX_KERNEL_RADIUS];
	float totalWeight = 1.0;

	for (uint i = 1; i <= radius; i++) {
		float w = exp(-float(i * i) * rcpTwoSigma2);
		result += (g_cache[localIdx + MAX_KERNEL_RADIUS - i] + g_cache[localIdx + MAX_KERNEL_RADIUS + i]) * w;
		totalWeight += 2.0 * w;
	}

	OutputTexture[dispatchThreadID.xy] = result * rcp(totalWeight);
}
#endif
