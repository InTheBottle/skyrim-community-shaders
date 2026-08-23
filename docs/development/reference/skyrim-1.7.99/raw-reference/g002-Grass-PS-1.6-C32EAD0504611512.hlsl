Texture2D<float4> t0 : register(t0);

SamplerState s0_s : register(s0);

cbuffer cb11 : register(b11)
{
  float4 cb11[1];
}




// 3Dmigoto declarations
#define cmp -
Texture1D<float4> IniParams : register(t120);
Texture2D<float4> StereoParams : register(t125);


void main(
  float4 v0 : SV_POSITION0,
  float4 v1 : COLOR0,
  float4 v2 : TEXCOORD0,
  float4 v3 : TEXCOORD1,
  float4 v4 : TEXCOORD2,
  float2 v5 : TEXCOORD3,
  float4 v6 : POSITION1,
  float4 v7 : POSITION2,
  out float4 o0 : SV_Target0)
{
  float4 r0;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = t0.Sample(s0_s, v2.xy).w;
  r0.y = v1.w * r0.x + -cb11[0].x;
  r0.x = v1.w * r0.x;
  o0.w = r0.x;
  r0.x = cmp(r0.y < 0);
  if (r0.x != 0) discard;
  o0.xyz = v5.xxx / v5.yyy;
  return;
}