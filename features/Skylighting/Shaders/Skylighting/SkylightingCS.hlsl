
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
Texture2D<float4> BlueNoise : register(t3);

RWTexture2D<unorm half> SkylightingTextureRW : register(u0);

cbuffer PerFrame : register(b0)
{
	float4 BufferDim;
	float4 DynamicRes;
	uint FrameCount;
};

SamplerState LinearSampler : register(s0);

half InterleavedGradientNoise(half2 uv)
{
	// Temporal factor
	half frameStep = half(FrameCount % 16) * 0.0625f;
	uv.x += frameStep * 4.7526;
	uv.y += frameStep * 3.1914;

	half3 magic = half3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(uv, magic.xy)));
}

half GetBlueNoise(half2 uv)
{
	return BlueNoise[uv % 32];
}

float2 rotate(float2 pos, float2 rotationTrig)
{
	return float2(pos.x * rotationTrig.x - pos.y * rotationTrig.y, pos.y * rotationTrig.x + pos.x * rotationTrig.y);
}

[numthreads(8, 8, 1)] void main(uint3 globalId : SV_DispatchThreadID) {
	half2 uv = half2(globalId.xy + 0.5) * BufferDim.zw * DynamicRes.zw;

	half rawDepth = DepthTexture[globalId.xy];

	float4 positionCS = float4(2 * float2(uv.x, -uv.y + 1) - 1, rawDepth, 1);

	PerGeometry sD = perShadow[0];

    float4 positionMS = mul(sD.CameraViewProjInverse, positionCS);
    positionMS.xyz = positionMS.xyz / positionMS.w;

	float skylighting = 0;

	float3 positionMSstart = positionMS;

	float fadeFactor = saturate(dot(positionMS.xyz, positionMS.xyz) / sD.ShadowLightParam.z);

	float noise = GetBlueNoise(globalId.xy);

	float2 PoissonOffsets[32] = {
		float2(0.06407013, 0.05409927),
		float2(0.7366577, 0.5789394),
		float2(-0.6270542, -0.5320278),
		float2(-0.4096107, 0.8411095),
		float2(0.6849564, -0.4990818),
		float2(-0.874181, -0.04579735),
		float2(0.9989998, 0.0009880066),
		float2(-0.004920578, -0.9151649),
		float2(0.1805763, 0.9747483),
		float2(-0.2138451, 0.2635818),
		float2(0.109845, 0.3884785),
		float2(0.06876755, -0.3581074),
		float2(0.374073, -0.7661266),
		float2(0.3079132, -0.1216763),
		float2(-0.3794335, -0.8271583),
		float2(-0.203878, -0.07715034),
		float2(0.5912697, 0.1469799),
		float2(-0.88069, 0.3031784),
		float2(0.5040108, 0.8283722),
		float2(-0.5844124, 0.5494877),
		float2(0.6017799, -0.1726654),
		float2(-0.5554981, 0.1559997),
		float2(-0.3016369, -0.3900928),
		float2(-0.5550632, -0.1723762),
		float2(0.925029, 0.2995041),
		float2(-0.2473137, 0.5538505),
		float2(0.9183037, -0.2862392),
		float2(0.2469421, 0.6718712),
		float2(0.3916397, -0.4328209),
		float2(-0.03576927, -0.6220032),
		float2(-0.04661255, 0.7995201),
		float2(0.4402924, 0.3640312),
	};

	float rotationAngle = noise * 3.1415926;
	float2 rotationTrig = float2(cos(rotationAngle), sin(rotationAngle));

	uint sampleCount = 32;
	float range = 256;
	
	for (uint i = 0; i < sampleCount; i++) {
		float2 offset = PoissonOffsets[i] * range;
		offset = rotate(offset, rotationTrig);

		positionMS.xy = positionMSstart + offset;
		float shadowMapDepth = length(positionMS);
		if (10000 > shadowMapDepth)
		{        
			float cascadeIndex = 0;
			float4x3 lightProjectionMatrix = sD.ShadowMapProj[0];
			float shadowMapThreshold = sD.AlphaTestRef.y;

			if (1000 < shadowMapDepth)
			{
				lightProjectionMatrix = sD.ShadowMapProj[1];
				shadowMapThreshold = sD.AlphaTestRef.z;
				cascadeIndex = 1;
			}
					
			float3 positionLS = mul(transpose(lightProjectionMatrix), float4(positionMS.xyz, 1)).xyz;
			float shadowMapValue = TexShadowMapSampler.SampleLevel(LinearSampler, float3(positionLS.xy, cascadeIndex), 0).x;

			skylighting += saturate((shadowMapValue - (positionLS.z - shadowMapThreshold)) * 4096);
		}	
	}

	skylighting /= float(sampleCount);

	SkylightingTextureRW[globalId.xy] = lerp(saturate(skylighting), 1.0, fadeFactor);
}
