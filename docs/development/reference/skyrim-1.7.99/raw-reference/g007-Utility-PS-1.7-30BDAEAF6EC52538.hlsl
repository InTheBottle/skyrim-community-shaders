Texture2DArray<float4> t6 : register(t6);

Texture2D<uint4> t5 : register(t5);

Texture2DArray<float4> t4 : register(t4);

Texture2D<float4> t2 : register(t2);

SamplerComparisonState s6_s : register(s6);

SamplerComparisonState s4_s : register(s4);

SamplerState s2_s : register(s2);

cbuffer cb2 : register(b2)
{
  float4 cb2[25];
}

cbuffer cb0 : register(b0)
{
  float4 cb0[5];
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
  const float4 icb[] = { { 1.000000, 0, 0, 0},
                              { 0, 1.000000, 0, 0},
                              { 0, 0, 1.000000, 0},
                              { 0, 0, 0, 1.000000} };
  float4 r0,r1,r2,r3,r4,r5,r6,r7;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = v0.xy * cb0[0].xy + cb0[0].zw;
  r1.z = t2.Sample(s2_s, r0.xy).x;
  r0.z = cmp(cb0[2].z >= r1.z);
  if (r0.z != 0) {
    t5.GetDimensions(0, uiDest.x, uiDest.y, uiDest.z);
    r0.zw = uiDest.xy;
    r0.zw = (uint2)r0.zw;
    r0.zw = r0.xy * r0.zw;
    r2.xy = (int2)r0.zw;
    r2.zw = float2(0,0);
    r0.z = t5.Load(r2.xyz).x;
    r2.x = cb12[44].x * r0.x;
    r2.z = -r0.y * cb12[44].y + 1;
    r1.xy = r2.xz * float2(2,2) + float2(-1,-1);
    r1.w = 1;
    r2.x = dot(cb12[32].xyzw, r1.xyzw);
    r2.y = dot(cb12[33].xyzw, r1.xyzw);
    r2.z = dot(cb12[34].xyzw, r1.xyzw);
    r0.x = dot(cb12[35].xyzw, r1.xyzw);
    r2.xyz = r2.xyz / r0.xxx;
    r0.x = dot(r2.xyz, r2.xyz);
    r0.x = saturate(r0.x / cb2[3].z);
    r0.x = r0.x * r0.x;
    r0.x = r0.x * r0.x;
    r0.x = -r0.x * r0.x + 1;
    r0.y = cmp(2.5 < cb0[2].w);
    r1.xy = cmp(cb0[2].yx < r1.zz);
    r0.y = r0.y ? r1.x : 0;
    r3.xyzw = r1.yyyy ? cb2[19].xyzw : cb2[16].xyzw;
    r4.xyzw = r1.yyyy ? cb2[20].xyzw : cb2[17].xyzw;
    r5.xyzw = r1.yyyy ? cb2[21].xyzw : cb2[18].xyzw;
    r0.w = cb2[2].z;
    r6.z = r1.y ? r0.w : cb2[2].y;
    r6.w = r1.y ? 1.000000 : 0;
    r3.xyzw = r0.yyyy ? cb2[22].xyzw : r3.xyzw;
    r4.xyzw = r0.yyyy ? cb2[23].xyzw : r4.xyzw;
    r5.xyzw = r0.yyyy ? cb2[24].xyzw : r5.xyzw;
    r7.z = cb2[2].z;
    r7.w = 2;
    r6.xw = r0.yy ? r7.zw : r6.zw;
    r2.w = 1;
    r1.x = dot(r3.xyzw, r2.xyzw);
    r1.y = dot(r4.xyzw, r2.xyzw);
    r0.y = dot(r5.xyzw, r2.xyzw);
    r0.y = r0.y + -r6.x;
    r0.w = 0;
    r1.w = 0;
    while (true) {
      r3.x = cmp((int)r1.w >= 3);
      if (r3.x != 0) break;
      r3.x = (int)r1.w;
      r3.x = -1 + r3.x;
      r3.z = r0.w;
      r3.w = 0;
      while (true) {
        r4.x = cmp((int)r3.w >= 3);
        if (r4.x != 0) break;
        r4.x = (int)r3.w;
        r3.y = -1 + r4.x;
        r6.yz = r3.xy * cb0[1].zw + r1.xy;
        r3.y = t4.SampleCmpLevelZero(s4_s, r6.yzw, r0.y).x;
        r3.z = r3.z + r3.y;
        r3.w = (int)r3.w + 1;
      }
      r0.w = r3.z;
      r1.w = (int)r1.w + 1;
    }
    r0.y = 0.111111112 * r0.w;
    r0.w = cmp(r6.w < 1);
    r1.x = cmp(cb0[3].y < r1.z);
    r0.w = r0.w ? r1.x : 0;
    if (r0.w != 0) {
      r1.x = dot(cb2[19].xyzw, r2.xyzw);
      r1.y = dot(cb2[20].xyzw, r2.xyzw);
      r0.w = dot(cb2[21].xyzw, r2.xyzw);
      r0.w = -cb2[2].z + r0.w;
      r3.z = 1;
      r1.w = 0;
      r3.w = 0;
      while (true) {
        r4.x = cmp((int)r3.w >= 3);
        if (r4.x != 0) break;
        r4.x = (int)r3.w;
        r4.x = -1 + r4.x;
        r4.z = r1.w;
        r4.w = 0;
        while (true) {
          r5.x = cmp((int)r4.w >= 3);
          if (r5.x != 0) break;
          r5.x = (int)r4.w;
          r4.y = -1 + r5.x;
          r3.xy = r4.xy * cb0[1].zw + r1.xy;
          r3.x = t4.SampleCmpLevelZero(s4_s, r3.xyz, r0.w).x;
          r4.z = r4.z + r3.x;
          r4.w = (int)r4.w + 1;
        }
        r1.w = r4.z;
        r3.w = (int)r3.w + 1;
      }
      r0.w = -cb0[3].y + cb0[2].x;
      r1.x = -cb0[3].y + r1.z;
      r0.w = 1 / r0.w;
      r0.w = saturate(r1.x * r0.w);
      r1.x = r0.w * -2 + 3;
      r0.w = r0.w * r0.w;
      r0.w = r1.x * r0.w;
      r1.x = r1.w * 0.111111112 + -r0.y;
      r0.y = r0.w * r1.x + r0.y;
      r6.x = cb2[2].z;
    }
    if (r0.z != 0) {
      r0.z = (int)r0.z + -1;
      r0.w = (int)r0.z * 3;
      r1.xy = mad(int2(3,3), (int2)r0.zz, int2(1,2));
      r1.z = (uint)r0.z;
      r3.z = cb0[3].w + r1.z;
      r3.x = dot(cb2[r0.w+4].xyzw, r2.xyzw);
      r3.y = dot(cb2[r1.x+4].xyzw, r2.xyzw);
      r0.w = dot(cb2[r1.y+4].xyzw, r2.xyzw);
      r0.w = -r6.x * 3 + r0.w;
      r0.w = t6.SampleCmpLevelZero(s6_s, r3.xyz, r0.w).x;
      r0.z = dot(cb0[4].xyzw, icb[r0.z+0].xyzw);
      r0.w = -1 + r0.w;
      r0.z = r0.z * r0.w + 1;
      r0.y = min(r0.y, r0.z);
    }
    r0.y = -1 + r0.y;
    r0.xyzw = r0.xxxx * r0.yyyy + float4(1,1,1,1);
  } else {
    r0.xyzw = float4(1,1,1,1);
  }
  r1.x = -cb11[0].x + r0.w;
  r1.x = cmp(r1.x < 0);
  if (r1.x != 0) discard;
  o0.xyzw = r0.xyzw;
  return;
}