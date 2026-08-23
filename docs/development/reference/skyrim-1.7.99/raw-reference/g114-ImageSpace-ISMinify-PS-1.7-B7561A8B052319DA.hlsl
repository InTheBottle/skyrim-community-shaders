Texture2D<float4> t0 : register(t0);

SamplerState s0_s : register(s0);

cbuffer cb2 : register(b2)
{
  float4 cb2[3];
}

cbuffer cb12 : register(b12)
{
  float4 cb12[44];
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
  float4 r0,r1,r2;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.x = cmp(0 < asuint(cb2[2].x));
  r0.yz = cb12[43].xy * v1.xy;
  r1.xy = cb2[0].yx * r0.zy;
  r1.xy = (int2)r1.xy;
  r1.xy = (int2)r1.xy & int2(1,1);
  r1.xy = (int2)r1.xy ^ int2(1,1);
  r1.xy = (int2)r1.xy;
  r1.xy = r1.xy * float2(2,2) + float2(-1,-1);
  r0.yz = r1.xy * cb2[0].zw + r0.yz;
  r1.x = -cb12[41].w;
  r1.y = -0;
  r1.xy = cb12[43].xy + r1.xy;
  r0.yz = max(float2(0,0), r0.yz);
  r0.yz = min(r0.yz, r1.xy);
  r1.xy = cb2[0].yx * v1.yx;
  r1.xy = (int2)r1.xy;
  r1.xy = (int2)r1.xy & int2(1,1);
  r1.xy = (int2)r1.xy ^ int2(1,1);
  r1.xy = (int2)r1.xy;
  r1.xy = r1.xy * float2(2,2) + float2(-1,-1);
  r1.xy = r1.xy * cb2[0].zw + v1.xy;
  r0.xy = r0.xx ? r0.yz : r1.xy;
  r0.z = max(cb2[0].x, cb2[0].y);
  r0.z = cmp(r0.z == 3.000000);
  if (r0.z != 0) {
    r0.zw = (int2)cb2[0].xy;
    r0.zw = (int2)r0.zw & int2(1,1);
    r0.zw = (int2)r0.zw;
    r0.zw = r0.zw * float2(0.166666672,0.166666672) + float2(0.5,0.5);
    r1.xy = r0.zw / cb2[0].xy;
    r0.zw = r1.xy + r0.xy;
    r0.z = t0.Sample(s0_s, r0.zw).x;
    r1.zw = -r1.xy;
    r2.xyzw = r1.zyxw + r0.xyxy;
    r0.w = t0.Sample(s0_s, r2.xy).x;
    r0.z = r0.z + r0.w;
    r1.xy = -r1.xy + r0.xy;
    r0.w = t0.Sample(s0_s, r1.xy).x;
    r0.z = r0.z + r0.w;
    r0.w = t0.Sample(s0_s, r2.zw).x;
    r0.z = r0.z + r0.w;
    o0.x = 0.25 * r0.z;
    o0.yzw = float3(0,0,0);
  } else {
    r0.xyzw = t0.Sample(s0_s, r0.xy).xyzw;
    o0.xyzw = r0.xyzw;
  }
  return;
}