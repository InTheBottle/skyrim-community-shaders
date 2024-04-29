#include "VoxelUtil.hlsli"

struct PS_INPUT
{
    float4 Position     : SV_POSITION;
    float4 PositionWS   : POSITION;
};

RWStructuredBuffer<VoxelType> VoxelGrid     : register(u0);

cbuffer Voxel : register(b3)
{
	VoxelRadiance voxel_radiance;
};

void main(PS_INPUT input)
{  
    float3 diff = (input.PositionWS.xyz + voxel_radiance.GridCenter) * voxel_radiance.DataResRCP * voxel_radiance.DataSizeRCP;
    //float3 diff = (input.PositionWS.xyz) * voxel_radiance.DataResRCP * voxel_radiance.DataSizeRCP;

    float3 uvw = diff * float3(0.5f, -0.5f, 0.5f) + 0.5f;

    [branch]
    if (any(0 > uvw || 1 <= uvw))
    {
        return;
    }
    
    float3 normal = float3(0, 0, 1);

    uint colorEncoded = EncodeColor(1);
    uint normalEncoded = EncodeNormal(normal);
    
    uint3 writecoord = floor(uvw * voxel_radiance.DataRes);
    uint id = Flatten3D(writecoord, voxel_radiance.DataRes); 
    
    InterlockedMax(VoxelGrid[id].ColorMask, colorEncoded);
    InterlockedMax(VoxelGrid[id].NormalMask, normalEncoded);
}