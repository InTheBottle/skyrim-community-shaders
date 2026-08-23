Texture2D<uint4> t5 : register(t5);

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
  float4 r0,r1,r2,r3,r4,r5;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = v0.xy * cb0[0].xy + cb0[0].zw;
  r1.z = t2.Sample(s2_s, r0.xy).x;
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
  r1.xyz = r2.xyz / r0.xxx;
  r0.x = dot(r1.xyz, r1.xyz);
  r0.x = saturate(r0.x / cb2[3].z);
  r0.x = r0.x * r0.x;
  r0.x = r0.x * r0.x;
  r0.x = -r0.x * r0.x + 1;
  r1.w = 1;
  r2.x = dot(cb2[16].xyzw, r1.xyzw);
  r2.y = dot(cb2[17].xyzw, r1.xyzw);
  r2.z = dot(cb2[18].xyzw, r1.xyzw);
  r0.y = dot(cb2[19].xyzw, r1.xyzw);
  r2.xyz = r2.xyz / r0.yyy;
  r0.yw = float2(0.5,0.5) * r2.xy;
  r2.xy = r2.xy * float2(0.5,0.5) + float2(0.5,0.5);
  r2.z = -cb2[2].y + r2.z;
  r3.z = cb0[2].x;
  r2.w = 0;
  r3.w = 0;
  while (true) {
    r4.x = cmp((int)r3.w >= 3);
    if (r4.x != 0) break;
    r4.x = (int)r3.w;
    r4.x = -1 + r4.x;
    r4.z = r2.w;
    r4.w = 0;
    while (true) {
      r5.x = cmp((int)r4.w >= 3);
      if (r5.x != 0) break;
      r5.x = (int)r4.w;
      r4.y = -1 + r5.x;
      r3.xy = r4.xy * cb0[1].zw + r2.xy;
      r3.x = t4.SampleCmpLevelZero(s4_s, r3.xyz, r2.z).x;
      r4.z = r4.z + r3.x;
      r4.w = (int)r4.w + 1;
    }
    r2.w = r4.z;
    r3.w = (int)r3.w + 1;
  }
  r2.x = 0.111111112 * r2.w;
  r0.y = dot(r0.yw, r0.yw);
  r0.y = sqrt(r0.y);
  r0.y = r0.y + r0.y;
  r0.y = log2(r0.y);
  r0.y = cb2[3].x * r0.y;
  r0.y = exp2(r0.y);
  r0.y = r0.y * -r2.x + r2.x;
  if (r0.z != 0) {
    r0.z = (int)r0.z + -1;
    r0.w = (int)r0.z * 3;
    r2.xy = mad(int2(3,3), (int2)r0.zz, int2(1,2));
    r2.z = (uint)r0.z;
    r3.z = cb0[3].w + r2.z;
    r4.x = dot(cb2[r0.w+4].xyzw, r1.xyzw);
    r4.y = dot(cb2[r2.x+4].xyzw, r1.xyzw);
    r0.w = dot(cb2[r2.y+4].xyzw, r1.xyzw);
    r0.w = -cb2[2].y * 3 + r0.w;
    r1.xy = float2(0,0);
    while (true) {
      r1.z = cmp((int)r1.y >= 3);
      if (r1.z != 0) break;
      r1.z = (int)r1.y;
      r2.x = -1 + r1.z;
      r1.z = r1.x;
      r1.w = 0;
      while (true) {
        r2.z = cmp((int)r1.w >= 3);
        if (r2.z != 0) break;
        r2.z = (int)r1.w;
        r2.y = -1 + r2.z;
        r3.xy = r2.xy * cb0[1].zw + r4.xy;
        r2.y = t4.SampleCmpLevelZero(s4_s, r3.xyz, r0.w).x;
        r1.z = r2.y + r1.z;
        r1.w = (int)r1.w + 1;
      }
      r1.x = r1.z;
      r1.y = (int)r1.y + 1;
    }
    r0.z = dot(cb0[4].xyzw, icb[r0.z+0].xyzw);
    r0.w = r1.x * 0.111111112 + -1;
    r0.z = r0.z * r0.w + 1;
    r0.y = min(r0.y, r0.z);
  }
  r1.xyzw = r0.xxxx * r0.yyyy;
  r0.x = r0.x * r0.y + -cb11[0].x;
  r0.x = cmp(r0.x < 0);
  if (r0.x != 0) discard;
  o0.xyzw = r1.xyzw;
  return;
}