
struct VS_INPUT
{
	float4 PositionMS : POSITION0;

#if defined(TEXTURE)
	float2 TexCoord : TEXCOORD0;
#endif

#if defined(NORMALS)
	float4 Normal : NORMAL0;
	float4 Bitangent : BINORMAL0;
#endif
#if defined(VC)
	float4 Color : COLOR0;
#endif
#if defined(SKINNED)
	float4 BoneWeights : BLENDWEIGHT0;
	float4 BoneIndices : BLENDINDICES0;
#endif
};

struct VS_OUTPUT
{
	float4 PositionWS : POSITION;
};

cbuffer PerGeometry : register(b2)
{
	float4 ShadowFadeParam : packoffset(c0);
	row_major float4x4 World : packoffset(c1);
	float4 EyePos : packoffset(c5);
	float4 WaterParams : packoffset(c6);
	float4 TreeParams : packoffset(c7);
};

VS_OUTPUT main(VS_INPUT input)
{
	VS_OUTPUT vsout;
	vsout.PositionWS = mul(float4(input.PositionMS.xyz, 1.0), World);
	return vsout;
}