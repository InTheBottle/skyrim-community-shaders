Texture2D<float4> t0 : register(t0);

SamplerState s0_s : register(s0);

cbuffer cb2 : register(b2)
{
  float4 cb2[3];
}

cbuffer cb12 : register(b12)
{
  float4 cb12[44];
}




// 3Dmigoto declarations
#define cmp -
Texture1D<float4> IniParams : register(t120);
Texture2D<float4> StereoParams : register(t125);


void main(
  float4 v0 : SV_POSITION0,
  float2 v1 : TEXCOORD0,
  out float4 o0 : SV_Target0)
{
  float4 r0,r1;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = cb12[43].xy * v1.xy;
  r0.zw = cb2[0].yx * r0.yx;
  r0.zw = (int2)r0.zw;
  r0.zw = (int2)r0.zw & int2(1,1);
  r0.zw = (int2)r0.zw ^ int2(1,1);
  r0.zw = (int2)r0.zw;
  r0.zw = r0.zw * float2(2,2) + float2(-1,-1);
  r0.xy = r0.zw * cb2[0].zw + r0.xy;
  r0.xy = max(float2(0,0), r0.xy);
  r1.x = -cb12[41].w;
  r1.y = -0;
  r0.zw = cb12[43].xy + r1.xy;
  r0.xy = min(r0.xy, r0.zw);
  r0.zw = cb2[0].yx * v1.yx;
  r0.zw = (int2)r0.zw;
  r0.zw = (int2)r0.zw & int2(1,1);
  r0.zw = (int2)r0.zw ^ int2(1,1);
  r0.zw = (int2)r0.zw;
  r0.zw = r0.zw * float2(2,2) + float2(-1,-1);
  r0.zw = r0.zw * cb2[0].zw + v1.xy;
  r1.x = cmp(0 < asuint(cb2[2].x));
  r0.xy = r1.xx ? r0.xy : r0.zw;
  o0.xyzw = t0.Sample(s0_s, r0.xy).xyzw;
  return;
}