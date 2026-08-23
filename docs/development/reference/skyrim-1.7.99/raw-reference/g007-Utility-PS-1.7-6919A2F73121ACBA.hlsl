Texture2DArray<float4> t4 : register(t4);

Texture2D<float4> t2 : register(t2);

SamplerComparisonState s4_s : register(s4);

SamplerState s2_s : register(s2);

cbuffer cb2 : register(b2)
{
  float4 cb2[25];
}

cbuffer cb0 : register(b0)
{
  float4 cb0[4];
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
  float4 r0,r1,r2,r3,r4,r5,r6;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = v0.xy * cb0[0].xy + cb0[0].zw;
  r1.z = t2.Sample(s2_s, r0.xy).x;
  r0.z = cmp(cb0[2].z >= r1.z);
  if (r0.z != 0) {
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
    r1.y = cmp(2.5 < cb0[2].w);
    r2.xy = cmp(cb0[2].yx < r1.zz);
    r1.y = r1.y ? r2.x : 0;
    r3.xyzw = r2.yyyy ? cb2[19].xyzw : cb2[16].xyzw;
    r4.xyzw = r2.yyyy ? cb2[20].xyzw : cb2[17].xyzw;
    r5.xyzw = r2.yyyy ? cb2[21].xyzw : cb2[18].xyzw;
    r1.w = cb2[2].z;
    r2.z = r2.y ? r1.w : cb2[2].y;
    r2.w = r2.y ? 1.000000 : 0;
    r3.xyzw = r1.yyyy ? cb2[22].xyzw : r3.xyzw;
    r4.xyzw = r1.yyyy ? cb2[23].xyzw : r4.xyzw;
    r5.xyzw = r1.yyyy ? cb2[24].xyzw : r5.xyzw;
    r6.z = cb2[2].z;
    r6.w = 2;
    r2.zw = r1.yy ? r6.zw : r2.zw;
    r0.w = 1;
    r3.x = dot(r3.xyzw, r0.xyzw);
    r3.y = dot(r4.xyzw, r0.xyzw);
    r1.y = dot(r5.xyzw, r0.xyzw);
    r1.y = r1.y + -r2.z;
    r1.w = 0;
    r2.z = 0;
    while (true) {
      r3.z = cmp((int)r2.z >= 3);
      if (r3.z != 0) break;
      r3.z = (int)r2.z;
      r4.x = -1 + r3.z;
      r3.z = r1.w;
      r3.w = 0;
      while (true) {
        r4.z = cmp((int)r3.w >= 3);
        if (r4.z != 0) break;
        r4.z = (int)r3.w;
        r4.y = -1 + r4.z;
        r2.xy = r4.xy * cb0[1].zw + r3.xy;
        r2.x = t4.SampleCmpLevelZero(s4_s, r2.xyw, r1.y).x;
        r3.z = r3.z + r2.x;
        r3.w = (int)r3.w + 1;
      }
      r1.w = r3.z;
      r2.z = (int)r2.z + 1;
    }
    r1.y = 0.111111112 * r1.w;
    r1.w = cmp(r2.w < 1);
    r2.x = cmp(cb0[3].y < r1.z);
    r1.w = r1.w ? r2.x : 0;
    if (r1.w != 0) {
      r2.x = dot(cb2[19].xyzw, r0.xyzw);
      r2.y = dot(cb2[20].xyzw, r0.xyzw);
      r0.x = dot(cb2[21].xyzw, r0.xyzw);
      r0.x = -cb2[2].z + r0.x;
      r3.z = 1;
      r0.yz = float2(0,0);
      while (true) {
        r0.w = cmp((int)r0.z >= 3);
        if (r0.w != 0) break;
        r0.w = (int)r0.z;
        r4.x = -1 + r0.w;
        r0.w = r0.y;
        r1.w = 0;
        while (true) {
          r2.z = cmp((int)r1.w >= 3);
          if (r2.z != 0) break;
          r2.z = (int)r1.w;
          r4.y = -1 + r2.z;
          r3.xy = r4.xy * cb0[1].zw + r2.xy;
          r2.z = t4.SampleCmpLevelZero(s4_s, r3.xyz, r0.x).x;
          r0.w = r2.z + r0.w;
          r1.w = (int)r1.w + 1;
        }
        r0.y = r0.w;
        r0.z = (int)r0.z + 1;
      }
      r0.x = -cb0[3].y + cb0[2].x;
      r0.z = -cb0[3].y + r1.z;
      r0.x = 1 / r0.x;
      r0.x = saturate(r0.z * r0.x);
      r0.z = r0.x * -2 + 3;
      r0.x = r0.x * r0.x;
      r0.x = r0.z * r0.x;
      r0.y = r0.y * 0.111111112 + -r1.y;
      r1.y = r0.x * r0.y + r1.y;
    }
    r0.x = -1 + r1.y;
    r0.xyzw = r1.xxxx * r0.xxxx + float4(1,1,1,1);
  } else {
    r0.xyzw = float4(1,1,1,1);
  }
  r1.x = -cb11[0].x + r0.w;
  r1.x = cmp(r1.x < 0);
  if (r1.x != 0) discard;
  o0.xyzw = r0.xyzw;
  return;
}