
#include "Common/Color.hlsli"
#include "Common/DICETonemapper.hlsli"

Texture2D<float4> Framebuffer : register(t0);
Texture2D<float4> UI : register(t1);

RWTexture2D<float4> HDROutput : register(u0);

cbuffer PerFrame : register(b0)
{
	float4 HDRData;
};

[numthreads(8, 8, 1)] void main(uint3 dispatchID
								: SV_DispatchThreadID) {
	float3 framebuffer = Framebuffer[dispatchID.xy];
	float4 ui = UI[dispatchID.xy];

	// Scale framebuffer and UI brightness relative to game brightness
	framebuffer = Color::GammaToLinear(framebuffer);
	framebuffer *= HDRData.y / HDRData.z;
	framebuffer = Color::LinearToGamma(framebuffer);

	ui.xyz = Color::GammaToLinear(ui.xyz);
	ui.xyz *= HDRData.w / HDRData.z;
	ui.xyz = Color::LinearToGamma(ui.xyz);

	// Apply the Reinhard tonemapper on any background color in excess, to avoid it burning it through the UI.
	float3 excessBackgroundColor = framebuffer - min(1.0, framebuffer);
	float3 tonemappedBackgroundColor = excessBackgroundColor / (1.0 + excessBackgroundColor);
	framebuffer = min(1.0, framebuffer) + lerp(tonemappedBackgroundColor, excessBackgroundColor, 1.0 - ui.a);
	
	// Blend UI
	framebuffer = ui.xyz + framebuffer * (1.0 - ui.a);
	
	framebuffer = Color::GammaToLinear(framebuffer);
	
	// Scale framebuffer to game brightness
	framebuffer *= HDRData.z;

	framebuffer = BT709ToBT2020(framebuffer);
	framebuffer = LinearToPQ(framebuffer, 10000.0);

	HDROutput[dispatchID.xy] = float4(framebuffer, 1.0);
};