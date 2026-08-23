Texture2D<float4> t0 : register(t0);

SamplerState s0_s : register(s0);

cbuffer cb2 : register(b2)
{
  float4 cb2[2];
}

cbuffer cb12 : register(b12)
{
  float4 cb12[45];
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
  float4 r0,r1,r2,r3;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = cmp(0.5 < cb2[1].w);
  r0.yz = cb2[1].xy + -v1.xy;
  r0.w = dot(r0.yz, r0.yz);
  r1.x = sqrt(r0.w);
  r1.y = -cb2[0].z + r1.x;
  r1.y = max(0, r1.y);
  r1.z = cmp(0 < r1.y);
  r1.y = cb2[0].y * r1.y + 1;
  r1.y = -1 / r1.y;
  r1.y = 1 + r1.y;
  r1.y = cb2[0].x * r1.y;
  r1.y = r1.z ? r1.y : 0;
  r1.x = -cb2[1].z + r1.x;
  r1.x = max(0, r1.x);
  r1.z = cmp(0 < r1.x);
  r1.x = cb2[0].w * r1.x + 1;
  r1.x = -1 / r1.x;
  r1.x = 1 + r1.x;
  r1.x = cb2[0].x * r1.x;
  r1.x = r1.z ? r1.x : 0;
  r0.w = rsqrt(r0.w);
  r0.yz = r0.yz * r0.ww;
  r0.w = r1.y + -r1.x;
  r0.w = max(0, r0.w);
  r0.yz = r0.yz * r0.ww;
  r0.yz = float2(0.100000001,0.100000001) * r0.yz;
  r1.xyzw = float4(0,0,0,0);
  r0.w = -10;
  while (true) {
    r2.x = cmp(10 < r0.w);
    if (r2.x != 0) break;
    r2.xy = r0.yz * r0.ww + v1.xy;
    if (r0.x != 0) {
      r2.zw = cb12[43].xy * r2.xy;
      r2.zw = max(float2(0,0), r2.zw);
      r3.x = min(cb12[44].z, r2.z);
      r3.y = min(cb12[43].y, r2.w);
      r3.xyzw = t0.SampleLevel(s0_s, r3.xy, 0).xyzw;
    } else {
      r3.xyzw = t0.SampleLevel(s0_s, r2.xy, 0).xyzw;
    }
    r1.xyzw = r3.xyzw + r1.xyzw;
    r0.w = 1 + r0.w;
  }
  o0.xyzw = float4(0.0476190485,0.0476190485,0.0476190485,0.0476190485) * r1.xyzw;
  return;
}