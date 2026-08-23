cbuffer cb8 : register(b8)
{
  float4 cb8[240];
}

cbuffer cb7 : register(b7)
{
  float4 cb7[1];
}

cbuffer cb2 : register(b2)
{
  float4 cb2[22];
}




// 3Dmigoto declarations
#define cmp -
Texture1D<float4> IniParams : register(t120);
Texture2D<float4> StereoParams : register(t125);


void main(
  float4 v0 : POSITION0,
  float2 v1 : TEXCOORD0,
  float4 v2 : NORMAL0,
  float4 v3 : COLOR0,
  float4 v4 : TEXCOORD4,
  float4 v5 : TEXCOORD5,
  float4 v6 : TEXCOORD6,
  float4 v7 : TEXCOORD7,
  out float4 o0 : SV_POSITION0,
  out float4 o1 : COLOR0,
  out float4 o2 : TEXCOORD0,
  out float4 o3 : TEXCOORD1,
  out float4 o4 : TEXCOORD2,
  out float2 o5 : TEXCOORD3,
  out float4 o6 : POSITION1,
  out float4 o7 : POSITION2)
{
  const float4 icb[] = { { 1.000000, 0, 0, 0},
                              { 0, 1.000000, 0, 0},
                              { 0, 0, 1.000000, 0},
                              { 0, 0, 0, 1.000000} };
  float4 r0,r1,r2,r3,r4,r5;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = v4.x + v4.y;
  r0.y = -r0.x * 0.0078125 + cb2[17].w;
  r0.x = -r0.x * 0.0078125 + cb2[18].w;
  r0.xy = float2(0.400000006,0.400000006) * r0.xy;
  sincos(r0.x, r0.x, r1.x);
  sincos(r0.y, r2.x, r3.x);
  r0.yz = float2(3.1415925,6.28318501) * r2.xx;
  r0.w = 3.1415925 * r3.x;
  r0.w = cos(r0.w);
  r0.w = 0.200000003 * r0.w;
  r0.yz = sin(r0.yz);
  r0.y = r0.y + r0.z;
  r0.y = r0.y * 0.300000012 + r0.w;
  r0.z = v3.w * v3.w;
  r0.z = 0.5 * r0.z;
  r0.y = r0.y * r0.z;
  r0.y = cb2[17].z * r0.y;
  r2.x = v7.x;
  r2.y = v5.w;
  r2.z = v6.w;
  r1.yzw = v7.yyy * cb2[21].xyz + float3(1,1,1);
  r1.yzw = v0.xyz * r1.yzw;
  r2.z = dot(r2.xyz, r1.yzw);
  r2.x = dot(v5.xyz, r1.yzw);
  r2.y = dot(v6.xyz, r1.yzw);
  r3.xy = cb2[17].xy;
  r3.z = 0;
  r1.yzw = r3.xyz * r0.yyy + r2.xyz;
  r4.xyz = v4.xyz + r1.yzw;
  r4.w = 1;
  r5.x = dot(cb2[0].xyzw, r4.xyzw);
  r5.y = dot(cb2[1].xyzw, r4.xyzw);
  r5.w = dot(cb2[3].xyzw, r4.xyzw);
  r5.z = dot(cb2[2].xyzw, r4.xyzw);
  o0.xyzw = r5.xyzw;
  r0.y = dot(r5.xyz, r5.xyz);
  o5.xy = r5.zw;
  r0.y = sqrt(r0.y);
  r0.y = -cb2[19].w + r0.y;
  r0.y = saturate(r0.y / cb2[20].w);
  r0.y = 1 + -r0.y;
  r5.x = v5.z;
  r5.yz = v6.zw;
  r0.w = saturate(dot(cb2[18].xyz, r5.xyz));
  r1.yzw = v4.www * v3.xyz;
  r1.yzw = r1.yzw * r0.www;
  o1.xyz = cb2[19].xyz * r1.yzw;
  r0.w = asuint(cb7[0].x) >> 2;
  r1.y = asint(cb7[0].x) & 3;
  r0.w = dot(cb8[r0.w+0].xyzw, icb[r1.y+0].xyzw);
  o1.w = r0.y * r0.w;
  o2.xy = v1.xy;
  o2.z = cb2[16].w;
  r1.yzw = cb2[20].xyz * v3.xyz;
  o3.xyz = v4.www * r1.yzw;
  o3.w = cb2[21].w;
  o4.x = dot(cb2[4].xyzw, r4.xyzw);
  o4.y = dot(cb2[5].xyzw, r4.xyzw);
  o4.z = dot(cb2[6].xyzw, r4.xyzw);
  o6.x = dot(cb2[8].xyzw, r4.xyzw);
  o6.y = dot(cb2[9].xyzw, r4.xyzw);
  o6.z = dot(cb2[10].xyzw, r4.xyzw);
  o6.w = dot(cb2[11].xyzw, r4.xyzw);
  r0.w = 3.1415925 * r1.x;
  r0.w = cos(r0.w);
  r0.xyw = float3(3.1415925,6.28318501,0.200000003) * r0.xxw;
  r0.xy = sin(r0.xy);
  r0.x = r0.x + r0.y;
  r0.x = r0.x * 0.300000012 + r0.w;
  r0.x = r0.x * r0.z;
  r0.x = cb2[17].z * r0.x;
  r0.xyz = r3.xyz * r0.xxx + r2.xyz;
  r0.xyz = v4.xyz + r0.xyz;
  r0.w = 1;
  o7.x = dot(cb2[12].xyzw, r0.xyzw);
  o7.y = dot(cb2[13].xyzw, r0.xyzw);
  o7.z = dot(cb2[14].xyzw, r0.xyzw);
  o7.w = dot(cb2[15].xyzw, r0.xyzw);
  return;
}