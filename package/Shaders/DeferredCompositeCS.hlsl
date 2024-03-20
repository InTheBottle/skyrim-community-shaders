
Texture2D<half3> SpecularTexture 				: register(t0);
Texture2D<unorm half3> AlbedoTexture 			: register(t1);
Texture2D<unorm half3> ReflectanceTexture 		: register(t2);

RWTexture2D<half4> MainRW 						: register(u0);
RWTexture2D<half4> NormalTAAMaskSpecularMaskRW : register(u1);

half3 decodeNormal(half2 enc)
{
    half2 fenc = enc*4-2;
    half f = dot(fenc,fenc);
    half g = sqrt(1-f/4);
    half3 n;
    n.xy = fenc*g;
    n.z = 1-f/2;
    return n;
}

#	define SCREEN_RES half2(2560.0, 1440.0)
// #	define DEBUG

[numthreads(32, 32, 1)] void main(uint3 DTid : SV_DispatchThreadID) 
{
	half3 diffuseColor = MainRW[DTid.xy];
	half3 specularColor = SpecularTexture[DTid.xy];

	half3 color = diffuseColor + specularColor;

	half3 normalGlossiness = NormalTAAMaskSpecularMaskRW[DTid.xy];
	half2 normal = normalGlossiness.xy;
	half glossiness = normalGlossiness.z;

#	if defined(DEBUG)
	half2 texCoord = half2(DTid.xy) / SCREEN_RES;

	if (texCoord.x < 0.5 && texCoord.y < 0.5)
	{
		color = MainRW[DTid.xy].rgb;
	} else if (texCoord.x < 0.5)
	{
		color = SpecularTexture[DTid.xy];
	} else if (texCoord.y < 0.5)
	{
		color = decodeNormal(NormalTAAMaskSpecularMaskRW[DTid.xy]);	
	} else {
		color = glossiness;	
	}
#	endif

	MainRW[DTid.xy] = half4(color, 1.0);
	NormalTAAMaskSpecularMaskRW[DTid.xy] = half4(normal, 0.0, glossiness);
}
