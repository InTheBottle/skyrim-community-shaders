Texture2D<float4> t3 : register(t3);

Texture2D<float4> t2 : register(t2);

Texture2D<float4> t1 : register(t1);

Texture2D<float4> t0 : register(t0);

SamplerState s3_s : register(s3);

SamplerState s2_s : register(s2);

SamplerState s1_s : register(s1);

SamplerState s0_s : register(s0);

cbuffer cb2 : register(b2)
{
  float4 cb2[4];
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
  float4 r0,r1,r2,r3,r4,r5,r6;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = cmp(0.5 < cb2[0].z);
  if (r0.x != 0) {
    r0.xy = cb12[43].xy * v1.xy;
    r0.xy = max(float2(0,0), r0.xy);
    r1.x = cb12[44].z;
    r1.y = cb12[43].y;
    r0.xy = min(r1.xy, r0.xy);
    r1.xyz = t0.Sample(s0_s, r0.xy).xyz;
    r0.xyz = t1.Sample(s1_s, r0.xy).xyz;
  } else {
    r1.xyz = t0.Sample(s0_s, v1.xy).xyz;
    r0.xyz = t1.Sample(s1_s, v1.xy).xyz;
  }
  r0.w = cmp(0 < cb2[3].z);
  if (r0.w != 0) {
    r0.w = t3.Sample(s3_s, float2(0,0)).x;
    r1.w = saturate(r0.w * cb2[3].z + -cb2[3].w);
    r2.xz = cb2[1].xy;
    r2.yw = cb2[2].xw;
    r2.zw = r2.zw + -r2.xy;
    r2.xy = r1.ww * r2.wz + r2.yx;
  } else {
    r2.x = cb2[2].x;
    r2.y = cb2[1].x;
    r0.w = cb2[1].y;
  }
  r2.zw = cb12[43].xy * v1.xy;
  r2.zw = max(float2(0,0), r2.zw);
  r3.x = cb12[44].z;
  r3.y = cb12[43].y;
  r2.zw = min(r3.xy, r2.zw);
  r1.w = t2.Sample(s2_s, r2.zw).x;
  r4.xyz = cmp(float3(0,0,0) != cb2[2].wyz);
  r2.z = cmp(0.999998987 < r1.w);
  r2.z = r2.z ? r4.x : 0;
  r5.xyzw = cb2[0].xyxy * float4(-3,-3,-3,0) + v1.xyxy;
  r5.xyzw = cb12[43].xyxy * r5.xyzw;
  r5.xyzw = max(float4(0,0,0,0), r5.xyzw);
  r5.xyzw = min(r5.xyzw, r3.xyxy);
  r2.w = t2.Sample(s2_s, r5.xy).x;
  r3.z = t2.Sample(s2_s, r5.zw).x;
  r5.xyzw = cb2[0].xyxy * float4(-3,3,3,-3) + v1.xyxy;
  r5.xyzw = cb12[43].xyxy * r5.xyzw;
  r5.xyzw = max(float4(0,0,0,0), r5.xyzw);
  r5.xyzw = min(r5.xyzw, r3.xyxy);
  r3.w = t2.Sample(s2_s, r5.xy).x;
  r4.x = t2.Sample(s2_s, r5.zw).x;
  r5.xyzw = cb2[0].xyxy * float4(3,0,3,3) + v1.xyxy;
  r5.xyzw = cb12[43].xyxy * r5.xyzw;
  r5.xyzw = max(float4(0,0,0,0), r5.xyzw);
  r5.xyzw = min(r5.xyzw, r3.xyxy);
  r4.w = t2.Sample(s2_s, r5.xy).x;
  r5.x = t2.Sample(s2_s, r5.zw).x;
  r6.xyzw = cb2[0].xyxy * float4(0,-3,0,3) + v1.xyxy;
  r6.xyzw = cb12[43].xyxy * r6.xyzw;
  r6.xyzw = max(float4(0,0,0,0), r6.xyzw);
  r6.xyzw = min(r6.xyzw, r3.xyxy);
  r3.x = t2.Sample(s2_s, r6.xy).x;
  r3.y = t2.Sample(s2_s, r6.zw).x;
  if (r2.z != 0) {
    r2.z = cmp(0.999998987 < r2.w);
    r2.z = r2.z ? 0.222222224 : 0.111111112;
    r2.w = r2.w + r1.w;
    r5.y = cmp(0.999998987 < r3.z);
    r5.y = r5.y ? 0.111111 : 0;
    r2.z = r5.y + r2.z;
    r2.w = r2.w + r3.z;
    r3.z = cmp(0.999998987 < r3.w);
    r3.z = r3.z ? 0.111111 : 0;
    r2.z = r3.z + r2.z;
    r2.w = r2.w + r3.w;
    r3.z = cmp(0.999998987 < r4.x);
    r3.z = r3.z ? 0.111111 : 0;
    r2.z = r3.z + r2.z;
    r2.w = r2.w + r4.x;
    r3.z = cmp(0.999998987 < r4.w);
    r3.z = r3.z ? 0.111111 : 0;
    r2.z = r3.z + r2.z;
    r2.w = r2.w + r4.w;
    r3.z = cmp(0.999998987 < r5.x);
    r3.z = r3.z ? 0.111111 : 0;
    r2.z = r3.z + r2.z;
    r2.w = r2.w + r5.x;
    r3.z = cmp(0.999998987 < r3.x);
    r3.z = r3.z ? 0.111111 : 0;
    r2.z = r3.z + r2.z;
    r2.w = r2.w + r3.x;
    r3.x = cmp(0.999998987 < r3.y);
    r3.x = r3.x ? 0.111111 : 0;
    r2.z = r3.x + r2.z;
    r2.w = r2.w + r3.y;
    r1.w = 0.111111112 * r2.w;
    r2.w = cmp(8.99999046 < r2.w);
  } else {
    r2.zw = float2(0,0);
  }
  r2.w = ~(int)r2.w;
  r3.x = cmp(9.99999975e-006 < r1.w);
  r2.w = r3.x ? r2.w : 0;
  r3.x = cmp(0.00999999978 >= r1.w);
  r3.y = 100 * r1.w;
  r1.w = r1.w * 1.00999999 + -0.00999999978;
  r1.w = r3.x ? r3.y : r1.w;
  r3.xy = r3.xx ? cb2[3].xy : cb2[1].zw;
  r1.w = r1.w * 2 + -1;
  r3.z = dot(r3.xx, r3.yy);
  r3.w = r3.y + r3.x;
  r3.x = r3.y + -r3.x;
  r1.w = -r1.w * r3.x + r3.w;
  r1.w = r3.z / r1.w;
  r3.x = cmp(r1.w < r0.w);
  r3.x = r4.y ? r3.x : 0;
  r3.y = -r1.w + r0.w;
  r3.y = r3.y / r2.y;
  r3.x = r3.x ? r3.y : 0;
  r3.y = cmp(r0.w < r1.w);
  r3.y = r4.z ? r3.y : 0;
  r0.w = r1.w + -r0.w;
  r0.w = r0.w / r2.y;
  r0.w = saturate(r3.y ? r0.w : r3.x);
  r1.w = -r2.z * 0.5 + 1;
  r1.w = r2.x * r1.w;
  r0.w = r1.w * r0.w;
  r0.w = (int)r0.w & (int)r2.w;
  r0.xyz = r0.xyz + -r1.xyz;
  o0.xyz = r0.www * r0.xyz + r1.xyz;
  o0.w = 1;
  return;
}