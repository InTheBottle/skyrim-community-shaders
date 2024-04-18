
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

[numthreads(8, 8, 1)] void main(uint3 globalId : SV_DispatchThreadID) {
	half2 uv = half2(globalId.xy + 0.5) * BufferDim.zw * DynamicRes.zw;

	half rawDepth = DepthTexture[globalId.xy];

	float4 positionCS = float4(2 * float2(uv.x, -uv.y + 1) - 1, rawDepth, 1);

	PerGeometry sD = perShadow[0];

    float4 positionMS = mul(sD.CameraViewProjInverse, positionCS);
    positionMS.xyz = positionMS.xyz / positionMS.w;

	float skylighting = 0;
	float weight = 0;

	float3 positionMSstart = positionMS;

	float fadeFactor = saturate(dot(positionMS.xyz, positionMS.xyz) / sD.ShadowLightParam.z);

	float sampleCount = 10;
	float range = 256;
	range /= sampleCount;

	float jitter = InterleavedGradientNoise(globalId.xy);
	float2x2 rotationMatrix = float2x2(cos(jitter), sin(jitter), -sin(jitter), cos(jitter));

	for (float i = -sampleCount; i < sampleCount; i++) {
		for (float k = -sampleCount; k < sampleCount; k++) {
			positionMS.xy = positionMSstart + mul(float2(i, k), rotationMatrix) * range;
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
				weight++;
			}
		}
	}

	if (weight)
	{
		skylighting /= weight;
	} else {
		skylighting = 1;
	}

	SkylightingTextureRW[globalId.xy] = lerp(saturate(skylighting), 1.0, fadeFactor);
}
