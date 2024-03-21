
#include "Common/GBuffer.hlsl"

Texture2D<half3> SpecularTexture 				: register(t0);
Texture2D<unorm half3> AlbedoTexture 			: register(t1);
Texture2D<unorm half3> ReflectanceTexture 		: register(t2);
Texture2D<unorm half4> ShadowMaskTexture 		: register(t3);

RWTexture2D<half4> MainRW 						: register(u0);
RWTexture2D<half4> NormalTAAMaskSpecularMaskRW 	: register(u1);

cbuffer PerFrame : register(b0)
{
	float4 DirLightDirection;
	float4 DirLightColor;
	float4 CameraData;
	float2 BufferDim;
	float2 RcpBufferDim;
	float4x4 InvViewMatrix[2];
	row_major float3x4 DirectionalAmbient;
	uint FrameCount;
	uint pad0[3];
};

 #	define DEBUG

[numthreads(32, 32, 1)] void main(uint3 DTid : SV_DispatchThreadID) 
{
	half3 diffuseColor = MainRW[DTid.xy];
	half3 specularColor = SpecularTexture[DTid.xy];

	half3 normalGlossiness = NormalTAAMaskSpecularMaskRW[DTid.xy];
	half3 normalVS = normalize(DecodeNormal(normalGlossiness.xyz));
	half3 normalWS = normalize(mul(InvViewMatrix[0], half4(normalVS, 0)));

	half glossiness = normalGlossiness.z;

	half3 albedo = AlbedoTexture[DTid.xy];

	half3 directionalAmbientColor = mul(DirectionalAmbient, half4(normalWS, 1.0));

	half3 color = diffuseColor + specularColor;
	color += albedo * directionalAmbientColor;

	half shadow = ShadowMaskTexture[DTid.xy].x;
	color += albedo * saturate(dot(normalWS, DirLightDirection.xyz)) * DirLightColor.xyz * shadow;

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
