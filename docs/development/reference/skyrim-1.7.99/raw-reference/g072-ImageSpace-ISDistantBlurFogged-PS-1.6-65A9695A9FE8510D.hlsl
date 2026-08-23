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
  float4 cb2[6];
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
  float4 r0,r1,r2,r3,r4,r5;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xy = cb12[43].xy * v1.xy;
  r0.xy = max(float2(0,0), r0.xy);
  r1.x = cb12[44].z;
  r1.y = cb12[43].y;
  r0.xy = min(r1.xy, r0.xy);
  r2.xyz = t0.Sample(s0_s, r0.xy).xyz;
  r3.xyz = t1.Sample(s1_s, r0.xy).xyz;
  r0.z = cmp(0 < cb2[3].z);
  if (r0.z != 0) {
    r0.z = t3.Sample(s3_s, float2(0,0)).x;
    r0.w = saturate(r0.z * cb2[3].z + -cb2[3].w);
    r4.xz = cb2[1].xy;
    r4.yw = cb2[2].xw;
    r1.zw = r4.zw + -r4.xy;
    r1.zw = r0.ww * r1.wz + r4.yx;
  } else {
    r1.z = cb2[2].x;
    r1.w = cb2[1].x;
    r0.z = cb2[1].y;
  }
  r0.x = t2.Sample(s2_s, r0.xy).x;
  r0.w = cmp(0 != cb2[2].w);
  r2.w = cmp(0.999998987 < r0.x);
  r0.w = r0.w ? r2.w : 0;
  r4.xyzw = cb2[0].xyxy * float4(-3,-3,-3,0) + v1.xyxy;
  r4.xyzw = cb12[43].xyxy * r4.xyzw;
  r4.xyzw = max(float4(0,0,0,0), r4.xyzw);
  r4.xyzw = min(r4.xyzw, r1.xyxy);
  r2.w = t2.Sample(s2_s, r4.xy).x;
  r3.w = t2.Sample(s2_s, r4.zw).x;
  r4.xyzw = cb2[0].xyxy * float4(-3,3,3,-3) + v1.xyxy;
  r4.xyzw = cb12[43].xyxy * r4.xyzw;
  r4.xyzw = max(float4(0,0,0,0), r4.xyzw);
  r4.xyzw = min(r4.xyzw, r1.xyxy);
  r4.x = t2.Sample(s2_s, r4.xy).x;
  r4.y = t2.Sample(s2_s, r4.zw).x;
  r5.xyzw = cb2[0].xyxy * float4(3,0,3,3) + v1.xyxy;
  r5.xyzw = cb12[43].xyxy * r5.xyzw;
  r5.xyzw = max(float4(0,0,0,0), r5.xyzw);
  r5.xyzw = min(r5.xyzw, r1.xyxy);
  r4.z = t2.Sample(s2_s, r5.xy).x;
  r4.w = t2.Sample(s2_s, r5.zw).x;
  r5.xyzw = cb2[0].xyxy * float4(0,-3,0,3) + v1.xyxy;
  r5.xyzw = cb12[43].xyxy * r5.xyzw;
  r5.xyzw = max(float4(0,0,0,0), r5.xyzw);
  r5.xyzw = min(r5.xyzw, r1.xyxy);
  r1.x = t2.Sample(s2_s, r5.xy).x;
  r1.y = t2.Sample(s2_s, r5.zw).x;
  if (r0.w != 0) {
    r0.w = cmp(0.999998987 < r2.w);
    r0.w = r0.w ? 0.222222224 : 0.111111112;
    r2.w = r2.w + r0.x;
    r5.x = cmp(0.999998987 < r3.w);
    r5.x = r5.x ? 0.111111 : 0;
    r0.w = r5.x + r0.w;
    r2.w = r2.w + r3.w;
    r3.w = cmp(0.999998987 < r4.x);
    r3.w = r3.w ? 0.111111 : 0;
    r0.w = r3.w + r0.w;
    r2.w = r2.w + r4.x;
    r3.w = cmp(0.999998987 < r4.y);
    r3.w = r3.w ? 0.111111 : 0;
    r0.w = r3.w + r0.w;
    r2.w = r2.w + r4.y;
    r3.w = cmp(0.999998987 < r4.z);
    r3.w = r3.w ? 0.111111 : 0;
    r0.w = r3.w + r0.w;
    r2.w = r2.w + r4.z;
    r3.w = cmp(0.999998987 < r4.w);
    r3.w = r3.w ? 0.111111 : 0;
    r0.w = r3.w + r0.w;
    r2.w = r2.w + r4.w;
    r3.w = cmp(0.999998987 < r1.x);
    r3.w = r3.w ? 0.111111 : 0;
    r0.w = r3.w + r0.w;
    r1.x = r2.w + r1.x;
    r2.w = cmp(0.999998987 < r1.y);
    r2.w = r2.w ? 0.111111 : 0;
    r0.w = r2.w + r0.w;
    r1.x = r1.x + r1.y;
    r0.x = 0.111111112 * r1.x;
    r1.x = cmp(8.99999046 < r1.x);
  } else {
    r1.x = 0;
    r0.w = 0;
  }
  r1.x = ~(int)r1.x;
  r1.y = cmp(9.99999975e-006 < r0.x);
  r1.x = r1.y ? r1.x : 0;
  r1.y = cmp(0.00999999978 >= r0.x);
  r2.w = 100 * r0.x;
  r3.w = r0.x * 1.00999999 + -0.00999999978;
  r2.w = r1.y ? r2.w : r3.w;
  r4.xy = r1.yy ? cb2[3].xy : cb2[1].zw;
  r1.y = r2.w * 2 + -1;
  r2.w = dot(r4.xx, r4.yy);
  r3.w = r4.y + r4.x;
  r4.x = r4.y + -r4.x;
  r1.y = -r1.y * r4.x + r3.w;
  r4.y = r2.w / r1.y;
  r0.z = r4.y + -r0.z;
  r0.z = saturate(r0.z / r1.w);
  r0.w = -r0.w * 0.5 + 1;
  r0.w = r1.z * r0.w;
  r4.x = r0.z * r0.w;
  r0.y = 0;
  r0.xy = r1.xx ? r4.xy : r0.yx;
  r1.xyz = r3.xyz + -r2.xyz;
  r0.xzw = r0.xxx * r1.xyz + r2.xyz;
  r1.x = cb2[5].x + -cb2[5].y;
  r1.x = 1 / r1.x;
  r1.y = cb2[5].y * r1.x;
  r0.y = saturate(r0.y * r1.x + -r1.y);
  r0.y = cb2[4].w * r0.y;
  r1.xyz = cb2[4].xyz + -r0.xzw;
  o0.xyz = r0.yyy * r1.xyz + r0.xzw;
  o0.w = 1;
  return;
}