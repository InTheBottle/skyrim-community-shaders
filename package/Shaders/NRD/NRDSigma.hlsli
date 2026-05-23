// Minimal SIGMA front-end/back-end helpers inlined from NRD v4 (NRD.hlsli).
// Only the directional light variant is included.

#ifndef NRD_SIGMA_HLSLI
#define NRD_SIGMA_HLSLI

#ifndef NRD_FP16_MAX
#	define NRD_FP16_MAX 65504.0
#endif

// Directional light source
// distanceToOccluder rules:
//   NoL <= 0        => 0
//   NoL > 0 && hit  => hit distance
//   NoL > 0 && miss => >= NRD_FP16_MAX
float SIGMA_FrontEnd_PackPenumbra(float distanceToOccluder, float tanOfLightAngularRadius)
{
	float penumbraSize = distanceToOccluder * tanOfLightAngularRadius;
	float penumbraRadius = penumbraSize * 0.5;
	return distanceToOccluder >= NRD_FP16_MAX ? NRD_FP16_MAX : min(penumbraRadius, 32768.0);
}

// shadow * shadow recovers linear shadow from sqrt-packed output
#define SIGMA_BackEnd_UnpackShadow(shadow) ((shadow) * (shadow))

#endif  // NRD_SIGMA_HLSLI
