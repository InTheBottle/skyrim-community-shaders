
#include "Common/Color.hlsli"
#include "Common/DICETonemapper.hlsli"

Texture2D<float4> Input : register(t0);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID
								: SV_DispatchThreadID) {
	float3 framebuffer = Input[dispatchID.xy];

	float peakWhite = 1000.0 / 203.0;

	framebuffer = Color::GammaToLinear(framebuffer);
	framebuffer *= peakWhite;
	
	framebuffer = BT709ToBT2020(framebuffer);
 	framebuffer = LinearToPQ(framebuffer * 203.0, 10000.0);

	Output[dispatchID.xy] = float4(framebuffer, 1.0);
};