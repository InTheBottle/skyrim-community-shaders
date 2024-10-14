#include "Common/Color.hlsli"
#include "Common/DICETonemapper.hlsli"
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
#	elif defined(BLEND)
SamplerState BlendSampler : register(s1);
#	endif
SamplerState AvgSampler : register(s2);

Texture2D<float4> ImageTex : register(t0);
#	if defined(DOWNSAMPLE)
Texture2D<float4> AdaptTex : register(t1);
#	elif defined(BLEND)
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

float GetTonemapFactorReinhard(float luminance)
{
	return (luminance * (luminance * Param.y + 1)) / (luminance + 1);
}

float GetTonemapFactorHejlBurgessDawson(float luminance)
{
	float tmp = max(0, luminance - 0.004);
	return Param.y *
	       pow(((tmp * 6.2 + 0.5) * tmp) / (tmp * (tmp * 6.2 + 1.7) + 0.06), Color::GammaCorrectionValue);
}

float3 BT709ToOKLab(float3 bt709) {
	static const float3x3 BT709_2_OKLABLMS = {
	0.4122214708f, 0.5363325363f, 0.0514459929f,
	0.2119034982f, 0.6806995451f, 0.1073969566f,
	0.0883024619f, 0.2817188376f, 0.6299787005f
	};

	static const float3x3 OKLABLMS_2_OKLAB = {
	0.2104542553f, 0.7936177850f, -0.0040720468f,
	1.9779984951f, -2.4285922050f, 0.4505937099f,
	0.0259040371f, 0.7827717662f, -0.8086757660f
	};

	float3 lms = mul(BT709_2_OKLABLMS, bt709);

	lms = pow(abs(lms), 1.0 / 3.0) * sign(lms);

	return mul(OKLABLMS_2_OKLAB, lms);
}

float3 OkLabToBT709(float3 oklab) {
	static const float3x3 OKLAB_2_OKLABLMS = {
	1.f, 0.3963377774f, 0.2158037573f,
	1.f, -0.1055613458f, -0.0638541728f,
	1.f, -0.0894841775f, -1.2914855480f
	};

	static const float3x3 OKLABLMS_2_BT709 = {
	4.0767416621f, -3.3077115913f, 0.2309699292f,
	-1.2684380046f, 2.6097574011f, -0.3413193965f,
	-0.0041960863f, -0.7034186147f, 1.7076147010f
	};

	float3 lms = mul(OKLAB_2_OKLABLMS, oklab);

	lms = lms * lms * lms;

	return mul(OKLABLMS_2_BT709, lms);
}

