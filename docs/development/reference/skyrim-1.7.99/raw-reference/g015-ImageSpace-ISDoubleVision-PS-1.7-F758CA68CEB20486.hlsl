Texture2D<float4> t1 : register(t1);

Texture2D<float4> t0 : register(t0);

SamplerState s1_s : register(s1);

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
  float4 r0,r1,r2,r3,r4;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = cb2[1].xy + v1.xy;
  r1.x = min(cb2[1].z, r0.x);
  r1.y = min(1, r0.y);
  r0.xy = -cb2[1].xy + v1.xy;
  r2.x = max(0, r0.x);
  r0.x = 1 + -cb2[1].w;
  r2.y = max(r0.y, r0.x);
  r0.x = cmp(0.5 < cb2[0].x);
  if (r0.x != 0) {
    r0.xy = cb12[43].xy * r1.xy;
    r0.xy = max(float2(0,0), r0.xy);
    r3.x = cb12[44].z;
    r3.y = cb12[43].y;
    r0.xy = min(r3.xy, r0.xy);
    r0.xyz = t0.Sample(s0_s, r0.xy).xyz;
    r1.zw = cb12[43].xy * r2.xy;
    r1.zw = max(float2(0,0), r1.zw);
    r1.zw = min(r1.zw, r3.xy);
    r4.xyz = t0.Sample(s0_s, r1.zw).xyz;
    r1.zw = cb12[43].xy * v1.xy;
    r1.zw = max(float2(0,0), r1.zw);
    r1.zw = min(r1.zw, r3.xy);
    r3.xyz = t1.Sample(s1_s, r1.zw).xyz;
  } else {
    r0.xyz = t0.Sample(s0_s, r1.xy).xyz;
    r4.xyz = t0.Sample(s0_s, r2.xy).xyz;
    r3.xyz = t1.Sample(s1_s, v1.xy).xyz;
  }
  r0.w = cb2[1].z / cb2[1].w;
  r1.xy = float2(-0.5,-0.5) + v1.xy;
  r0.w = dot(r0.ww, r1.xx);
  r1.x = r1.y + r1.y;
  r1.x = r1.x * r1.x;
  r0.w = r0.w * r0.w + r1.x;
  r0.w = sqrt(r0.w);
  r0.w = cb2[0].z * r0.w;
  r0.w = min(1, r0.w);
  r0.xyz = r4.xyz + r0.xyz;
  r1.x = 1 + -r0.w;
  r0.xyz = r1.xxx * r0.xyz;
  r1.xyz = r3.xyz * r0.www;
  o0.xyz = r0.xyz * float3(0.5,0.5,0.5) + r1.xyz;
  o0.w = 1;
  return;
}