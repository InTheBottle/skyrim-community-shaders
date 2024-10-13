
#include "Common/Color.hlsli"
#include "Common/DICETonemapper.hlsli"

Texture2D<float4> UI : register(t0);

RWTexture2D<float4> BackBuffer : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID
								: SV_DispatchThreadID) {
	float3 backbuffer = BackBuffer[dispatchID.xy];
	float4 ui = UI[dispatchID.xy];

	ui.xyz = Color::GammaToLinear(ui.xyz);
	ui.xyz *= 200.0 / 100.0;
	ui.xyz = Color::LinearToGamma(ui.xyz);

	backbuffer = ui.xyz + backbuffer * (1.0 - ui.w);

	backbuffer = sign(backbuffer) * Color::GammaToLinear(abs(backbuffer));
	backbuffer *= 203.0 / 80.0;

	BackBuffer[dispatchID.xy] = float4(backbuffer, 1.0);
};