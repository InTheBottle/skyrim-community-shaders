Texture2D<float4> t4 : register(t4);

Texture2D<float4> t2 : register(t2);

Texture2D<float4> t1 : register(t1);

Texture2D<float4> t0 : register(t0);

SamplerState s4_s : register(s4);

SamplerState s2_s : register(s2);

SamplerState s1_s : register(s1);

SamplerState s0_s : register(s0);

cbuffer cb2 : register(b2)
{
  float4 cb2[8];
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
  float4 r0,r1,r2,r3,r4,r5,r6,r7;
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
  r2.xy = cb12[43].xy * v1.xy;
  r2.xy = max(float2(0,0), r2.xy);
  r3.x = cb12[44].z;
  r3.y = cb12[43].y;
  r2.xy = min(r3.xy, r2.xy);
  r0.w = t4.Sample(s4_s, r2.xy).x;
  r4.xyzw = cb2[6].xyzw + -cb2[1].xyzw;
  r4.xyzw = r0.wwww * r4.xyzw + cb2[1].xyzw;
  r5.xyzw = cb2[7].xyzw + -cb2[2].xyzw;
  r5.xyzw = r0.wwww * r5.xyzw + cb2[2].xyzw;
  r2.x = t2.Sample(s2_s, r2.xy).x;
  r5.yzw = cmp(float3(0,0,0) != r5.wyz);
  r1.w = cmp(0.999998987 < r2.x);
  r1.w = r1.w ? r5.y : 0;
  r6.xyzw = cb2[0].xyxy * float4(-3,-3,-3,0) + v1.xyxy;
  r6.xyzw = cb12[43].xyxy * r6.xyzw;
  r6.xyzw = max(float4(0,0,0,0), r6.xyzw);
  r6.xyzw = min(r6.xyzw, r3.xyxy);
  r2.z = t2.Sample(s2_s, r6.xy).x;
  r2.w = t2.Sample(s2_s, r6.zw).x;
  r6.xyzw = cb2[0].xyxy * float4(-3,3,3,-3) + v1.xyxy;
  r6.xyzw = cb12[43].xyxy * r6.xyzw;
  r6.xyzw = max(float4(0,0,0,0), r6.xyzw);
  r6.xyzw = min(r6.xyzw, r3.xyxy);
  r3.z = t2.Sample(s2_s, r6.xy).x;
  r3.w = t2.Sample(s2_s, r6.zw).x;
  r6.xyzw = cb2[0].xyxy * float4(3,0,3,3) + v1.xyxy;
  r6.xyzw = cb12[43].xyxy * r6.xyzw;
  r6.xyzw = max(float4(0,0,0,0), r6.xyzw);
  r6.xyzw = min(r6.xyzw, r3.xyxy);
  r5.y = t2.Sample(s2_s, r6.xy).x;
  r6.x = t2.Sample(s2_s, r6.zw).x;
  r7.xyzw = cb2[0].xyxy * float4(0,-3,0,3) + v1.xyxy;
  r7.xyzw = cb12[43].xyxy * r7.xyzw;
  r7.xyzw = max(float4(0,0,0,0), r7.xyzw);
  r7.xyzw = min(r7.xyzw, r3.xyxy);
  r3.x = t2.Sample(s2_s, r7.xy).x;
  r3.y = t2.Sample(s2_s, r7.zw).x;
  if (r1.w != 0) {
    r1.w = cmp(0.999998987 < r2.z);
    r1.w = r1.w ? 0.222222224 : 0.111111112;
    r2.z = r2.x + r2.z;
    r6.y = cmp(0.999998987 < r2.w);
    r6.y = r6.y ? 0.111111 : 0;
    r1.w = r6.y + r1.w;
    r2.z = r2.z + r2.w;
    r2.w = cmp(0.999998987 < r3.z);
    r2.w = r2.w ? 0.111111 : 0;
    r1.w = r2.w + r1.w;
    r2.z = r2.z + r3.z;
    r2.w = cmp(0.999998987 < r3.w);
    r2.w = r2.w ? 0.111111 : 0;
    r1.w = r2.w + r1.w;
    r2.z = r2.z + r3.w;
    r2.w = cmp(0.999998987 < r5.y);
    r2.w = r2.w ? 0.111111 : 0;
    r1.w = r2.w + r1.w;
    r2.z = r2.z + r5.y;
    r2.w = cmp(0.999998987 < r6.x);
    r2.w = r2.w ? 0.111111 : 0;
    r1.w = r2.w + r1.w;
    r2.z = r2.z + r6.x;
    r2.w = cmp(0.999998987 < r3.x);
    r2.w = r2.w ? 0.111111 : 0;
    r1.w = r2.w + r1.w;
    r2.z = r2.z + r3.x;
    r2.w = cmp(0.999998987 < r3.y);
    r2.w = r2.w ? 0.111111 : 0;
    r1.w = r2.w + r1.w;
    r2.z = r2.z + r3.y;
    r2.x = 0.111111112 * r2.z;
    r2.z = cmp(8.99999046 < r2.z);
  } else {
    r2.z = 0;
    r1.w = 0;
  }
  r2.z = ~(int)r2.z;
  r2.w = cmp(9.99999975e-006 < r2.x);
  r2.z = r2.w ? r2.z : 0;
  r2.w = cmp(0.00999999978 >= r2.x);
  r3.x = 100 * r2.x;
  r3.y = r2.x * 1.00999999 + -0.00999999978;
  r3.x = r2.w ? r3.x : r3.y;
  r3.yz = r2.ww ? cb2[3].xy : r4.zw;
  r2.w = r3.x * 2 + -1;
  r3.x = dot(r3.yy, r3.zz);
  r3.w = r3.z + r3.y;
  r3.y = r3.z + -r3.y;
  r2.w = -r2.w * r3.y + r3.w;
  r3.y = r3.x / r2.w;
  r2.w = cmp(r3.y < r4.y);
  r2.w = r5.z ? r2.w : 0;
  r3.z = r4.y + -r3.y;
  r3.z = r3.z / r4.x;
  r2.w = r2.w ? r3.z : 0;
  r3.z = cmp(r4.y < r3.y);
  r3.z = r5.w ? r3.z : 0;
  r3.w = cmp(r0.w == 0.000000);
  r3.z = (int)r3.w | (int)r3.z;
  r3.w = r3.y + -r4.y;
  r3.w = r3.w / r4.x;
  r2.w = saturate(r3.z ? r3.w : r2.w);
  r1.w = -r1.w * 0.5 + 1;
  r1.w = r5.x * r1.w;
  r3.x = r2.w * r1.w;
  r2.y = 0;
  r2.xy = r2.zz ? r3.xy : r2.yx;
  r0.xyz = r0.xyz + -r1.xyz;
  r0.xyz = r2.xxx * r0.xyz + r1.xyz;
  r1.x = cb2[5].x + -cb2[5].y;
  r1.x = 1 / r1.x;
  r1.y = cb2[5].y * r1.x;
  r1.x = saturate(r2.y * r1.x + -r1.y);
  r1.x = cb2[4].w * r1.x;
  r0.w = r1.x * r0.w;
  r1.xyz = cb2[4].xyz + -r0.xyz;
  o0.xyz = r0.www * r1.xyz + r0.xyz;
  o0.w = 1;
  return;
}