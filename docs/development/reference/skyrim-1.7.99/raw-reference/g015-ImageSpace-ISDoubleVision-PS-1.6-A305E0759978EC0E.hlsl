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
  float4 r0,r1,r2;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = 1 + -cb2[1].w;
  r0.yz = -cb2[1].xy + v1.xy;
  r1.y = max(r0.z, r0.x);
  r1.x = max(0, r0.y);
  r0.xy = cb12[43].xy * r1.xy;
  r0.xy = max(float2(0,0), r0.xy);
  r1.x = cb12[44].z;
  r1.y = cb12[43].y;
  r0.xy = min(r1.xy, r0.xy);
  r0.xyz = t0.Sample(s0_s, r0.xy).xyz;
  r1.zw = cb2[1].xy + v1.xy;
  r2.x = min(cb2[1].z, r1.z);
  r2.y = min(1, r1.w);
  r1.zw = cb12[43].xy * r2.xy;
  r1.zw = max(float2(0,0), r1.zw);
  r1.zw = min(r1.zw, r1.xy);
  r2.xyz = t0.Sample(s0_s, r1.zw).xyz;
  r0.xyz = r2.xyz + r0.xyz;
  r0.w = cb2[1].z / cb2[1].w;
  r1.zw = float2(-0.5,-0.5) + v1.xy;
  r0.w = dot(r0.ww, r1.zz);
  r1.z = r1.w + r1.w;
  r1.z = r1.z * r1.z;
  r0.w = r0.w * r0.w + r1.z;
  r0.w = sqrt(r0.w);
  r0.w = cb2[0].z * r0.w;
  r0.w = min(1, r0.w);
  r1.z = 1 + -r0.w;
  r0.xyz = r1.zzz * r0.xyz;
  r1.zw = cb12[43].xy * v1.xy;
  r1.zw = max(float2(0,0), r1.zw);
  r1.xy = min(r1.zw, r1.xy);
  r1.xyz = t1.Sample(s1_s, r1.xy).xyz;
  r1.xyz = r1.xyz * r0.www;
  o0.xyz = r0.xyz * float3(0.5,0.5,0.5) + r1.xyz;
  o0.w = 1;
  return;
}