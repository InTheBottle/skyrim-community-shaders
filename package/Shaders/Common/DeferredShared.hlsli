cbuffer PerFrameDeferredShared : register(b0)
{
	float4 BufferDim;
	float4 CameraData;
	row_major float3x4 DirectionalAmbient;
	uint FrameCount;
	uint pad0[3];
};