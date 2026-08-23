Texture2D<float4> t1 : register(t1);

Texture2D<float4> t0 : register(t0);

SamplerState s0_s : register(s0);

cbuffer cb2 : register(b2)
{
  float4 cb2[22];
}

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
  float2 v1 : TEXCOORD0,
  float w1 : TEXCOORD1,
  float w1 : TEXCOORD2,
  float3 v2 : COLOR0,
  float3 v3 : TEXCOORD3,
  float3 v4 : POSITION1,
  float3 v5 : POSITION2,
  out float4 o0 : SV_Target0,
  out float2 o1 : SV_Target1,
  out float4 o2 : SV_Target2)
{
  float4 r0,r1,r2;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = w1.x;
  r1.xyzw = t0.Sample(s0_s, v1.xy).xyzw;
  r0.x = r0.x * r1.w + -cb11[0].x;
  r0.x = cmp(r0.x < 0);
  if (r0.x != 0) discard;
  r0.xy = (int2)v0.xy;
  r0.zw = float2(0,0);
  r0.x = t1.Load(r0.xyz).x;
  r0.y = 1 + -r0.x;
  r0.x = cb2[21].w * r0.y + r0.x;
  r0.yzw = v2.xyz * w1.xzz;
  r0.yzw = cb2[19].xyz * r0.yzw;
  r0.yzw = r0.yzw * r1.xyz;
  r2.xyz = cb2[20].xyz * v2.xyz;
  r1.xyz = r2.xyz * r1.xyz;
  r0.xyz = r0.yzw * r0.xxx + r1.xyz;
  o0.xyz = cb2[16].www * r0.xyz;
  o0.w = 1;
  r0.xyz = v4.xyz;
  r0.w = 1;
  r1.x = dot(cb12[12].xyzw, r0.xyzw);
  r1.y = dot(cb12[13].xyzw, r0.xyzw);
  r0.x = dot(cb12[15].xyzw, r0.xyzw);
  r0.xy = r1.xy / r0.xx;
  r1.xyz = v5.xyz;
  r1.w = 1;
  r2.x = dot(cb12[16].xyzw, r1.xyzw);
  r2.y = dot(cb12[17].xyzw, r1.xyzw);
  r0.z = dot(cb12[19].xyzw, r1.xyzw);
  r0.zw = r2.xy / r0.zz;
  r0.xy = r0.xy + -r0.zw;
  o1.xy = float2(-0.5,0.5) * r0.xy;
  r0.xyz = ddx_coarse(v3.zxy);
  r1.xyz = ddy_coarse(v3.yzx);
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