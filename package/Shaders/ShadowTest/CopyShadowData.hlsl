
struct PerGeometry
{
	float4 VPOSOffset;
	float4 ShadowSampleParam;    // fPoissonRadiusScale / iShadowMapResolution in z and w
	float4 EndSplitDistances;    // cascade end distances int xyz, cascade count int z
	float4 StartSplitDistances;  // cascade start ditances int xyz, 4 int z
	float4 FocusShadowFadeParam;
	float4 DebugColor;
	float4 PropertyColor;
	float4 AlphaTestRef;
	float4 ShadowLightParam;  // Falloff in x, ShadowDistance squared in z
	float4x3 FocusShadowMapProj[4];
	float4x3 ShadowMapProj[4];
	float4x4 CameraViewProjInverse;
};

cbuffer PerFrame : register(b0)
{
	float4 DebugColor;
	float4 PropertyColor;
	float4 AlphaTestRef;
	float4 ShadowLightParam;  // Falloff in x, ShadowDistance squared in z
	float4x3 FocusShadowMapProj[4];
	float4x3 ShadowMapProj[4];
}

cbuffer PerFrame2 : register(b1)
{
	row_major float4x4 CameraView : packoffset(c0);
	row_major float4x4 CameraProj : packoffset(c4);
	row_major float4x4 CameraViewProj : packoffset(c8);
	row_major float4x4 CameraViewProjUnjittered : packoffset(c12);
	row_major float4x4 CameraPreviousViewProjUnjittered : packoffset(c16);
	row_major float4x4 CameraProjUnjittered : packoffset(c20);
	row_major float4x4 CameraProjUnjitteredInverse : packoffset(c24);
	row_major float4x4 CameraViewInverse : packoffset(c28);
	row_major float4x4 CameraViewProjInverse : packoffset(c32);
	row_major float4x4 CameraProjInverse : packoffset(c36);
	float4 CameraPosAdjust : packoffset(c40);
	float4 CameraPreviousPosAdjust : packoffset(c41);   // fDRClampOffset in w
	float4 FrameParams : packoffset(c42);               // inverse fGamma in x, some flags in yzw
	float4 DynamicResolutionParams1 : packoffset(c43);  // fDynamicResolutionWidthRatio in x, fDynamicResolutionHeightRatio in y, fDynamicResolutionPreviousWidthRatio in z, fDynamicResolutionPreviousHeightRatio in w
	float4 DynamicResolutionParams2 : packoffset(c44);  // inverse fDynamicResolutionWidthRatio in x, inverse fDynamicResolutionHeightRatio in y, fDynamicResolutionWidthRatio - fDRClampOffset in z, fDynamicResolutionPreviousWidthRatio - fDRClampOffset in w
}

cbuffer PerFrame3 : register(b2)
{
	float4 VPOSOffset : packoffset(c0);
	float4 ShadowSampleParam : packoffset(c1);    // fPoissonRadiusScale / iShadowMapResolution in z and w
	float4 EndSplitDistances : packoffset(c2);    // cascade end distances int xyz, cascade count int z
	float4 StartSplitDistances : packoffset(c3);  // cascade start ditances int xyz, 4 int z
	float4 FocusShadowFadeParam : packoffset(c4);
}

RWStructuredBuffer<PerGeometry> copiedData : register(u0);

[numthreads(1, 1, 1)] void main() {
	PerGeometry perGeometry;
	perGeometry.DebugColor = DebugColor;
	perGeometry.PropertyColor = PropertyColor;
	perGeometry.AlphaTestRef = AlphaTestRef;
	perGeometry.ShadowLightParam = ShadowLightParam;
	perGeometry.FocusShadowMapProj = FocusShadowMapProj;
	perGeometry.ShadowMapProj = ShadowMapProj;

	perGeometry.CameraViewProjInverse = CameraViewProjInverse;

	perGeometry.VPOSOffset = VPOSOffset;
	perGeometry.ShadowSampleParam = ShadowSampleParam;
	perGeometry.EndSplitDistances = EndSplitDistances;
	perGeometry.StartSplitDistances = StartSplitDistances;
	perGeometry.FocusShadowFadeParam = FocusShadowFadeParam;

	copiedData[0] = perGeometry;
}