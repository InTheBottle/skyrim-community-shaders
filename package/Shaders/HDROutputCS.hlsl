
#include "Common/Color.hlsli"
#include "Common/DICETonemapper.hlsli"

Texture2D<float4> BackBuffer : register(t0);
Texture2D<float4> UI : register(t1);

RWTexture2D<float4> HDRTexture : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID
								: SV_DispatchThreadID) {
  float3 backbuffer = BackBuffer[dispatchID.xy];
  backbuffer = PQToLinear(backbuffer, 1.0) * (10000.0 / 203.0);
  backbuffer = BT2020ToBT709(backbuffer);
  backbuffer = sign(backbuffer) * Color::LinearToGamma(abs(backbuffer));
  
  float4 ui = UI[dispatchID.xy];

  float3 finalLinearColor = ui.xyz + backbuffer * (1.0 - ui.w);

  finalLinearColor = sign(finalLinearColor) * Color::GammaToLinear(abs(finalLinearColor));
  finalLinearColor = BT709ToBT2020(finalLinearColor);
  finalLinearColor = LinearToPQ(finalLinearColor / (10000.0 / 203.0), 1.0);

  HDRTexture[dispatchID.xy] = float4(finalLinearColor, 1.0);
};