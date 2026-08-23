cbuffer cb2 : register(b2)
{
  float4 cb2[6];
}

cbuffer cb0 : register(b0)
{
  float4 cb0[2];
}

cbuffer cb12 : register(b12)
{
  float4 cb12[12];
}




// 3Dmigoto declarations
#define cmp -
Texture1D<float4> IniParams : register(t120);
Texture2D<float4> StereoParams : register(t125);


void main(
  float4 v0 : POSITION0,
  float4 v1 : NORMAL0,
  out float4 o0 : SV_POSITION0,
  out float4 o1 : TEXCOORD0,
  out float o2 : TEXCOORD5,
  out float3 p2 : TEXCOORD7,
  out float4 o3 : POSITION1,
  out float4 o4 : POSITION2)
{
  float4 r0,r1,r2,r3;
  uint4 bitmask, uiDest;
  float4 fDest;

  r0.xyzw = cb2[0].xyzw * cb12[8].xxxx;
  r1.xyzw = cb2[1].xyzw * cb12[8].yyyy;
  r0.xyzw = r1.xyzw + r0.xyzw;
  r1.xyzw = cb2[2].xyzw * cb12[8].zzzz;
  r0.xyzw = r1.xyzw + r0.xyzw;
  r1.xyzw = float4(0,0,0,1) * cb12[8].wwww;
  r0.xyzw = r1.xyzw + r0.xyzw;
  r1.xyz = v0.xyz;
  r1.w = 1;
  o0.x = dot(r0.xyzw, r1.xyzw);
  r0.xyzw = cb2[0].xyzw * cb12[9].xxxx;
  r2.xyzw = cb2[1].xyzw * cb12[9].yyyy;
  r0.xyzw = r2.xyzw + r0.xyzw;
  r2.xyzw = cb2[2].xyzw * cb12[9].zzzz;
  r0.xyzw = r2.xyzw + r0.xyzw;
  r2.xyzw = float4(0,0,0,1) * cb12[9].wwww;
  r0.xyzw = r2.xyzw + r0.xyzw;
  o0.y = dot(r0.xyzw, r1.xyzw);
  r0.xyzw = cb2[0].xyzw * cb12[10].xxxx;
  r2.xyzw = cb2[1].xyzw * cb12[10].yyyy;
  r0.xyzw = r2.xyzw + r0.xyzw;
  r2.xyzw = cb2[2].xyzw * cb12[10].zzzz;
  r0.xyzw = r2.xyzw + r0.xyzw;
  r2.xyzw = float4(0,0,0,1) * cb12[10].wwww;
  r0.xyzw = r2.xyzw + r0.xyzw;
  o0.z = dot(r0.xyzw, r1.xyzw);
  r0.xyzw = cb2[0].xyzw * cb12[11].xxxx;
  r2.xyzw = cb2[1].xyzw * cb12[11].yyyy;
  r0.xyzw = r2.xyzw + r0.xyzw;
  r2.xyzw = cb2[2].xyzw * cb12[11].zzzz;
  r0.xyzw = r2.xyzw + r0.xyzw;
  r2.xyzw = float4(0,0,0,1) * cb12[11].wwww;
  r0.xyzw = r2.xyzw + r0.xyzw;
  o0.w = dot(r0.xyzw, r1.xyzw);
  o1.xyzw = float4(0,0,1,0);
  r0.xyz = cb2[1].xyz * cb12[3].yyy;
  r0.xyz = cb12[3].xxx * cb2[0].xyz + r0.xyz;
  r0.xyz = cb12[3].zzz * cb2[2].xyz + r0.xyz;
  r2.xyz = v1.xyz * float3(2,2,2) + float3(-1,-1,-1);
  r0.w = dot(r0.xyz, r2.xyz);
  r3.xyz = cb2[1].xyz * cb12[0].yyy;
  r3.xyz = cb12[0].xxx * cb2[0].xyz + r3.xyz;
  r3.xyz = cb12[0].zzz * cb2[2].xyz + r3.xyz;
  r0.x = dot(r3.xyz, r2.xyz);
  r3.xyz = cb2[1].xyz * cb12[1].yyy;
  r3.xyz = cb12[1].xxx * cb2[0].xyz + r3.xyz;
  r3.xyz = cb12[1].zzz * cb2[2].xyz + r3.xyz;
  r0.y = dot(r3.xyz, r2.xyz);
  r3.xyz = cb2[1].xyz * cb12[2].yyy;
  r3.xyz = cb12[2].xxx * cb2[0].xyz + r3.xyz;
  r3.xyz = cb12[2].zzz * cb2[2].xyz + r3.xyz;
  r0.z = dot(r3.xyz, r2.xyz);
  r0.w = dot(r0.xyzw, r0.xyzw);
  r0.w = rsqrt(r0.w);
  p2.xyz = r0.xyz * r0.www;
  o2.x = cb0[1].w;
  o3.w = 1;
  o3.x = dot(cb2[0].xyzw, r1.xyzw);
  o3.y = dot(cb2[1].xyzw, r1.xyzw);
  o3.z = dot(cb2[2].xyzw, r1.xyzw);
  o4.w = 1;
  o4.x = dot(cb2[3].xyzw, r1.xyzw);
  o4.y = dot(cb2[4].xyzw, r1.xyzw);
  o4.z = dot(cb2[5].xyzw, r1.xyzw);
  return;
}