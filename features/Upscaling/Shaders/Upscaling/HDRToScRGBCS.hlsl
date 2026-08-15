#include "Common/Color.hlsli"

Texture2D<float4> HDR10Input : register(t0);
RWTexture2D<float4> ScRGBOutput : register(u0);

#ifdef HDR10_SCRGB_GAMMA_ENCODE
float3 LinearToSRGB(float3 color)
{
	float3 low = color * 12.92;
	float3 high = 1.055 * pow(max(color, 0.0), 1.0 / 2.4) - 0.055;
	return lerp(high, low, color <= 0.0031308);
}
#endif

[numthreads(8, 8, 1)]
void main(uint3 dispatchID : SV_DispatchThreadID)
{
	uint width, height;
	ScRGBOutput.GetDimensions(width, height);
	if (dispatchID.x >= width || dispatchID.y >= height)
		return;

	float3 hdr10 = saturate(HDR10Input[dispatchID.xy].rgb);
	float3 outputColor = Color::BT2020ToBT709(Color::pq::Decode(hdr10, 80.0));
#ifdef HDR10_SCRGB_GAMMA_ENCODE
	outputColor = LinearToSRGB(max(outputColor, 0.0));
#endif
	ScRGBOutput[dispatchID.xy] = float4(outputColor, 1.0);
}
