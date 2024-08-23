#ifdef VSHADER
void main(
  float4 v0 : POSITION0,
  float2 v1 : TEXCOORD0,
  out float4 o0 : SV_POSITION0,
  out float2 o1 : TEXCOORD0)
{
  o0.xyz = v0.xyz;
  o0.w = 1;
  o1.xy = v1.xy;
  return;
}
#endif

#if defined(PSHADER)
#include "Common/FrameBuffer.hlsli"

Texture2D<float4> NormalTexture : register(t0);
Texture2D<float4> ColorTexture : register(t1);
Texture2D<float4> DepthTexture : register(t2);
Texture2D<float4> AlphaTexture : register(t3);

SamplerState NormalSampler : register(s0);
SamplerState ColorSampler : register(s1);
SamplerState DepthSampler : register(s2);
SamplerState AlphaSampler : register(s3);

cbuffer Params : register(b2)
{
  float ReflectionRayThickness;
  float ReflectionMarchingRadius;
  float AlphaWeight;
  float ReflectionMarchingRadiusRcp;
  float4 DefaultNormal;
}

float3 GetDecodedNormal(float2 enc)
{
    float2 fenc = enc*4-2;
    float f = dot(fenc,fenc);
    float g = sqrt(1-f/4);
    float3 n;
    n.xy = fenc*g;
    n.z = 1-f/2;
    return n;
}

#define cmp -

void main(
  float4 v0 : SV_POSITION0,
  float2 v1 : TEXCOORD0,
  out float4 o0 : SV_Target0)
{
	float4 r0,r1,r2,r3,r4,r5,r6,uvAndDepth,r8;

	float2 uv = v1.xy;

	r0.xy = GetDynamicResolutionAdjustedScreenPosition(uv);

	r2.xyzw = NormalTexture.SampleLevel(NormalSampler, r0.xy, 0).xyzw;

	float startDepth = DepthTexture.SampleLevel(DepthSampler, r0.xy, 0).x;
	r3.z = startDepth;

	// Check if the surface is marked for SSR
	if (r2.z < 9.99999975e-06) {
		o0.xyzw = float4(0,0,0,0);
		return;
	}

	r2.xyz = GetDecodedNormal(r2.xy);

	r2.w = dot(r2.xyz, r2.xyz);
	r2.w = rsqrt(r2.w);
	r4.xyz = r2.xyz * r2.www;
	r2.xyz = -r2.xyz * r2.www + DefaultNormal.xyz;
	r2.xyz = r2.xyz + r4.xyz;

	r3.xy = v1.yx;

	// project depth to world

	r4.xyz = float3(uv, r3.z) * float3(2.0, -2.0, 1.0) + float3(-1.0, 1.0, 0.0);
	r5.xyzw = mul(CameraProjInverse[0], float4(r4.xyz, 1.0));
	r5.xyzw = r5.xyzw / r5.wwww;

	r0.w = dot(-r5.xyz, -r5.xyz);
	r0.w = rsqrt(r0.w);
	r6.xyz = -r5.xyz * r0.www;
	r6.w = 0;
	r0.w = dot(-r6.xyz, r2.xyz);
	r0.w = r0.w + r0.w;
	r2.xyz = r2.xyz * -r0.www;
	r2.w = 0;
	r2.xyzw = r2.xyzw + -r6.xyzw;
	r5.xyzw = r5.xyzw + r2.xyzw;

	// view to uv

	uvAndDepth.xyzw = mul(CameraProj[0], float4(r5.xyz, 1.0));
	r4.xyw = uvAndDepth.xyz / uvAndDepth.www;
	r4.xyw = r4.xyw * float3(0.5, -0.5, 1.0) + float3(0.5, 0.5, 0.0);

	r4.xyw = r4.xyw + -r3.yxz;
	r0.w = dot(r4.xy, r4.xy);
	r0.w = sqrt(r0.w);
	r0.w = rcp(r0.w);
	r0.w = ReflectionRayThickness * r0.w;
	r3.xyw = r4.xyw * r0.www + r3.yxz;

	r3.xy = GetDynamicResolutionAdjustedScreenPosition(r3.xy);

	r0.z = r4.z;
	r3.xyw = r3.xyw + -r0.xyz;
	r5.xyz = r0.xyz;
	uvAndDepth.xy = float3(r0.xy, r3.z);

	uint sampleCount = 16;

	bool found = false;

	for(uint i = 1; i < sampleCount + 1; i++)
	{
		r2.w = rcp(sampleCount) * i;
		r8.xyz = r2.www * r3.xyw + r0.xyz;
		r2.w = DepthTexture.SampleLevel(DepthSampler, r8.xy, 0).x;
		r2.w = cmp(r2.w < r8.z);
		if (r2.w != 0) {
			r5.xyz = uvAndDepth.xyz;
			uvAndDepth.xyz = r8.xyz;
			found = true;
			break;
		}
		r5.xyz = uvAndDepth.xyz;
		uvAndDepth.xyz = r8.xyz;
	}

	if (!found)
	{
		o0.xyzw = float4(0,0,0,0);
		return;
	}

	// TODO: binary search

	r0.xy = GetDynamicResolutionAdjustedScreenPosition(uvAndDepth.xy); 
	r0.xyz = AlphaTexture.Sample(AlphaSampler, r0.xy).xyz;

	r3.zw = max(float2(0,0), uvAndDepth.xy);
	r1.xy = min(r3.zw, float2(DynamicResolutionParams2.z, DynamicResolutionParams1.y));
	r3.xyz = ColorTexture.Sample(ColorSampler, r1.xy).xyz;

	o0.xyz = AlphaWeight * r0.xyz + r3.xyz;

	float2 outputUV = uvAndDepth.xy;
	float2 outputUVnoDR = outputUV * DynamicResolutionParams2.xy;

	float2 uvDiffFromStart = outputUVnoDR - uv;

	float fade1 = dot(uvDiffFromStart, uvDiffFromStart);
	fade1 = sqrt(fade1);
	fade1 = -fade1 * ReflectionMarchingRadiusRcp + 1;

	float2 uvDiffFromCentre = outputUVnoDR - float2(0.5, 0.5);

	float fade2 = dot(uvDiffFromCentre, uvDiffFromCentre);
	fade2 = sqrt(fade2);
	fade2 = fade2 + fade2;
	fade2 = min(1, fade2);
	fade2 = -fade2 * fade2 + 1;

	o0.w = fade1 * fade2;

  	return;
}
#endif
