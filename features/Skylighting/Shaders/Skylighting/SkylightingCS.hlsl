
Texture2D<unorm half> DepthTexture : register(t0);

struct PerGeometry
{
	float4 VPOSOffset;					            
	float4 ShadowSampleParam;   // fPoissonRadiusScale / iShadowMapResolution in z and w
	float4 EndSplitDistances;   // cascade end distances int xyz, cascade count int z
	float4 StartSplitDistances; // cascade start ditances int xyz, 4 int z
	float4 FocusShadowFadeParam;
	float4 DebugColor;					      
	float4 PropertyColor;					           
	float4 AlphaTestRef;					          
	float4 ShadowLightParam;   	// Falloff in x, ShadowDistance squared in z
	float4x3 FocusShadowMapProj[4];
	float4x3 ShadowMapProj[4];
	float4x4 CameraViewProjInverse;
};

Texture2DArray<float> TexShadowMapSampler : register(t1);
StructuredBuffer<PerGeometry> perShadow : register(t2);
Texture2DArray<float4> BlueNoise : register(t3);

RWTexture2D<unorm half> SkylightingTextureRW : register(u0);

cbuffer PerFrame : register(b0)
{
	float4 BufferDim;
	float4 DynamicRes;
	float4 CameraData;
	uint FrameCount;
};

SamplerState LinearSampler : register(s0);

half GetBlueNoise(half2 uv)
{
	return BlueNoise[uint3(uv % 128, FrameCount % 64)];
}

// Rotation with angle (in radians) and axis
// https://gist.github.com/keijiro/ee439d5e7388f3aafc5296005c8c3f33
half3x3 AngleAxis3x3(half angle, half3 axis)
{
    half c, s;
    sincos(angle, s, c);

    half t = 1 - c;
    half x = axis.x;
    half y = axis.y;
    half z = axis.z;

    return half3x3(
        t * x * x + c,      t * x * y - s * z,  t * x * z + s * y,
        t * x * y + s * z,  t * y * y + c,      t * y * z - s * x,
        t * x * z - s * y,  t * y * z + s * x,  t * z * z + c
    );
}

half GetScreenDepth(half depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

[numthreads(8, 8, 1)] void main(uint3 globalId : SV_DispatchThreadID) {
	half2 uv = half2(globalId.xy + 0.5) * BufferDim.zw * DynamicRes.zw;

	half rawDepth = DepthTexture[globalId.xy];

	half4 positionCS = half4(2 * half2(uv.x, -uv.y + 1) - 1, rawDepth, 1);

	PerGeometry sD = perShadow[0];
	sD.EndSplitDistances.x = GetScreenDepth(sD.EndSplitDistances.x);
	sD.EndSplitDistances.y = GetScreenDepth(sD.EndSplitDistances.y);
	sD.EndSplitDistances.z = GetScreenDepth(sD.EndSplitDistances.z);
	sD.EndSplitDistances.w = GetScreenDepth(sD.EndSplitDistances.w);

    half4 positionMS = mul(sD.CameraViewProjInverse, positionCS);
    positionMS.xyz = positionMS.xyz / positionMS.w;

	half3 startPositionMS = positionMS;

	half fadeFactor = pow(saturate(dot(positionMS.xyz, positionMS.xyz) / sD.ShadowLightParam.z), 8);

	half noise = GetBlueNoise(globalId.xy);
	
	half3x3 rotationMatrix = AngleAxis3x3(noise * 2.0 - 1.0, 0);

	half2 PoissonOffsets[32] = {
		half2(0.06407013, 0.05409927),
		half2(0.7366577, 0.5789394),
		half2(-0.6270542, -0.5320278),
		half2(-0.4096107, 0.8411095),
		half2(0.6849564, -0.4990818),
		half2(-0.874181, -0.04579735),
		half2(0.9989998, 0.0009880066),
		half2(-0.004920578, -0.9151649),
		half2(0.1805763, 0.9747483),
		half2(-0.2138451, 0.2635818),
		half2(0.109845, 0.3884785),
		half2(0.06876755, -0.3581074),
		half2(0.374073, -0.7661266),
		half2(0.3079132, -0.1216763),
		half2(-0.3794335, -0.8271583),
		half2(-0.203878, -0.07715034),
		half2(0.5912697, 0.1469799),
		half2(-0.88069, 0.3031784),
		half2(0.5040108, 0.8283722),
		half2(-0.5844124, 0.5494877),
		half2(0.6017799, -0.1726654),
		half2(-0.5554981, 0.1559997),
		half2(-0.3016369, -0.3900928),
		half2(-0.5550632, -0.1723762),
		half2(0.925029, 0.2995041),
		half2(-0.2473137, 0.5538505),
		half2(0.9183037, -0.2862392),
		half2(0.2469421, 0.6718712),
		half2(0.3916397, -0.4328209),
		half2(-0.03576927, -0.6220032),
		half2(-0.04661255, 0.7995201),
		half2(0.4402924, 0.3640312),
	};

	uint sampleCount = 32;
	half range = 256;

	half skylighting = 0;

	for (uint i = 0; i < sampleCount; i++) 
	{
		half3 offset = half3(PoissonOffsets[i].xy, 1.0 - PoissonOffsets[i].x) * range;
		offset = mul(offset, rotationMatrix);

		positionMS.xyz = startPositionMS + offset;

		half shadowMapDepth = length(positionMS.xyz);

		if (sD.EndSplitDistances.z > shadowMapDepth)
		{        
			half cascadeIndex = 0;
			half4x3 lightProjectionMatrix = sD.ShadowMapProj[0];
			half shadowMapThreshold = sD.AlphaTestRef.y;

			if (2.5 < sD.EndSplitDistances.w && sD.EndSplitDistances.y < shadowMapDepth)
			{
				lightProjectionMatrix = sD.ShadowMapProj[2];
				shadowMapThreshold = sD.AlphaTestRef.z;
				cascadeIndex = 2;
			}
			else if (sD.EndSplitDistances.x < shadowMapDepth)
			{
				lightProjectionMatrix = sD.ShadowMapProj[1];
				shadowMapThreshold = sD.AlphaTestRef.z;
				cascadeIndex = 1;
			}

			half3 positionLS = mul(transpose(lightProjectionMatrix), half4(positionMS.xyz, 1)).xyz;
			half shadowMapValue = TexShadowMapSampler.SampleLevel(LinearSampler, half3(positionLS.xy, cascadeIndex), 0).x;

			skylighting += saturate((shadowMapValue - (positionLS.z - shadowMapThreshold)) * 4096);
		}	
	}

	skylighting /= half(sampleCount);

	SkylightingTextureRW[globalId.xy] = lerp(saturate(skylighting), 1.0, fadeFactor);
}
