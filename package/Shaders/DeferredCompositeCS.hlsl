
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

float3 DecodeNormal(float2 enc)
{
	float4 r2;
	r2.xy = enc;
	r2.xy = r2.xy * float2(4,4) + float2(-2,-2);
	r2.z = dot(r2.xy, r2.xy);
	r2.zw = -r2.zz * float2(0.25,0.5) + float2(1,1);
	r2.z = sqrt(r2.z);
	r2.xy = r2.xy * r2.zz;
	r2.z = -r2.w;
	r2.w = dot(r2.xyz, r2.xyz);
	r2.w = rsqrt(r2.w);
	return r2.xyz * r2.www;
}

// #	define DEBUG

[numthreads(32, 32, 1)] void main(uint3 DTid : SV_DispatchThreadID) 
{
	half3 diffuseColor = MainRW[DTid.xy];
	half3 specularColor = SpecularTexture[DTid.xy];

	half3 normalGlossiness = NormalTAAMaskSpecularMaskRW[DTid.xy];
	half3 normalVS = DecodeNormal(normalGlossiness.xy);
	half3 normalWS = normalize(mul(InvViewMatrix[0], float4(normalVS, 0)));

	half glossiness = normalGlossiness.z;

	half3 albedo = AlbedoTexture[DTid.xy];

	float3 directionalAmbientColor = mul(DirectionalAmbient, float4(normalWS, 1.0));

	half3 color = diffuseColor + specularColor;
	color += albedo * directionalAmbientColor;

	half shadow = ShadowMaskTexture[DTid.xy].x;
	color += albedo * saturate(dot(normalWS, DirLightDirection.xyz)) * DirLightColor.xyz * shadow;

#	if defined(DEBUG)
	float2 texCoord = float2(DTid.xy) / BufferDim.xy;

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
	NormalTAAMaskSpecularMaskRW[DTid.xy] = half4(normalGlossiness.xy, 0.0, glossiness);
}
