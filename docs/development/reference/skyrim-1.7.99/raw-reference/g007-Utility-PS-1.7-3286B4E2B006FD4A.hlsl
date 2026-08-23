Texture2DArray<float4> t4 : register(t4);

Texture2D<float4> t2 : register(t2);

SamplerComparisonState s4_s : register(s4);

SamplerState s2_s : register(s2);

cbuffer cb2 : register(b2)
{
  float4 cb2[20];
}

cbuffer cb0 : register(b0)
{
  float4 cb0[3];
}

cbuffer cb12 : register(b12)
{
  float4 cb12[45];
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
  out float4 o0 : SV_Target0)
{
  float4 r0,r1,r2,r3,r4;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = v0.xy * cb0[0].xy + cb0[0].zw;
  r1.z = t2.Sample(s2_s, r0.xy).x;
  r0.x = cb12[44].x * r0.x;
  r0.z = -r0.y * cb12[44].y + 1;
  r1.xy = r0.xz * float2(2,2) + float2(-1,-1);
  r1.w = 1;
  r0.x = dot(cb12[32].xyzw, r1.xyzw);
  r0.y = dot(cb12[33].xyzw, r1.xyzw);
  r0.z = dot(cb12[34].xyzw, r1.xyzw);
  r0.w = dot(cb12[35].xyzw, r1.xyzw);
  r0.xyz = r0.xyz / r0.www;
  r1.x = dot(r0.xyz, r0.xyz);
  r1.x = saturate(r1.x / cb2[3].z);
  r1.x = r1.x * r1.x;
  r1.x = r1.x * r1.x;
  r1.x = -r1.x * r1.x + 1;
  r0.w = 1;
  r2.x = dot(cb2[16].xyzw, r0.xyzw);
  r2.y = dot(cb2[17].xyzw, r0.xyzw);
  r2.z = dot(cb2[18].xyzw, r0.xyzw);
  r0.x = dot(cb2[19].xyzw, r0.xyzw);
  r0.y = r2.z * 0.5 + 0.5;
  r0.xzw = r2.xyz / r0.xxx;
  r1.y = dot(r0.xzw, r0.xzw);
  r1.z = sqrt(r1.y);
  r1.y = rsqrt(r1.y);
  r0.y = cmp(r0.y < 0);
  r2.xyz = r0.yyy ? float3(0,0,-1) : float3(0,0,1);
  r0.xzw = r0.xzw * r1.yyy + r2.xyz;
  r1.y = dot(r0.xzw, r0.xzw);
  r1.y = rsqrt(r1.y);
  r0.xzw = r1.yyy * r0.xzw;
  r0.xz = r0.xz / r0.ww;
  r0.w = saturate(r1.z / cb2[3].x);
  r2.xy = r0.xz * float2(0.5,0.5) + float2(0.5,0.5);
  r0.x = 0.5 * r2.y;
  r0.z = -r2.y * 0.5 + 1;
  r2.z = r0.y ? r0.z : r0.x;
  r0.x = -cb2[2].y + r0.w;
  r3.z = cb0[2].x;
  r0.yz = float2(0,0);
  while (true) {
    r0.w = cmp((int)r0.z >= 3);
    if (r0.w != 0) break;
    r0.w = (int)r0.z;
    r4.x = -1 + r0.w;
    r0.w = r0.y;
    r1.y = 0;
    while (true) {
      r1.z = cmp((int)r1.y >= 3);
      if (r1.z != 0) break;
      r1.z = (int)r1.y;
      r4.y = -1 + r1.z;
      r3.xy = r4.xy * cb0[1].zw + r2.xz;
      r1.z = t4.SampleCmpLevelZero(s4_s, r3.xyz, r0.x).x;
      r0.w = r1.z + r0.w;
      r1.y = (int)r1.y + 1;
    }
    r0.y = r0.w;
    r0.z = (int)r0.z + 1;
  }
  r0.x = r0.y * r1.x;
  r1.xyzw = float4(0.111111112,0.111111112,0.111111112,0.111111112) * r0.xxxx;
  r0.x = r0.x * 0.111111112 + -cb11[0].x;
  r0.x = cmp(r0.x < 0);
  if (r0.x != 0) discard;
  o0.xyzw = r1.xyzw;
  return;
}