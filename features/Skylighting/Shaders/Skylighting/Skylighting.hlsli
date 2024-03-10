struct PerFrameSkylighting
{
	row_major float4x4 PrecipProj;
};

StructuredBuffer<PerFrameSkylighting> perFrameSkylighting : register(t69);
Texture2D<float> TexPrecipOcclusion : register(t70);
