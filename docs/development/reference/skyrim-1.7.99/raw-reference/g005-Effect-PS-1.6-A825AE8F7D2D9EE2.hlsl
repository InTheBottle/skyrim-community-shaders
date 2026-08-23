cbuffer cb2 : register(b2)
{
  float4 cb2[9];
}

cbuffer cb1 : register(b1)
{
  float4 cb1[3];
}

cbuffer cb12 : register(b12)
{
  float4 cb12[20];
}




// 3Dmigoto declarations
#define cmp -
Texture1D<float4> IniParams : register(t120);
Texture2D<float4> StereoParams : register(t125);


void main(
  float4 v0 : SV_POSITION0,
  float4 v1 : TEXCOORD0,
  float v2 : TEXCOORD5,
  float3 w2 : TEXCOORD7,
  float4 v3 : POSITION1,
  float4 v4 : POSITION2,
  out float4 o0 : SV_Target0,
  out float2 o1 : SV_Target1,
  out float4 o2 : SV_Target2)
{
  float4 r0,r1;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = 1;
  r0.w = v1.z;
  r0.xyzw = cb1[0].xyzw * r0.xxxw;
  r1.xyz = cb2[8].xyz * r0.xyz + -r0.xyz;
  o0.xyz = cb1[2].xxx * r1.xyz + r0.xyz;
  o0.w = cb2[8].w * r0.w;
  r0.x = dot(cb12[12].xyzw, v3.xyzw);
  r0.y = dot(cb12[13].xyzw, v3.xyzw);
  r0.z = dot(cb12[15].xyzw, v3.xyzw);
  r0.xy = r0.xy / r0.zz;
  r1.x = dot(cb12[16].xyzw, v4.xyzw);
  r1.y = dot(cb12[17].xyzw, v4.xyzw);
  r0.z = dot(cb12[19].xyzw, v4.xyzw);
  r0.zw = r1.xy / r0.zz;
  r0.xy = r0.xy + -r0.zw;
  o1.xy = float2(-0.5,0.5) * r0.xy;
  r0.x = dot(w2.xyz, w2.xyz);
  r0.x = rsqrt(r0.x);
  r0.xyz = w2.xyz * r0.xxx;
  r0.z = r0.z * -8 + 8;
  r0.z = sqrt(r0.z);
  r0.z = max(0.00100000005, r0.z);
  r0.xy = r0.xy / r0.zz;
  o2.xy = float2(0.5,0.5) + r0.xy;
  o2.zw = float2(0,0);
  return;
}