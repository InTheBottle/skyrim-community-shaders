Texture2D<float4> t1 : register(t1);

Texture2D<float4> t0 : register(t0);

SamplerState s0_s : register(s0);

cbuffer cb12 : register(b12)
{
  float4 cb12[20];
}

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
  float3 v4 : TEXCOORD2,
  float4 v5 : POSITION1,
  float4 v6 : POSITION2,
  out float4 o0 : SV_Target0,
  out float2 o1 : SV_Target1,
  out float4 o2 : SV_Target2)
{
  float4 r0,r1,r2;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xyzw = t0.Sample(s0_s, v2.xy).xyzw;
  r0.w = v1.w * r0.w + -cb11[0].x;
  r0.w = cmp(r0.w < 0);
  if (r0.w != 0) discard;
  r1.xy = (int2)v0.xy;
  r1.zw = float2(0,0);
  r0.w = t1.Load(r1.xyz).x;
  r1.x = 1 + -r0.w;
  r0.w = v3.w * r1.x + r0.w;
  r1.xyz = v1.xyz * r0.xyz;
  r0.xyz = v3.xyz * r0.xyz;
  r0.xyz = r1.xyz * r0.www + r0.xyz;
  o0.xyz = v2.zzz * r0.xyz;
  o0.w = 1;
  r0.x = dot(cb12[12].xyzw, v5.xyzw);
  r0.y = dot(cb12[13].xyzw, v5.xyzw);
  r0.z = dot(cb12[15].xyzw, v5.xyzw);
  r0.xy = r0.xy / r0.zz;
  r1.x = dot(cb12[16].xyzw, v6.xyzw);
  r1.y = dot(cb12[17].xyzw, v6.xyzw);
  r0.z = dot(cb12[19].xyzw, v6.xyzw);
  r0.zw = r1.xy / r0.zz;
  r0.xy = r0.xy + -r0.zw;
  o1.xy = float2(-0.5,0.5) * r0.xy;
  r0.xyz = ddx_coarse(v4.zxy);
  r1.xyz = ddy_coarse(v4.yzx);
  r2.xyz = r1.xyz * r0.xyz;
  r0.xyz = r0.zxy * r1.yzx + -r2.xyz;
  r0.w = dot(r0.xyz, r0.xyz);
  r0.w = rsqrt(r0.w);
  r0.xyz = r0.xyz * r0.www;
  r0.z = r0.z * -8 + 8;
  r0.z = sqrt(r0.z);
  r0.z = max(0.00100000005, r0.z);
  r0.xy = r0.xy / r0.zz;
  o2.xy = float2(0.5,0.5) + r0.xy;
  o2.zw = float2(0,0);
  return;
}