#include "Common/Color.hlsli"
#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
};

#if defined(PSHADER)
SamplerState ImageSampler : register(s0);
#	if defined(DOWNSAMPLE)
SamplerState AdaptSampler : register(s1);
#	elif defined(TONEMAP)
SamplerState BlendSampler : register(s1);
#	endif
SamplerState AvgSampler : register(s2);

Texture2D<float4> ImageTex : register(t0);
#	if defined(DOWNSAMPLE)
Texture2D<float4> AdaptTex : register(t1);
#	elif defined(TONEMAP)
Texture2D<float4> BlendTex : register(t1);
#	endif
Texture2D<float4> AvgTex : register(t2);

cbuffer PerGeometry : register(b2)
{
	float4 Flags : packoffset(c0);
	float4 TimingData : packoffset(c1);
	float4 Param : packoffset(c2);
	float4 Cinematic : packoffset(c3);
	float4 Tint : packoffset(c4);
	float4 Fade : packoffset(c5);
	float4 BlurScale : packoffset(c6);
	float4 BlurOffsets[16] : packoffset(c7);
};

float3 GetTonemapFactorReinhard(float3 luminance)
{
	return (luminance * (luminance * Param.y + 1)) / (luminance + 1);
}

float3 GetTonemapFactorHejlBurgessDawson(float3 luminance)
{
	float3 tmp = max(0, luminance - 0.004);
	return Param.y *
	       pow(((tmp * 6.2 + 0.5) * tmp) / (tmp * (tmp * 6.2 + 1.7) + 0.06), GammaCorrectionValue);
}

/*  AgX Reference:
 *  AgX by longbool https://www.shadertoy.com/view/dtSGD1
 *  AgX Minimal by bwrensch https://www.shadertoy.com/view/cd3XWr
 *  Fork AgX Minima troy_s 342 by troy_s https://www.shadertoy.com/view/mdcSDH
 */

// Mean error^2: 1.85907662e-06
float3 AgxDefaultContrastApprox6(float3 x)
{
	float3 x2 = x * x;
	float3 x4 = x2 * x2;

	return -17.86 * x4 * x2 * x + 78.01 * x4 * x2 - 126.7 * x4 * x + 92.06 * x4 -
	       28.72 * x2 * x + 4.361 * x2 - 0.1718 * x + 0.002857;
}

float3 Agx(float3 val)
{
	const float3x3 agx_mat = transpose(
		float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
			0.0784335999999992, 0.878468636469772, 0.0784336,
			0.0792237451477643, 0.0791661274605434, 0.879142973793104));

	const float min_ev = -12.47393f;
	const float max_ev = 4.026069f;

	// Input transform
	val = mul(agx_mat, val);

	// Log2 space encoding
	val = clamp(log2(val), min_ev, max_ev);
	val = (val - min_ev) / (max_ev - min_ev);

	// Apply sigmoid function approximation
	val = AgxDefaultContrastApprox6(val);

	return val;
}

float3 AgxEotf(float3 val)
{
	const float3x3 agx_mat_inv = transpose(
		float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
			-0.0980208811401368, 1.15190312990417, -0.0980434501171241,
			-0.0990297440797205, -0.0989611768448433, 1.15107367264116));

	// Undo input transform
	val = mul(agx_mat_inv, val);

	// sRGB IEC 61966-2-1 2.2 Exponent Reference EOTF Display
	// NOTE: We're linearizing the output here. Comment/adjust when
	// *not* using a sRGB render target
	val = pow(saturate(val), 2.2);

	return val;
}

float3 AgxMinimal(float3 val)
{
	val = Agx(val);
	val = AgxEotf(val);
	return val;
}

#	include "Color.hlsl"
#	include "DICE.hlsl"
#	include "Math.hlsl"
#	include "Oklab.hlsl"

