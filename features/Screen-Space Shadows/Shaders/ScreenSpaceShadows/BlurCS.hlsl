
// https://github.com/nvpro-samples/gl_ssao/blob/master/bilateralblur.frag.glsl
// https://aras-p.info/blog/2009/09/17/strided-blur-and-other-tips-for-ssao/

Texture2D<unorm half> InputTexture : register(t0);
Texture2D<unorm half> DepthTexture : register(t1);

RWTexture2D<unorm half> OutputTexture : register(u0);

SamplerState LinearSampler : register(s0);

cbuffer PerFrame : register(b0)
{
	float4 DirLightDirectionVS[2];
	float4 DirLightColor;
	float4 CameraData;
	float2 BufferDim;
	float2 RcpBufferDim;
	float4x4 ViewMatrix[2];
	float4x4 ProjMatrix[2];
	float4x4 ViewProjMatrix[2];
	float4x4 InvViewMatrix[2];
	float4x4 InvProjMatrix[2];
	row_major float3x4 DirectionalAmbient;
	uint FrameCount;
	uint pad0[3];
};

half GetDepth(half2 uv)
{
	return DepthTexture.SampleLevel(LinearSampler, uv, 0).r;
}

half GetScreenDepth2(half depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

half GetScreenDepth(half2 uv)
{
	return GetScreenDepth2(GetDepth(uv));
}

#define KERNEL_RADIUS 2
#define g_Sharpness 1.0

half BlurFunction(half2 uv, half r, half center_c, half center_d, inout half w_total)
{
  half c = InputTexture.SampleLevel(LinearSampler, uv, 0);
  half d = GetScreenDepth(uv);
  
  const half BlurSigma = half(KERNEL_RADIUS) * 0.5;
  const half BlurFalloff = 1.0 / (2.0*BlurSigma*BlurSigma);
  
  half ddiff = (d - center_d) * g_Sharpness;
  half w = exp2(-r*r*BlurFalloff - ddiff*ddiff);
  w_total += w;

  return c*w;
}

[numthreads(32, 32, 1)] void main(uint3 DTid : SV_DispatchThreadID) 
{
	half2 texCoord = (DTid.xy + 0.5) * RcpBufferDim.xy;

	half center_c = InputTexture[DTid.xy];
  	half center_d = GetScreenDepth2(DepthTexture[DTid.xy]);

	half c_total = center_c;
	half w_total = 1.0;
	
	for (half r = 1; r <= KERNEL_RADIUS; ++r)
	{
#	if defined(HORIZONTAL)
		half2 uv = texCoord + half2(RcpBufferDim.x, 0) * r * 2.0;
#	else
		half2 uv = texCoord + half2(0, RcpBufferDim.y) * r * 2.0;
#	endif
		c_total += BlurFunction(uv, r, center_c, center_d, w_total);  
	}
	
	for (half r = 1; r <= KERNEL_RADIUS; ++r)
	{
#	if defined(HORIZONTAL)
		half2 uv = texCoord - half2(RcpBufferDim.x, 0) * r * 2.0;
#	else
		half2 uv = texCoord - half2(0, RcpBufferDim.y) * r * 2.0;
#	endif
		c_total += BlurFunction(uv, r, center_c, center_d, w_total);  
	}

	OutputTexture[DTid.xy] = c_total / w_total;
}