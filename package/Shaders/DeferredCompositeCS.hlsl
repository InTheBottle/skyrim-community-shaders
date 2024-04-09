
#include "Common/DeferredShared.hlsl"
#include "Common/GBuffer.hlsl"

Texture2D<half3> SpecularTexture : register(t0);
Texture2D<unorm half4> AlbedoTexture : register(t1);
Texture2D<unorm half3> ReflectanceTexture : register(t2);
Texture2D<unorm half4> NormalRoughnessTexture : register(t3);
Texture2D<unorm half> ShadowMaskTexture : register(t4);
Texture2D<unorm half> DepthTexture : register(t5);
Texture2D<unorm half4> GITexture : register(t6);

RWTexture2D<half4> MainRW : register(u0);
RWTexture2D<half4> NormalTAAMaskSpecularMaskRW : register(u1);

SamplerState LinearSampler : register(s0);

// #	define DEBUG

half GetScreenDepth(half depth)
{
	return (CameraData.w / (-depth * CameraData.z + CameraData.x));
}

half2 WorldToUV(half3 x, bool is_position = true, uint a_eyeIndex = 0)
{
	half4 newPosition = half4(x, (half)is_position);
	half4 uv = mul(ViewProjMatrix[a_eyeIndex], newPosition);
	return (uv.xy / uv.w) * half2(0.5f, -0.5f) + 0.5f;
}

half InterleavedGradientNoise(half2 uv)
{
	// Temporal factor
	half frameStep = half(FrameCount % 16) * 0.0625f;
	uv.x += frameStep * 4.7526;
	uv.y += frameStep * 3.1914;

	half3 magic = half3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(uv, magic.xy)));
}

// Inverse project UV + raw depth into the view space.
half3 DepthToView(half2 uv, half z, uint a_eyeIndex)
{
	uv.y = 1 - uv.y;
	half4 cp = half4(uv * 2 - 1, z, 1);
	half4 vp = mul(InvProjMatrix[a_eyeIndex], cp);
	return vp.xyz / vp.w;
}

half2 ViewToUV(half3 position, bool is_position, uint a_eyeIndex)
{
	half4 uv = mul(ProjMatrix[a_eyeIndex], half4(position, (half)is_position));
	return (uv.xy / uv.w) * half2(0.5f, -0.5f) + 0.5f;
}


[numthreads(32, 32, 1)] void DirectionalPass(uint3 globalId
												   : SV_DispatchThreadID, uint3 localId
												   : SV_GroupThreadID, uint3 groupId
												   : SV_GroupID) {
	half2 uv = half2(globalId.xy + 0.5) * RcpBufferDim.xy;

	half3 normalGlossiness = NormalRoughnessTexture[globalId.xy];
	half3 normalVS = DecodeNormal(normalGlossiness.xyz);

	half rawDepth = DepthTexture[globalId.xy];
	half depth = GetScreenDepth(rawDepth);

	half weight = 1.0;

	half NdotL = dot(normalVS, DirLightDirectionVS[0].xyz);

	half4 albedo = AlbedoTexture[globalId.xy];

	half3 color = MainRW[globalId.xy].rgb;
	color += albedo * lerp(max(0, NdotL), 1.0, albedo.w) * DirLightColor.xyz;

	MainRW[globalId.xy] = half4(color.xyz, 1.0);
};

