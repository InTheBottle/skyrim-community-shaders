#include "VoxelUtil.hlsli"

struct GS_INPUT
{
    float4 PositionWS   : POSITION;
};

struct GS_OUTPUT
{
    float4 Position     : SV_POSITION;
    float4 PositionWS   : POSITION;
};

cbuffer Voxel : register(b0)
{
	VoxelRadiance voxel_radiance;
};

[maxvertexcount(3)]
void main(
	triangle GS_INPUT input[3] : SV_POSITION,
	inout TriangleStream<GS_OUTPUT> outputStream
)
{
    float3 facenormal = float3(0, 0, 1);

    uint maxi = facenormal[1] > facenormal[0] ? 1 : 0;
    maxi = facenormal[2] > facenormal[maxi] ? 2 : maxi;

    for (uint i = 0; i < 3; ++i)
    {
        GS_OUTPUT output;

		// World space -> Voxel grid space:
        //output.Position.xyz = (input[i].PositionWS.xyz + voxel_radiance.GridCenter) * voxel_radiance.DataSizeRCP;
        output.Position.xyz = (input[i].PositionWS.xyz) * voxel_radiance.DataSizeRCP;

		// Project onto dominant axis:
		[flatten]
        if (maxi == 0)
        {
            output.Position.xyz = output.Position.zyx;
        }
        else if (maxi == 1)
        {
            output.Position.xyz = output.Position.xzy;
        }

		// Voxel grid space -> Clip space
        output.Position.xy *= voxel_radiance.DataResRCP;
        output.Position.zw = float2(0, 1);
		
        output.PositionWS = input[i].PositionWS;

        outputStream.Append(output);    
    }
}