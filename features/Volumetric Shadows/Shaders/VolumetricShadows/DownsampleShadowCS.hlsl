Texture2DArray<float> InputTexture : register(t0);
Texture2DArray<float> ESRAMShadow : register(t1);
RWTexture2D<float4> OutputTexture : register(u0);
SamplerState LinearSampler : register(s0);

cbuffer EVSMLinearizeCB : register(b0)
{
	float CascadeNear;
	float CascadeFar;
	float GlobalNear;
	float GlobalFar;
	float ExponentPositive;
	float ExponentNegative;
};

// Convert orthographic shadow map depth [0,1] to globally-normalized linear depth [0,1].
// Shadow map depth is linear within each cascade: worldZ = near + depth * (far - near).
// We then remap to a global range shared by both cascades so exponents behave consistently.
float NormalizeDepth(float depth)
{
	float worldZ = CascadeNear + depth * (CascadeFar - CascadeNear);
	return (worldZ - GlobalNear) / (GlobalFar - GlobalNear);
}

// Warp depth into EVSM moments: (e^(c*d), e^(2c*d), e^(-c*d), e^(-2c*d))
// Positive exponent detects front-face occlusion, negative detects back-face (light bleeding).
float4 WarpDepth(float depth)
{
	float d = NormalizeDepth(depth);
	float posWarp = exp(ExponentPositive * d);
	float negWarp = exp(-ExponentNegative * d);
	return float4(posWarp, posWarp * posWarp, negWarp, negWarp * negWarp);
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

	float4 evsmMoments = 0;
	for (uint i = 0; i < 4; i++)
		evsmMoments += WarpDepth(depths[i]);
	evsmMoments *= 0.25;

	g_scratchDepths[groupThreadID.x][groupThreadID.y] = evsmMoments;

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