float3 ExtendGamut(float3 color, float extendGamutAmount)
{
  float3 colorOKLab = BT709ToOKLab(color);

  // Extract L, C, h from OKLab
  float L = colorOKLab[0];
  float a = colorOKLab[1];
  float b = colorOKLab[2];
  float C = sqrt(a * a + b * b);
  float h = atan2(b, a);

  // Calculate the exponential weighting factor based on luminance and chroma
  float chromaWeight = 1.0f - exp(-4.0f * C);
  float luminanceWeight = 1.0f - exp(-4.0f * L * L);
  float weight = chromaWeight * luminanceWeight * extendGamutAmount;

  // Apply the expansion factor
  C *= (1.0f + weight);

  // Convert back to OKLab with adjusted chroma
  a = C * cos(h);
  b = C * sin(h);
  float3 adjustedOKLab = float3(L, a, b);

  float3 adjustedColor = OkLabToBT709(adjustedOKLab);
  return adjustedColor;
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

#	if defined(DOWNSAMPLE)
	float3 downsampledColor = 0;
	for (int sampleIndex = 0; sampleIndex < DOWNSAMPLE; ++sampleIndex) {
		float2 texCoord = BlurOffsets[sampleIndex].xy * BlurScale.xy + input.TexCoord;
		[branch] if (Flags.x > 0.5)
		{
			texCoord = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(texCoord);
		}
		float3 imageColor = ImageTex.Sample(ImageSampler, texCoord).xyz;
#		if defined(RGB2LUM)
		imageColor = Color::RGBToLuminance(imageColor);
#		elif (defined(LUM) || defined(LUMCLAMP)) && !defined(DOWNADAPT)
		imageColor = imageColor.x;
#		endif
		downsampledColor += imageColor * BlurOffsets[sampleIndex].z;
	}
#		if defined(DOWNADAPT)
	float2 adaptValue = AdaptTex.Sample(AdaptSampler, input.TexCoord).xy;
	if (isnan(downsampledColor.x) || isnan(downsampledColor.y) || isnan(downsampledColor.z)) {
		downsampledColor.xy = adaptValue;
	} else {
		float2 adaptDelta = downsampledColor.xy - adaptValue;
		downsampledColor.xy =
			sign(adaptDelta) * clamp(abs(Param.wz * adaptDelta), 0.00390625, abs(adaptDelta)) +
			adaptValue;
	}
	downsampledColor = max(asfloat(0x00800000), downsampledColor);  // Black screen fix
#		endif
	psout.Color = float4(downsampledColor, BlurScale.z);

#	elif defined(BLEND)
	float2 uv = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(input.TexCoord);

	float3 inputColor = BlendTex.Sample(BlendSampler, uv).xyz;

	float3 bloomColor = 0;
	if (Flags.x > 0.5) {
		bloomColor = ImageTex.Sample(ImageSampler, uv).xyz;
	} else {
		bloomColor = ImageTex.Sample(ImageSampler, input.TexCoord.xy).xyz;
	}

	float2 avgValue = AvgTex.Sample(AvgSampler, input.TexCoord.xy).xy;

	// Vanilla tonemapping and post-processing
	float3 gameSdrColor = 0.0;
	float3 ppColor = 0.0;
	{
		float luminance = max(1e-5, Color::RGBToLuminance(inputColor));
		float exposureAdjustedLuminance = (avgValue.y / avgValue.x) * luminance;
		float blendFactor;
		if (Param.z > 0.5) {
			blendFactor = GetTonemapFactorHejlBurgessDawson(exposureAdjustedLuminance);
		} else {
			blendFactor = GetTonemapFactorReinhard(exposureAdjustedLuminance);
		}

		float3 blendedColor = inputColor * (blendFactor / luminance);
		blendedColor += saturate(Param.x - blendFactor) * bloomColor;

		gameSdrColor = blendedColor;

		float blendedLuminance = Color::RGBToLuminance(blendedColor);

		float3 linearColor = Cinematic.w * lerp(lerp(blendedLuminance, float4(blendedColor, 1), Cinematic.x), blendedLuminance * Tint, Tint.w);

		// Contrast modified to fix crushed shadows
		linearColor = pow(abs(linearColor) / avgValue.x, Cinematic.z) * avgValue.x * sign(linearColor);

		gameSdrColor = max(0, gameSdrColor);
		ppColor = max(0, linearColor);
	}

	// HDR tonemapping

	float3 hdrColor = Color::GammaToLinear(ppColor);

	float peakWhite = 1000.0 / 203.0;

	float shoulder = Color::GammaToLinear(0.5);
	shoulder /= peakWhite;
	shoulder = Color::LinearToGamma(shoulder);

	hdrColor = ApplyHuePreservingShoulder(hdrColor / peakWhite, shoulder) * peakWhite;
	
	hdrColor = Color::LinearToGamma(hdrColor);

#		if defined(FADE)
	hdrColor = lerp(hdrColor, Fade.xyz, Fade.w);
#		endif

	hdrColor = FrameBuffer::ToSRGBColor(hdrColor);
	
	psout.Color = float4(hdrColor, 1.0);

#	endif

	return psout;
}
#endif
