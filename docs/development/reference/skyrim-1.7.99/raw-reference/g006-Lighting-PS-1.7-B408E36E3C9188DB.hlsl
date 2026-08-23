Texture2D<float4> t1 : register(t1);

Texture2D<float4> t0 : register(t0);

SamplerState s1_s : register(s1);

SamplerState s0_s : register(s0);

cbuffer cb2 : register(b2)
{
  float4 cb2[30];
}

cbuffer cb1 : register(b1)
{
  float4 cb1[9];
}

cbuffer cb0 : register(b0)
{
  float4 cb0[2];
}

cbuffer cb12 : register(b12)
{
  float4 cb12[43];
}




// 3Dmigoto declarations
#define cmp -
Texture1D<float4> IniParams : register(t120);
Texture2D<float4> StereoParams : register(t125);


void main(
  float4 v0 : SV_POSITION0,
  float4 v1 : TEXCOORD0,
  float4 v2 : TEXCOORD4,
  float4 v3 : TEXCOORD1,
  float4 v4 : TEXCOORD2,
  float4 v5 : TEXCOORD3,
  float4 v6 : TEXCOORD8,
  float4 v7 : TEXCOORD9,
  float3 v8 : TEXCOORD10,
  float4 v9 : POSITION1,
  float4 v10 : POSITION2,
  float4 v11 : COLOR0,
  float4 v12 : COLOR1,
  out float4 o0 : SV_Target0,
  out float4 o1 : SV_Target1,
  out float4 o2 : SV_Target2)
{
  float4 r0,r1,r2,r3,r4,r5,r6;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xyzw = t0.Sample(s0_s, v1.xy).xyzw;
  r1.xyzw = t1.Sample(s1_s, v1.xy).xyzw;
  r1.xyz = r1.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r2.x = min(7, cb2[29].x);
  r3.x = dot(v3.xyz, r1.xyz);
  r3.y = dot(v4.xyz, r1.xyz);
  r3.z = dot(v5.xyz, r1.xyz);
  r2.y = dot(r3.xyz, r3.xyz);
  r2.y = rsqrt(r2.y);
  r3.xyz = r3.xyz * r2.yyy;
  r2.y = saturate(dot(r3.xyz, cb2[0].xyz));
  r2.yzw = cb2[1].xyz * r2.yyy;
  r4.x = cmp(0 < r2.x);
  if (r4.x != 0) {
    r4.xyz = r2.yzw;
    r4.w = 0;
    while (true) {
      r5.x = cmp(r4.w >= r2.x);
      if (r5.x != 0) break;
      r5.x = (int)r4.w;
      r5.yzw = cb2[r5.x+15].xyz + -v2.xyz;
      r6.x = dot(r5.yzw, r5.yzw);
      r6.y = sqrt(r6.x);
      r6.y = saturate(r6.y / cb2[r5.x+15].w);
      r6.y = -r6.y * r6.y + 1;
      r6.x = rsqrt(r6.x);
      r5.yzw = r6.xxx * r5.yzw;
      r5.y = saturate(dot(r3.xyz, r5.yzw));
      r5.xyz = cb2[r5.x+22].xyz * r5.yyy;
      r4.xyz = r5.xyz * r6.yyy + r4.xyz;
      r4.w = 1 + r4.w;
    }
    r2.yzw = r4.xyz;
  }
  r3.w = 1;
  r4.x = dot(cb2[11].xyzw, r3.xyzw);
  r4.y = dot(cb2[12].xyzw, r3.xyzw);
  r4.z = dot(cb2[13].xyzw, r3.xyzw);
  r3.xyz = cb2[4].yzw + r4.xyz;
  r2.xyz = r3.xyz + r2.yzw;
  r2.xyz = cb1[8].yzw * cb1[8].xxx + r2.xyz;
  r0.xyz = r2.xyz * r0.xyz;
  r2.xyz = v11.xyz * r0.xyz;
  r3.x = dot(cb12[12].xyzw, v9.xyzw);
  r3.y = dot(cb12[13].xyzw, v9.xyzw);
  r2.w = dot(cb12[15].xyzw, v9.xyzw);
  r3.xy = r3.xy / r2.ww;
  r4.x = dot(cb12[16].xyzw, v10.xyzw);
  r4.y = dot(cb12[17].xyzw, v10.xyzw);
  r2.w = dot(cb12[19].xyzw, v10.xyzw);
  r3.zw = r4.xy / r2.ww;
  r3.xy = r3.xy + -r3.zw;
  r3.xy = float2(-0.5,0.5) * r3.xy;
  r0.xyz = -r0.xyz * v11.xyz + v12.xyz;
  r0.xyz = v12.www * r0.xyz + r2.xyz;
  r0.xyz = -r0.xyz * cb0[0].www + r2.xyz;
  r4.xyz = cb12[42].yyy * r0.xyz;
  r0.xyz = r0.xyz * cb12[42].yyy + cb0[1].xxx;
  r0.xyz = min(r2.xyz, r0.xyz);
  r0.w = cb2[3].z * r0.w;
  r2.w = v11.w * r0.w;
  r2.xyz = -r4.xyz * cb12[42].zzz + r0.xyz;
  r0.x = dot(v6.xyz, r1.xyz);
  r0.y = dot(v7.xyz, r1.xyz);
  r0.z = dot(v8.xyz, r1.xyz);
  r0.w = dot(r0.xyz, r0.xyz);
  r0.w = rsqrt(r0.w);
  r0.xyz = r0.xyz * r0.www;
  r0.w = -9.99999975e-006 + cb2[7].x;
  r1.x = cb2[7].y + -r0.w;
  r0.w = r1.w + -r0.w;
  r1.x = 1 / r1.x;
  r0.w = saturate(r1.x * r0.w);
  r1.x = r0.w * -2 + 3;
  r0.w = r0.w * r0.w;
  r0.w = r1.x * r0.w;
  r1.w = cb2[7].w * r0.w;
  r0.z = r0.z * -8 + 8;
  r0.z = sqrt(r0.z);
  r0.z = max(0.00100000005, r0.z);
  r0.xy = r0.xy / r0.zz;
  r1.xy = float2(0.5,0.5) + r0.xy;
  r0.x = cmp(9.99999975e-006 < cb2[7].z);
  r1.z = 0;
  o2.xyzw = r0.xxxx ? r2.xyzw : r1.xyzw;
  o1.xy = r0.xx ? float2(1,0) : r3.xy;
  o0.xyzw = r2.xyzw;
  o1.zw = float2(0,1);
  return;
}