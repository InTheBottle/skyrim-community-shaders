
#include "Common/Color.hlsli"
#include "Common/DICETonemapper.hlsli"

Texture2D<float4> HDRInput : register(t0);
Texture2D<float4> UI : register(t1);

RWTexture2D<float4> HDROutput : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID
								: SV_DispatchThreadID) {
	float3 framebuffer = HDRInput[dispatchID.xy];
	float4 ui = UI[dispatchID.xy];

	float peakWhite = 1000.0 / 203.0;

	framebuffer = Color::GammaToLinear(framebuffer);
	framebuffer *= peakWhite;
	framebuffer = Color::LinearToGamma(framebuffer);

	// Apply the Reinhard tonemapper on any background color in excess, to avoid it burning it through the UI.
	float3 excessBackgroundColor = framebuffer - min(1.0, framebuffer);
	float3 tonemappedBackgroundColor = excessBackgroundColor / (1.0 + excessBackgroundColor);
	framebuffer = min(1.0, framebuffer) + lerp(tonemappedBackgroundColor, excessBackgroundColor, 1.0 - ui.a);
	
	// Blend UI
	framebuffer = ui.xyz + framebuffer * (1.0 - ui.a);
	
	framebuffer = Color::GammaToLinear(framebuffer);
	framebuffer = BT709ToBT2020(framebuffer);
	framebuffer = LinearToPQ(framebuffer * 100.0, 10000.0);

	HDROutput[dispatchID.xy] = float4(framebuffer, 1.0);
};