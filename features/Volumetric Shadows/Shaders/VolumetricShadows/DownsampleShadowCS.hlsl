Texture2DArray<float> InputTexture : register(t0);
Texture2DArray<float> ESRAMShadow : register(t1);
RWTexture2D<float4> OutputTexture : register(u0);
SamplerState LinearSampler : register(s0);

cbuffer VSMLinearizeCB : register(b0)
{
	float CascadeNear;
	float CascadeFar;
	float2 _pad;
};

float LinearizeDepth(float depth)
{
	float linZ = CascadeNear * CascadeFar / (CascadeFar - depth * (CascadeFar - CascadeNear));
	return (linZ - CascadeNear) / (CascadeFar - CascadeNear);
}

// Compute 4 power moments with RGBA16 quantization optimization
// Reference: Peters, "Moment Shadow Mapping" (I3D 2015)
float4 GetOptimizedMoments(in float depth)
{
	float d = LinearizeDepth(depth);
	float d2 = d * d;
	float4 moments = float4(d, d2, d * d2, d2 * d2);
	float4 optimized = mul(moments, float4x4(
		-2.07224649,     13.7948857237,   0.105877704,    9.7924062118,
		 32.23703778,   -59.4683975703,  -1.9077466311,  -33.7652110555,
		-68.571074599,   82.0359750338,   9.3496555107,   47.9456096605,
		 39.3703274134, -35.364903257,   -6.6543490743,  -23.9728048165));
	optimized[0] += 0.035955884801;
	return optimized;
}

float4 ReduceMoments(float4 a, float4 b, float4 c, float4 d)
{
	return (a + b + c + d) * 0.25;
}

groupshared float4 g_scratchDepths[8][8];

#if defined(DOWNSAMPLE_SHADOW_MIP0)
static const uint CASCADE = 1;
#elif defined(DOWNSAMPLE_SHADOW_MIP1)
static const uint CASCADE = 0;
#endif

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID, uint3 groupThreadID : SV_GroupThreadID) {
	uint2 pixCoord = dispatchThreadID.xy * 2;

	uint inputW, inputH, inputSlices;
	InputTexture.GetDimensions(inputW, inputH, inputSlices);
	float2 uv = (pixCoord + 0.5) / float2(inputW, inputH);

	uint outputW, outputH;
	OutputTexture.GetDimensions(outputW, outputH);

	// Determine reduction levels from input/output ratio
	// Gather handles 2x, each group reduction handles another 2x
	uint totalReduction = inputW / outputW;
	uint groupReductions = 0;
	if (totalReduction >= 4)
		groupReductions = 1;
	if (totalReduction >= 8)
		groupReductions = 2;
	if (totalReduction >= 16)
		groupReductions = 3;

	// Gather from shadow cascades and mix with ESRAM shadow
	float4 depths = InputTexture.GatherRed(LinearSampler, float3(uv, CASCADE));
	float4 esramDepths = ESRAMShadow.GatherRed(LinearSampler, float3(uv, CASCADE));
	depths = min(depths, esramDepths);

	float4 msmMoments = 0;
	for (uint i = 0; i < 4; i++)
		msmMoments += GetOptimizedMoments(depths[i]);
	msmMoments *= 0.25;

	g_scratchDepths[groupThreadID.x][groupThreadID.y] = msmMoments;

	GroupMemoryBarrierWithGroupSync();

	// First reduction: 2x2
	if (groupReductions >= 1) {
		if (all((groupThreadID.xy % 2) == 0)) {
			uint2 tid = groupThreadID.xy;
			g_scratchDepths[tid.x][tid.y] = ReduceMoments(
				g_scratchDepths[tid.x + 0][tid.y + 0],
				g_scratchDepths[tid.x + 1][tid.y + 0],
				g_scratchDepths[tid.x + 0][tid.y + 1],
				g_scratchDepths[tid.x + 1][tid.y + 1]);
		}
		GroupMemoryBarrierWithGroupSync();
	}

	// Second reduction: 4x4
	if (groupReductions >= 2) {
		if (all((groupThreadID.xy % 4) == 0)) {
			uint2 tid = groupThreadID.xy;
			g_scratchDepths[tid.x][tid.y] = ReduceMoments(
				g_scratchDepths[tid.x + 0][tid.y + 0],
				g_scratchDepths[tid.x + 2][tid.y + 0],
				g_scratchDepths[tid.x + 0][tid.y + 2],
				g_scratchDepths[tid.x + 2][tid.y + 2]);
		}
		GroupMemoryBarrierWithGroupSync();
	}

	// Third reduction: 8x8
	if (groupReductions >= 3) {
		if (all(groupThreadID.xy == 0)) {
			g_scratchDepths[0][0] = ReduceMoments(
				g_scratchDepths[0][0],
				g_scratchDepths[4][0],
				g_scratchDepths[0][4],
				g_scratchDepths[4][4]);
		}
		GroupMemoryBarrierWithGroupSync();
	}

	// Write output - only threads aligned to the output grid
	uint outputDiv = max(totalReduction / 2, 1);
	if (all((groupThreadID.xy % outputDiv) == 0)) {
		OutputTexture[dispatchThreadID.xy / outputDiv] = g_scratchDepths[groupThreadID.x][groupThreadID.y];
	}
}
