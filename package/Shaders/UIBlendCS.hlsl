
#include "Common/Color.hlsli"
#include "Common/DICETonemapper.hlsli"

Texture2D<float4> UI : register(t0);

RWTexture2D<float4> FrameBuffer : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID
								: SV_DispatchThreadID) {
	float3 framebuffer = FrameBuffer[dispatchID.xy];
	float4 ui = UI[dispatchID.xy];

	ui.xyz = Color::GammaToLinear(ui.xyz);
	ui.xyz *= 200.0 / 100.0;
	ui.xyz = Color::LinearToGamma(ui.xyz);

	framebuffer = ui.xyz + framebuffer * (1.0 - ui.w);

	FrameBuffer[dispatchID.xy] = float4(framebuffer, 1.0);
};