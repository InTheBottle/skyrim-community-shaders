
#include "Common/GBuffer.hlsl"

Texture2D<unorm half2> NormalRoughnessTexture 	: register(t0);
Texture2D<unorm half> ShadowMaskTexture 		: register(t1);
Texture2D<unorm half> DepthTexture 				: register(t2);

RWTexture2D<unorm half> FilteredShadowRW 			: register(u0);

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

half GetScreenDepth(half depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

half2 WorldToUV(half3 x, bool is_position = true, uint a_eyeIndex = 0)
{
	half4 newPosition = half4(x, (half)is_position);
	half4 uv = mul(ViewProjMatrix[a_eyeIndex], newPosition);
	return (uv.xy / uv.w) * half2(0.5f, -0.5f) + 0.5f;
}

half InterleavedGradientNoise(half2 uv)
{
	// Temporal factor
	half frameStep = half(FrameCount % 16) * 0.0625f;
	uv.x += frameStep * 4.7526;
	uv.y += frameStep * 3.1914;

	half3 magic = half3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(uv, magic.xy)));
}

// Inverse project UV + raw depth into the view space.
half3 DepthToView(half2 uv, half z, uint a_eyeIndex)
{
	uv.y = 1 - uv.y;
	half4 cp = half4(uv * 2 - 1, z, 1);
	half4 vp = mul(InvProjMatrix[a_eyeIndex], cp);
	return vp.xyz / vp.w;
}

half2 ViewToUV(half3 position, bool is_position, uint a_eyeIndex)
{
	half4 uv = mul(ProjMatrix[a_eyeIndex], half4(position, (half)is_position));
	return (uv.xy / uv.w) * half2(0.5f, -0.5f) + 0.5f;
}

[numthreads(32, 32, 1)] void main(uint3 globalId : SV_DispatchThreadID, uint3 localId : SV_GroupThreadID, uint3 groupId : SV_GroupID) 
{
	half2 uv = half2(globalId.xy + 0.5) * RcpBufferDim;

	half3 normalVS = DecodeNormal(NormalRoughnessTexture[globalId.xy]);

	half rawDepth = DepthTexture[globalId.xy];
	half depth = GetScreenDepth(rawDepth);

	uint eyeIndex = 0;

	half shadowMask = ShadowMaskTexture[globalId.xy].x;

	half3 viewPosition = DepthToView(uv, rawDepth, 0);
	viewPosition.z = depth;
	
	half3 endPosVS = viewPosition + DirLightDirectionVS[0].xyz * 5;
	half2 endPosUV = ViewToUV(endPosVS, false, eyeIndex);

	half2 startPosPixel = clamp(uv * BufferDim, 0, BufferDim);
	half2 endPosPixel = clamp(endPosUV * BufferDim, 0, BufferDim);

	half NdotL = dot(normalVS, DirLightDirectionVS[0].xyz);
	
	half shadow = 0;

	half3 viewDirectionVS = normalize(viewPosition);
	
	// Only march for: not shadowed, not self-shadowed, march distance greater than 1 pixel
	bool validMarchPixel = NdotL > 0.0 && shadowMask != 0.0 && startPosPixel.x != endPosPixel.x && startPosPixel.y != endPosPixel.y;
	
	if (true)
	{
		half step = 1.0 / 5.0;
		half pos = step + step * (InterleavedGradientNoise(globalId.xy) * 2.0 - 1.0);
		half slope = -NdotL;

		for(int i = 0; i < 5; i++)
		{
			uint2 	tmpCoords 	= lerp(startPosPixel, endPosPixel, pos);
			half3	tmpNormal 	= DecodeNormal(NormalRoughnessTexture[tmpCoords]);
			half	tmpDepth 	= GetScreenDepth(DepthTexture[tmpCoords]);
			half	tmpNdotL 	= dot(tmpNormal, DirLightDirectionVS[0].xyz);

			half	shadowed = -tmpNdotL;
			shadowed += NdotL * pos;
			shadowed += max(0, dot(tmpNormal, viewDirectionVS));
			shadowed *= 1 - min(1, abs(depth - tmpDepth) * 0.1);

			slope += shadowed;

			shadow += max(0, slope);
			pos += step;
		}
	}

	shadow = saturate(1.0 - shadow);

	FilteredShadowRW[globalId.xy] = shadowMask * shadow;
}