// Takes any original color (before some post process is applied to it) and re-applies the same transformation the post process had applied to it on a different (but similar) color.
// The images are expected to have roughly the same mid gray.
// It can be used for example to apply any SDR LUT or SDR Color Correction on an HDR color.
float3 RestorePostProcess(float3 NonPostProcessedTargetColor, float3 NonPostProcessedSourceColor, float3 PostProcessedSourceColor)
{
	static const float MaxShadowsColor = pow(1.f / 3.f, 2.2);
	const float3 PostProcessColorRatio = safeDivision(PostProcessedSourceColor, NonPostProcessedSourceColor);
	const float3 PostProcessColorOffset = PostProcessedSourceColor - NonPostProcessedSourceColor;
	const float3 PostProcessedRatioColor = NonPostProcessedTargetColor * PostProcessColorRatio;
	const float3 PostProcessedOffsetColor = NonPostProcessedTargetColor + PostProcessColorOffset;
	// Near black, we prefer using the "offset" (sum) pp restoration method, as otherwise any raised black from post processing would not work to apply,
	// for example if any zero was shifted to a more raised color, "PostProcessColorRatio" would not be able to replicate that shift due to a division by zero.
	const float3 PostProcessedTargetColor = lerp(PostProcessedOffsetColor, PostProcessedRatioColor, max(saturate(abs(NonPostProcessedTargetColor) / MaxShadowsColor), saturate(abs(NonPostProcessedSourceColor) / MaxShadowsColor)));
	return PostProcessedTargetColor;
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

#	if defined(DOWNSAMPLE)
	float3 downsampledColor = 0;
	for (int sampleIndex = 0; sampleIndex < SAMPLES_COUNT; ++sampleIndex) {
		float2 texCoord = BlurOffsets[sampleIndex].xy * BlurScale.xy + input.TexCoord;
		if (Flags.x > 0.5) {
			texCoord = GetDynamicResolutionAdjustedScreenPosition(texCoord);
		}
		float3 bloomColor = ImageTex.Sample(ImageSampler, texCoord).xyz;
#		if defined(LUM)
		bloomColor = bloomColor.x;
#		elif defined(RGB2LUM)
		bloomColor = RGBToLuminance(bloomColor);
#		endif
		downsampledColor += bloomColor * BlurOffsets[sampleIndex].z;
	}
#		if defined(LIGHT_ADAPT)
	float2 adaptValue = AdaptTex.Sample(AdaptSampler, input.TexCoord).xy;
	if (isnan(downsampledColor.x) || isnan(downsampledColor.y) || isnan(downsampledColor.z)) {
		downsampledColor.xy = adaptValue;
	} else {
		float2 adaptDelta = downsampledColor.xy - adaptValue;
		downsampledColor.xy =
			sign(adaptDelta) * clamp(abs(Param.wz * adaptDelta), 0.00390625, abs(adaptDelta)) +
			adaptValue;
	}
#		endif
	psout.Color = float4(downsampledColor, BlurScale.z);

#	elif defined(TONEMAP)
	float2 uv = GetDynamicResolutionAdjustedScreenPosition(input.TexCoord);

	float3 inputColor = BlendTex.Sample(BlendSampler, uv).xyz;

	float3 bloomColor = 0;
	if (Flags.x > 0.5) {
		bloomColor = ImageTex.Sample(ImageSampler, uv).xyz;
	} else {
		bloomColor = ImageTex.Sample(ImageSampler, input.TexCoord.xy).xyz;
	}

	float2 avgValue = AvgTex.Sample(AvgSampler, input.TexCoord.xy).xy;

	// Apply bloom directly to the HDR input to match both tonemapping types
	inputColor += saturate(Param.x - RGBToLuminance(inputColor)) * bloomColor;

	// SDR tonemapping and post-processing
	// Tonemapping is modified to be per-channel, to match the HDR tonemapper
	float3 gameSdrColor = 0.0;
	float3 ppColor = 0.0;
	{
		float3 exposureAdjustedLuminance = (avgValue.y / avgValue.x) * inputColor;
		float3 blendFactor;
		if (Param.z > 0.5) {
			blendFactor = GetTonemapFactorHejlBurgessDawson(exposureAdjustedLuminance);
		} else {
			blendFactor = GetTonemapFactorReinhard(exposureAdjustedLuminance);
		}

		gameSdrColor = inputColor * (blendFactor / inputColor);

		float blendedLuminance = RGBToLuminance(gameSdrColor);

		ppColor = lerp(avgValue.x, Cinematic.w * lerp(lerp(blendedLuminance, gameSdrColor, Cinematic.x), blendedLuminance * Tint, Tint.w), Cinematic.z);
	}

	// Linearize game buffers, before they are used by HDR processing
	gameSdrColor = pow(gameSdrColor, 2.2);
	ppColor = pow(ppColor, 2.2);
	inputColor = pow(inputColor, 2.2);

	// In HDR, apply the SDR tonemapping and post-processing
	inputColor = RestorePostProcess(inputColor, gameSdrColor, ppColor);

	// Expand the gamut to better make use of the HDR color space
	inputColor = linear_srgb_to_oklch(inputColor);
	inputColor.y *= 1.25;
	inputColor = oklch_to_linear_srgb(inputColor);

	float3 hdrColor = DICETonemap(inputColor * 203, 1000) / 203;

#		if defined(FADE)
	// Linearize the fade color, paper-white brightness
	hdrColor = lerp(hdrColor, pow(Fade.xyz, 2.2), Fade.w);
#		endif

	// Convert the HDR image to the HDR10 color space
	float3 bt2020 = BT709_To_BT2020(hdrColor);
	psout.Color = float4(Linear_to_PQ((bt2020 * 203.0) / 10000.0), 1.0);

#	endif

	return psout;
}
#endif
