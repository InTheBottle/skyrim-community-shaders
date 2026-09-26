struct VS_OUTPUT
{
	float4 Position: SV_POSITION;
	float2 TexCoord: TEXCOORD0;
	nointerpolation float AlphaRef: TEXCOORD1;
};

struct STATIC_INPUT
{
	float3 Position: POSITION0;
	float2 TexCoord: TEXCOORD0;
	float4 ObjectToClip0: TEXCOORD1;
	float4 ObjectToClip1: TEXCOORD2;
	float4 ObjectToClip2: TEXCOORD3;
	float4 TexTransform: TEXCOORD4;
	float4 Params: TEXCOORD5;
};

VS_OUTPUT StaticVS(STATIC_INPUT input)
{
	float4 position = float4(input.Position, 1.0);

	VS_OUTPUT output;
	output.Position = float4(dot(input.ObjectToClip0, position), dot(input.ObjectToClip1, position), dot(input.ObjectToClip2, position), 1.0);
	output.TexCoord = input.TexCoord * input.TexTransform.zw + input.TexTransform.xy;
	output.AlphaRef = input.Params.x;
	return output;
}

cbuffer TreeDrawCB : register(b0)
{
	float4 TreeObjectToClip0;
	float4 TreeObjectToClip1;
	float4 TreeObjectToClip2;
	float4 TreeParams;
};

struct TREE_INPUT
{
	float3 Position: POSITION0;
	float2 TexCoord: TEXCOORD0;
	float4 InstanceData1: TEXCOORD4;
};

VS_OUTPUT TreeVS(TREE_INPUT input)
{
	float3 scaled = input.InstanceData1.www * input.Position;
	float2 rotation = TreeParams.xy;
	float3 rotated = float3(rotation.x * scaled.x - rotation.y * scaled.y, rotation.y * scaled.x + rotation.x * scaled.y, scaled.z);
	float4 position = float4(input.InstanceData1.xyz + rotated, 1.0);

	VS_OUTPUT output;
	output.Position = float4(dot(TreeObjectToClip0, position), dot(TreeObjectToClip1, position), dot(TreeObjectToClip2, position), 1.0);
	output.TexCoord = input.TexCoord;
	output.AlphaRef = TreeParams.z;
	return output;
}

Texture2D<float4> DiffuseTexture : register(t0);
SamplerState DiffuseSampler : register(s0);

void AlphaTestPS(VS_OUTPUT input)
{
	clip(DiffuseTexture.Sample(DiffuseSampler, input.TexCoord).a - input.AlphaRef);
}
