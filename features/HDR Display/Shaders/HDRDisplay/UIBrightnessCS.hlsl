// Preprocess vanilla UI for Frame Gen compositing.
// HDR path converts UI to PQ/BT.2020 using configured paper white.
// SDR path keeps gamma UI and only clamps negatives.

#include "Common/Color.hlsli"

RWTexture2D<float4> UITex : register(u0);

cbuffer PerFrame : register(b0)
{
	float enableHDR : packoffset(c0.x);                 ///< 1.0 = HDR output with PQ, 0.0 = SDR output with gamma
	float paperWhite : packoffset(c0.y);                ///< Reference white in nits (used by HDR UI conversion)
	float peakNits : packoffset(c0.z);                  ///< Maximum display brightness in nits for HDR (unused here)
	float skipUIComposite : packoffset(c0.w);           ///< Unused in this shader
	float uiBrightness : packoffset(c1.x);              ///< UI brightness multiplier
	float isSceneLinear : packoffset(c1.y);             ///< Unused in this shader
	float isMainOrLoadingMenu : packoffset(c1.z);       ///< Unused; layout matches HDRDataCB
	float fgTweenMenuMidAlphaBoost : packoffset(c1.w);  ///< Unused in this shader
}

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	// Bounds check to prevent UAV out-of-bounds reads/writes
	uint width, height;
	UITex.GetDimensions(width, height);
	if (dispatchID.x >= width || dispatchID.y >= height)
		return;

	float4 ui = UITex[dispatchID.xy];

	bool hdrEnabled = enableHDR > 0.5;

	if (hdrEnabled) {
		float3 uiLinear = Color::SrgbToLinear(max(0, ui.rgb));
		uiLinear *= uiBrightness;
		ui.rgb = Color::pq::Encode(Color::BT709ToBT2020(uiLinear), paperWhite);
	} else {
		// SDR path: keep premultiplied gamma UI, clamp negatives only.
		ui.rgb = max(0, ui.rgb);
		UITex[dispatchID.xy] = ui;
		return;
	}

	UITex[dispatchID.xy] = ui;
}
