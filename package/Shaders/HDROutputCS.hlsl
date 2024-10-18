
#include "Common/Color.hlsli"
#include "Common/DICETonemapper.hlsli"

Texture2D<float4> Framebuffer : register(t0);
Texture2D<float4> UI : register(t1);

RWTexture2D<float4> HDROutput : register(u0);

cbuffer PerFrame : register(b0)
{
	float4 HDRData;
};

float3 BT709ToOKLab(float3 bt709)
{
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

float3 OkLabToBT709(float3 oklab)
{
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

[numthreads(8, 8, 1)] void main(uint3 dispatchID
								: SV_DispatchThreadID) {
	float3 framebuffer = Framebuffer[dispatchID.xy];
	float4 ui = UI[dispatchID.xy];

	// Scale framebuffer and UI brightness relative to game brightness
    framebuffer = Color::GammaToLinearSafe(framebuffer);
    // framebuffer = ExtendGamut(framebuffer, 0.2);
    framebuffer *= HDRData.y / HDRData.z;
	framebuffer = Color::LinearToGammaSafe(framebuffer);

	ui.xyz = Color::GammaToLinear(ui.xyz);
	ui.xyz *= HDRData.w / HDRData.z;
	ui.xyz = Color::LinearToGamma(ui.xyz);

	// Apply the Reinhard tonemapper on any background color in excess, to avoid it burning it through the UI.
	float3 excessBackgroundColor = framebuffer - min(1.0, framebuffer);
	float3 tonemappedBackgroundColor = excessBackgroundColor / (1.0 + excessBackgroundColor);
	framebuffer = min(1.0, framebuffer) + lerp(tonemappedBackgroundColor, excessBackgroundColor, 1.0 - ui.a);

	// Blend UI
	framebuffer = ui.xyz + framebuffer * (1.0 - ui.a);

	framebuffer = Color::GammaToLinearSafe(framebuffer);

	// Scale framebuffer to game brightness
	framebuffer *= HDRData.z;

	framebuffer = BT709ToBT2020(framebuffer);
	framebuffer = LinearToPQ(framebuffer, 10000.0);

	HDROutput[dispatchID.xy] = float4(framebuffer, 1.0);
};