[numthreads(32, 32, 1)] void DirectionalShadowPass(uint3 globalId
												   : SV_DispatchThreadID, uint3 localId
												   : SV_GroupThreadID, uint3 groupId
												   : SV_GroupID) {
	half2 uv = half2(globalId.xy + 0.5) * RcpBufferDim.xy;

	half3 normalGlossiness = NormalRoughnessTexture[globalId.xy];
	half3 normalVS = DecodeNormal(normalGlossiness.xyz);

	half rawDepth = DepthTexture[globalId.xy];
	half depth = GetScreenDepth(rawDepth);
	
	half weight = 1.0;

	half NdotL = dot(normalVS, DirLightDirectionVS[0].xyz);
	half shadow = 1.0;

	if (NdotL > 0.0) {
		shadow = ShadowMaskTexture[globalId.xy];

		if (shadow != 0){
			// Approximation of PCF in screen-space
			for (int i = -1; i < 1; i++) {
				for (int k = -1; k < 1; k++) {
					if (i == 0 && k == 0)
						continue;
					float2 offset = float2(i, k) * RcpBufferDim.xy * 1.5;
					float sampleDepth = GetScreenDepth(DepthTexture.SampleLevel(LinearSampler, uv + offset, 0));
					float attenuation = 1.0 - saturate(abs(sampleDepth - depth));
					shadow += ShadowMaskTexture.SampleLevel(LinearSampler, uv + offset, 0) * attenuation;
					weight += attenuation;
				}
			}
			shadow /= weight;
		}
	}

	half4 albedo = AlbedoTexture[globalId.xy];

	half3 color = MainRW[globalId.xy].rgb;
	color += albedo * lerp(max(0, NdotL), 1.0, albedo.w) * DirLightColor.xyz * shadow;

	MainRW[globalId.xy] = half4(color.xyz, 1.0);
};

[numthreads(32, 32, 1)] void AmbientCompositePass(uint3 globalId
											   : SV_DispatchThreadID, uint3 localId
											   : SV_GroupThreadID, uint3 groupId
											   : SV_GroupID) {
	half2 uv = half2(globalId.xy + 0.5) * RcpBufferDim.xy;

	half3 normalGlossiness = NormalRoughnessTexture[globalId.xy];
	half3 normalVS = DecodeNormal(normalGlossiness.xyz);

	uint eyeIndex = 0;

	half4 diffuseColor = MainRW[globalId.xy];

	half3 normalWS = normalize(mul(InvViewMatrix[0], half4(normalVS, 0)));

	half4 albedo = AlbedoTexture[globalId.xy];

	half4 giAo = GITexture[globalId.xy];
	half3 gi = giAo.rgb;
	half ao = giAo.w;

	half3 directionalAmbientColor = mul(DirectionalAmbient, half4(normalWS, 1.0));
	diffuseColor.rgb += albedo * directionalAmbientColor * ao + gi;

	MainRW[globalId.xy] = half4(diffuseColor.xyz, 1.0);
}

[numthreads(32, 32, 1)] void MainCompositePass(uint3 globalId
											   : SV_DispatchThreadID, uint3 localId
											   : SV_GroupThreadID, uint3 groupId
											   : SV_GroupID) {
	half2 uv = half2(globalId.xy + 0.5) * RcpBufferDim.xy;

	half3 normalGlossiness = NormalRoughnessTexture[globalId.xy];
	half3 normalVS = DecodeNormal(normalGlossiness.xyz);

	uint eyeIndex = 0;

	half4 diffuseColor = MainRW[globalId.xy];
	half3 specularColor = SpecularTexture[globalId.xy];

	half3 normalWS = normalize(mul(InvViewMatrix[0], half4(normalVS, 0)));

	half glossiness = normalGlossiness.z;
	half3 color = diffuseColor + specularColor;

#if defined(DEBUG)
	half2 texCoord = half2(globalId.xy) / BufferDim.xy;

	if (texCoord.x < 0.5 && texCoord.y < 0.5) {
		color = color;
	} else if (texCoord.x < 0.5) {
		color = albedo;
	} else if (texCoord.y < 0.5) {
		color = normalWS;
	} else {
		color = glossiness;
	}
#endif

	MainRW[globalId.xy] = half4(color.xyz, 1.0);
	NormalTAAMaskSpecularMaskRW[globalId.xy] = half4(EncodeNormalVanilla(normalVS), 0.0, glossiness);
}