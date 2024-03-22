
#include "Common/GBuffer.hlsl"

Texture2D<half3> SpecularTexture 				: register(t0);
Texture2D<unorm half3> AlbedoTexture 			: register(t1);
Texture2D<unorm half3> ReflectanceTexture 		: register(t2);
Texture2D<unorm half3> NormalRoughnessTexture 	: register(t3);
Texture2D<unorm half4> ShadowMaskTexture 		: register(t4);
Texture2D<unorm half> DepthTexture 				: register(t5);

RWTexture2D<half4> MainRW 						: register(u0);
RWTexture2D<half4> NormalTAAMaskSpecularMaskRW 	: register(u1);

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

// #	define DEBUG

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

[numthreads(32, 32, 1)] void main(uint3 DTid : SV_DispatchThreadID) 
{
	half2 uv = half2(DTid.xy + 0.5) * RcpBufferDim;

	half3 diffuseColor = MainRW[DTid.xy];
	half3 specularColor = SpecularTexture[DTid.xy];

	half3 normalGlossiness = NormalRoughnessTexture[DTid.xy];
	half3 normalVS = DecodeNormal(normalGlossiness.xyz);
	half3 normalWS = normalize(mul(InvViewMatrix[0], half4(normalVS, 0)));

	half rawDepth = DepthTexture[DTid.xy];
	half depth = GetScreenDepth(rawDepth);

	uint eyeIndex = 0;

	half glossiness = normalGlossiness.z;

	half3 albedo = AlbedoTexture[DTid.xy];

	half3 color = diffuseColor + specularColor;

	half shadowMask = ShadowMaskTexture[DTid.xy].x;

	half3 viewPosition = DepthToView(uv, rawDepth, 0);
	viewPosition.z = depth;
	half3 worldPosition = mul(InvViewMatrix[0], half4(viewPosition, 1));
	
	half3 startPosVS = viewPosition;
	half3 endPosVS = viewPosition + DirLightDirectionVS[0].xyz * 10;

	half2 startPosUV = ViewToUV(startPosVS, true, eyeIndex);
	half2 endPosUV = ViewToUV(endPosVS, true, eyeIndex);

	half2 startPosPixel = clamp(startPosUV * BufferDim, 0, BufferDim);
	half2 endPosPixel = clamp(endPosUV * BufferDim, 0, BufferDim);

	half NdotL = dot(normalVS, DirLightDirectionVS[0].xyz);
	
	half shadow = 0;

	// Only march for: not shadowed, not self-shadowed, march distance greater than 1 pixel
	if (NdotL > 0.0 && shadowMask != 0.0 && startPosPixel.x != endPosPixel.x && startPosPixel.y != endPosPixel.y)
	{
		half step = 1.0 / 10.0;
		half pos = step + step * (InterleavedGradientNoise(DTid.xy) * 2.0 - 1.0);
		half slope = -NdotL;

		while (pos <= 1.0)
		{
			uint2 	tmpCoords 	= lerp(startPosPixel, endPosPixel, pos);
			half3	tmpNormal 	= DecodeNormal(NormalRoughnessTexture[tmpCoords]);
			half	tmpDepth 	= GetScreenDepth(DepthTexture[tmpCoords]);
			half	tmpNdotL 	= dot(tmpNormal, DirLightDirectionVS[0].xyz);

			half	shadowed = -tmpNdotL;
			shadowed += NdotL * pos;
			shadowed *= 1.0 - min(1, abs(depth - tmpDepth) * 0.1);

			slope += shadowed;

			shadow += max(0, slope);
			pos += step;
		}
	}
	
	shadow = saturate(1.0 - shadow);

	color += albedo * max(0, NdotL) * DirLightColor.xyz * shadow * shadowMask;

	half3 directionalAmbientColor = mul(DirectionalAmbient, half4(normalWS, 1.0));
	color += albedo * directionalAmbientColor;
	
#	if defined(DEBUG)
	half2 texCoord = half2(DTid.xy) / BufferDim.xy;

	if (texCoord.x < 0.5 && texCoord.y < 0.5)
	{
		color = color;
	} else if (texCoord.x < 0.5)
	{
		color = albedo;
	} else if (texCoord.y < 0.5)
	{
		color = normalWS;	
	} else {
		color = glossiness;	
	}
#	endif

	MainRW[DTid.xy] = half4(color, 1.0);
	NormalTAAMaskSpecularMaskRW[DTid.xy] = half4(EncodeNormalVanilla(normalVS), 0.0, glossiness);
}
