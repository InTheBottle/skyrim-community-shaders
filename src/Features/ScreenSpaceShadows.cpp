#include "ScreenSpaceShadows.h"
#include "Features/ReverseZ.h"

#include "Deferred.h"
#include "Features/TerrainBlending.h"
#include "Features/TerrainShadows.h"
#include "I18n/I18n.h"
#include "State.h"
#include "Utils/D3D.h"

#define I18N_KEY_PREFIX "feature.screen_space_shadows."

#pragma warning(push)
#pragma warning(disable: 4838 4244)
#include "ScreenSpaceShadows/bend_sss_cpu.h"
#pragma warning(pop)

using RE::RENDER_TARGETS;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceShadows::BendSettings,
	Enable,
	SampleCount,
	SurfaceThickness,
	BilinearThreshold,
	ShadowContrast)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceShadows::DistantSettings,
	Enable,
	SampleCount,
	MaxRayLength,
	Intensity,
	Thickness,
	Method,
	MapResolution,
	MapRange,
	MinCasterSize,
	UpdateInterval,
	FilterRadius,
	Bias,
	TreeLOD)

static constexpr const char* DistantSettingsKey = "DistantShadows";

void ScreenSpaceShadows::DrawSettings()
{
	if (ImGui::TreeNodeEx(T(TKEY("general"), "General"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("enable"), "Enable"), (bool*)&bendSettings.Enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("enable_tooltip"), "Enable screen-space contact shadows from the sun/moon direction."));

		ImGui::SliderInt(T(TKEY("sample_count"), "Sample Count Multiplier"), (int*)&bendSettings.SampleCount, 1, 4);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("sample_count_tooltip"), "Multiplier for shadow ray sample count. Higher values increase shadow reach at the cost of performance. Adapts to render resolution."));

		ImGui::SliderFloat(T(TKEY("surface_thickness"), "Surface Thickness"), &bendSettings.SurfaceThickness, 0.005f, 0.05f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("surface_thickness_tooltip"), "Assumed thickness of surfaces for shadow detection. Lower values produce thinner, more precise shadows."));

		ImGui::SliderFloat(T(TKEY("bilinear_threshold"), "Bilinear Threshold"), &bendSettings.BilinearThreshold, 0.02f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("bilinear_threshold_tooltip"), "Depth threshold for edge detection during bilinear interpolation. Higher values smooth more aggressively across edges."));

		ImGui::SliderFloat(T(TKEY("shadow_contrast"), "Shadow Contrast"), &bendSettings.ShadowContrast, 0.0f, 4.0f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("shadow_contrast_tooltip"), "Contrast boost for the shadow transition. Higher values produce harder shadow edges."));

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx(T(TKEY("distant_shadows"), "Distant LOD Shadows"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox(T(TKEY("distant_enable"), "Enable Distant Shadows"), &distantSettings.Enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("distant_enable_tooltip"), "Casts sun/moon shadows from object LOD, tree LOD and large objects beyond the shadow map distance, where distant LOD otherwise receives no shadows."));

		const char* methods[] = { T(TKEY("distant_method_shadow_map"), "Shadow Map"), T(TKEY("distant_method_screen_space"), "Screen Space") };
		int method = static_cast<int>(std::min(distantSettings.Method, 1u));
		if (ImGui::Combo(T(TKEY("distant_method"), "Distant Shadow Method"), &method, methods, IM_ARRAYSIZE(methods)))
			distantSettings.Method = static_cast<uint>(method);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("distant_method_tooltip"), "Shadow Map renders distant geometry from the sun, so off-screen casters work.\nScreen Space traces the depth buffer and only sees what is on screen."));

		if (distantSettings.Method == static_cast<uint>(DistantMethod::ShadowMap)) {
			const char* resolutions[] = { "1024", "2048", "4096" };
			int resolutionIndex = 1;
			for (int i = 0; i < IM_ARRAYSIZE(resolutions); i++)
				if (DistantMapResolutions[i] == distantSettings.MapResolution)
					resolutionIndex = i;
			if (ImGui::Combo(T(TKEY("distant_map_resolution"), "Shadow Map Resolution"), &resolutionIndex, resolutions, IM_ARRAYSIZE(resolutions)))
				distantSettings.MapResolution = DistantMapResolutions[resolutionIndex];
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("distant_map_resolution_tooltip"), "Resolution of each of the two distant shadow cascades. Higher values give sharper distant shadows and use more VRAM."));

			ImGui::SliderFloat(T(TKEY("distant_map_range"), "Distant Shadow Range"), &distantSettings.MapRange, DistantMinMapRange, DistantMaxMapRange, "%.0f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("distant_map_range_tooltip"), "How far from the camera, in game units, distant shadows reach. Longer ranges spread the same resolution over a larger area."));

			ImGui::SliderFloat(T(TKEY("distant_min_caster_size"), "Minimum Caster Size"), &distantSettings.MinCasterSize, 0.0f, DistantMaxCasterSize, "%.0f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("distant_min_caster_size_tooltip"), "Loaded objects with a smaller bounding radius are left out of the distant shadow map. LOD is always included."));

			ImGui::SliderFloat(T(TKEY("distant_update_interval"), "Update Interval"), &distantSettings.UpdateInterval, 0.0f, DistantMaxUpdateInterval, "%.0f ms", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("distant_update_interval_tooltip"), "Maximum time between redraws of each cascade, to pick up streamed LOD. Moving the camera or sun always redraws. 0 redraws only on movement."));

			ImGui::SliderFloat(T(TKEY("distant_filter_radius"), "Distant Shadow Softness"), &distantSettings.FilterRadius, 0.0f, DistantMaxFilterRadius, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("distant_filter_radius_tooltip"), "Filter radius in shadow map texels."));

			ImGui::SliderFloat(T(TKEY("distant_bias"), "Distant Shadow Bias"), &distantSettings.Bias, 0.0f, DistantMaxBias, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("distant_bias_tooltip"), "Offset toward the sun in shadow map texels. Raise it if distant surfaces show striped self-shadowing."));

			ImGui::Checkbox(T(TKEY("distant_tree_lod"), "Tree LOD Casters"), &distantSettings.TreeLOD);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("distant_tree_lod_tooltip"), "Lets billboard tree LOD cast distant shadows."));
		} else {
			ImGui::SliderInt(T(TKEY("distant_sample_count"), "Distant Sample Count"), (int*)&distantSettings.SampleCount, DistantMinSampleCount, DistantMaxSampleCount, "%d", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("distant_sample_count_tooltip"), "Depth samples per distant shadow ray. Higher values catch thinner occluders at a higher GPU cost. Only pixels beyond the shadow map distance pay this cost."));

			ImGui::SliderFloat(T(TKEY("distant_ray_length"), "Distant Shadow Length"), &distantSettings.MaxRayLength, DistantMinRayLength, DistantMaxRayLength, "%.0f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("distant_ray_length_tooltip"), "How far, in game units, each surface searches toward the sun for an occluder. Longer lengths let mountains cast longer shadows but spread the samples further apart."));

			ImGui::SliderFloat(T(TKEY("distant_thickness"), "Distant Occluder Thickness"), &distantSettings.Thickness, DistantMinThickness, DistantMaxThickness, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("%s", T(TKEY("distant_thickness_tooltip"), "Assumed depth of occluders relative to the distance searched. Higher values fill in mountain shadows more solidly; lower values stop thin foreground objects from casting long shadows."));
		}

		ImGui::SliderFloat(T(TKEY("distant_intensity"), "Distant Shadow Intensity"), &distantSettings.Intensity, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", T(TKEY("distant_intensity_tooltip"), "Strength of distant shadows. Lower values keep some sunlight in shadowed areas."));

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}
}

void ScreenSpaceShadows::InvalidateRaymarchShaders()
{
	if (raymarchCS) {
		raymarchCS->Release();
		raymarchCS = nullptr;
	}
}

void ScreenSpaceShadows::ClearShaderCache()
{
	InvalidateRaymarchShaders();
	if (distantTraceCS) {
		distantTraceCS->Release();
		distantTraceCS = nullptr;
	}
	if (distantResolveCS) {
		distantResolveCS->Release();
		distantResolveCS = nullptr;
	}
	if (distantShadowMapCS) {
		distantShadowMapCS->Release();
		distantShadowMapCS = nullptr;
	}
	distantShadowMap.ClearShaderCache();
}

uint ScreenSpaceShadows::GetScaledSampleCount()
{
	float2 renderSize = Util::ConvertToDynamic(float2{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight });

	// Scale sample count based on both dimensions relative to 1920x1080 reference
	float2 referenceRes = { 1920.0f, 1080.0f };
	float referenceArea = referenceRes.x * referenceRes.y;
	float currentArea = renderSize.x * renderSize.y;
	float areaScale = std::sqrt(currentArea / referenceArea);
	uint scaledSampleCount = static_cast<uint>(std::round(bendSettings.SampleCount * 60 * areaScale));

	// Quantize to steps of 8 to prevent frequent recompilation from small DRS oscillations
	scaledSampleCount = ((scaledSampleCount + 7u) / 8u) * 8u;
	scaledSampleCount = std::max(scaledSampleCount, 8u);

	return scaledSampleCount;
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeRaymarch()
{
	uint scaledSampleCount = GetScaledSampleCount();

	if (scaledSampleCount != lastCompiledSampleCount) {
		lastCompiledSampleCount = scaledSampleCount;
		InvalidateRaymarchShaders();
	}

	if (!raymarchCS) {
		auto sampleCount = std::format("{}", scaledSampleCount);
		std::vector<std::pair<const char*, const char*>> defines{ { "SAMPLE_COUNT", sampleCount.c_str() } };
		// TERRAIN_BLENDING flips DepthTexture's HLSL type from `Texture2D<unorm float>`
		// (R24_UNORM_X8_TYPELESS game depth) to `Texture2D<float>` (R32_FLOAT blendedDepth).
		if (globals::features::terrainBlending.loaded)
			defines.push_back({ "TERRAIN_BLENDING", "" });
		raymarchCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\RaymarchCS.hlsl", defines, "cs_5_0");
	}
	return raymarchCS;
}

void ScreenSpaceShadows::DrawShadows()
{
	ZoneScopedS(8);
	TracyD3D11Zone(globals::state->tracyCtx, "Screen Space Shadows");

	auto context = globals::d3d::context;

	auto accumulator = *globals::game::currentAccumulator.get();
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());

	auto& directionNi = dirLight->GetWorldDirection();
	float3 light = { directionNi.x, directionNi.y, directionNi.z };
	light.Normalize();
	float4 lightProjection = float4(-light.x, -light.y, -light.z, 0.0f);

	// Helper lambda to calculate light projection
	auto CalculateLightProjection = [&]() -> std::array<float, 4> {
		auto viewProjMat = globals::game::frameBufferCached.GetCameraViewProj().Transpose();
		auto projectedLight = DirectX::SimpleMath::Vector4::Transform(lightProjection, viewProjMat);
		return { projectedLight.x, projectedLight.y, projectedLight.z, projectedLight.w };
	};

	auto lightProjectionF = CalculateLightProjection();

	float2 renderSize = Util::ConvertToDynamic(float2{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight });
	int viewportSize[2] = { (int)renderSize.x, (int)renderSize.y };

	int minRenderBounds[2] = { 0, 0 };
	int maxRenderBounds[2] = { viewportSize[0], viewportSize[1] };

	// Setup common render state.
	// SSS always uses 24/32-bit depth, never the R16_UNORM half-precision path.
	// With TerrainBlending loaded the SRV is R32_FLOAT (blendedDepthTexture);
	// without it, the game's kPOST_ZPREPASS_COPY (R24_UNORM_X8_TYPELESS).
	// The shader's DepthTexture declaration is conditional on TERRAIN_BLENDING:
	// `<float>` for the R32_FLOAT path, `<unorm float>` for the R24_UNORM path.
	auto* depthSRV = Util::GetCurrentSceneDepthSRV(false);
	context->CSSetShaderResources(0, 1, &depthSRV);

	auto uav = screenSpaceShadowsTexture->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	context->CSSetSamplers(0, 1, &pointBorderSampler);

	auto buffer = raymarchCB->CB();
	context->CSSetConstantBuffers(1, 1, &buffer);

	auto viewport = globals::game::graphicsState;

	float2 dynamicRes = { viewport->GetRuntimeData().dynamicResolutionWidthRatio, viewport->GetRuntimeData().dynamicResolutionHeightRatio };

	// Shared dispatch logic
	auto Dispatch = [&](ID3D11ComputeShader* shader, const float* lightProj,
						float invTexSizeX, float invTexSizeY) {
		globals::profiler->BeginPass("ScreenSpaceShadows::RayMarch");

		if (globals::state->frameAnnotations) {
			globals::state->BeginPerfEvent("SSS - Ray March");
		}

		context->CSSetShader(shader, nullptr, 0);

		auto dispatchList = Bend::BuildDispatchList(const_cast<float*>(lightProj), viewportSize, minRenderBounds, maxRenderBounds);

		for (int i = 0; i < dispatchList.DispatchCount; i++) {
			auto dispatchData = dispatchList.Dispatch[i];

			{
				TracyD3D11Zone(globals::state->tracyCtx, "SSS - Dispatch CB");

				RaymarchCB data{};
				data.LightCoordinate[0] = dispatchList.LightCoordinate_Shader[0];
				data.LightCoordinate[1] = dispatchList.LightCoordinate_Shader[1];
				data.LightCoordinate[2] = dispatchList.LightCoordinate_Shader[2];
				data.LightCoordinate[3] = dispatchList.LightCoordinate_Shader[3];

				data.WaveOffset[0] = dispatchData.WaveOffset_Shader[0];
				data.WaveOffset[1] = dispatchData.WaveOffset_Shader[1];

				const bool reverseZ = globals::features::reverseZ.IsActive();
				data.FarDepthValue = reverseZ ? 0.0f : 1.0f;
				data.NearDepthValue = reverseZ ? 1.0f : 0.0f;

				data.DynamicRes = dynamicRes;

				data.InvDepthTextureSize[0] = invTexSizeX;
				data.InvDepthTextureSize[1] = invTexSizeY;

				data.settings = bendSettings;

				raymarchCB->Update(data);
			}

			{
				TracyD3D11Zone(globals::state->tracyCtx, "SSS - Dispatch Sweep");
				context->Dispatch(dispatchData.WaveCount[0], dispatchData.WaveCount[1], dispatchData.WaveCount[2]);
			}
		}

		if (globals::state->frameAnnotations) {
			globals::state->EndPerfEvent();
		}

		globals::profiler->EndPass();
	};

	float InvTexSizeX = 1.0f / (float)viewportSize[0];
	float InvTexSizeY = 1.0f / (float)viewportSize[1];

	Dispatch(GetComputeRaymarch(), lightProjectionF.data(), InvTexSizeX, InvTexSizeY);

	ID3D11ShaderResourceView* views[1]{ nullptr };
	context->CSSetShaderResources(0, 1, views);

	ID3D11UnorderedAccessView* uavs[1]{ nullptr };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11SamplerState* sampler = nullptr;
	context->CSSetSamplers(0, 1, &sampler);

	buffer = nullptr;
	context->CSSetConstantBuffers(1, 1, &buffer);
}

bool ScreenSpaceShadows::CompileDistantShadows()
{
	if (distantTraceCS && distantResolveCS && distantShadowMapCS)
		return true;

	std::vector<std::pair<const char*, const char*>> defines;
	if (globals::features::terrainBlending.loaded)
		defines.push_back({ "TERRAIN_BLENDING", "" });
	if (globals::features::terrainShadows.loaded)
		defines.push_back({ "TERRAIN_SHADOWS", "" });

	if (!distantTraceCS)
		distantTraceCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\DistantShadowsCS.hlsl", defines, "cs_5_0", "TraceCS");
	if (!distantResolveCS)
		distantResolveCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\DistantShadowsCS.hlsl", defines, "cs_5_0", "ResolveCS");
	if (!distantShadowMapCS)
		distantShadowMapCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\DistantShadowsCS.hlsl", defines, "cs_5_0", "ShadowMapCS");
	return distantTraceCS && distantResolveCS && distantShadowMapCS;
}

float ScreenSpaceShadows::GetShadowCascadeEndDistance()
{
	auto* smState = globals::game::smState;
	if (!smState)
		return 0.0f;

	auto* shadowSceneNode = smState->shadowSceneNode[0];
	if (!shadowSceneNode)
		return 0.0f;

	auto* sunShadowLight = shadowSceneNode->GetRuntimeData().sunShadowDirLight;
	if (!sunShadowLight)
		return 0.0f;

	auto& dirData = sunShadowLight->GetShadowDirectionalLightRuntimeData();
	return std::max(dirData.endSplitDistances[0], dirData.endSplitDistances[1]);
}

void ScreenSpaceShadows::DrawDistantShadows(bool a_hasContactShadows)
{
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Screen Space Shadows - Distant");

	const float cascadeEnd = GetShadowCascadeEndDistance();
	if (!std::isfinite(cascadeEnd) || cascadeEnd <= 0.0f)
		return;

	const bool useShadowMap = distantSettings.Method == static_cast<uint>(DistantMethod::ShadowMap);
	if ((!useShadowMap && !distantHalfTexture) || !CompileDistantShadows())
		return;

	float2 renderSize = Util::ConvertToDynamic(float2{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight });

	DistantShadowsCB data{};
	data.RenderSize = renderSize;
	data.InvRenderSize = { 1.0f / renderSize.x, 1.0f / renderSize.y };
	data.FadeLength = std::min(DistantFadeLength, cascadeEnd);
	data.StartDistance = cascadeEnd - data.FadeLength;
	data.MaxRayLength = distantSettings.MaxRayLength;
	data.Intensity = distantSettings.Intensity;
	data.ThicknessScale = distantSettings.Thickness;
	data.SampleCount = distantSettings.SampleCount;
	data.UseContactShadows = a_hasContactShadows;
	data.HalfSize[0] = ((uint)renderSize.x + 1u) >> 1;
	data.HalfSize[1] = ((uint)renderSize.y + 1u) >> 1;

	if (useShadowMap) {
		auto& terrainShadows = globals::features::terrainShadows;
		DistantShadowMap::Config config{};
		config.Resolution = distantSettings.MapResolution;
		config.Range = distantSettings.MapRange;
		config.MinCasterRadius = distantSettings.MinCasterSize;
		config.UpdateInterval = distantSettings.UpdateInterval;
		config.StartDistance = data.StartDistance;
		config.IncludeTreeLOD = distantSettings.TreeLOD;
		config.IncludeTerrain = !(terrainShadows.loaded && terrainShadows.settings.EnableTerrainShadow && terrainShadows.IsHeightMapReady());
		if (!distantShadowMap.Update(config))
			return;

		distantShadowMap.GetCascadeData(data.MapCascades);
		data.MapFilterRadius = distantSettings.FilterRadius;
		data.MapBiasTexels = distantSettings.Bias;
		data.MapInvResolution = 1.0f / static_cast<float>(distantSettings.MapResolution);
		data.MapBlendBand = DistantMapBlendBand;
	}

	auto context = globals::d3d::context;

	globals::profiler->BeginPass("ScreenSpaceShadows::Distant");
	if (globals::state->frameAnnotations)
		globals::state->BeginPerfEvent("SSS - Distant");

	if (a_hasContactShadows)
		context->CopyResource(contactShadowsCopyTexture->resource.get(), screenSpaceShadowsTexture->resource.get());

	distantShadowsCB->Update(data);

	ID3D11Buffer* buffers[1] = { distantShadowsCB->CB() };
	context->CSSetConstantBuffers(1, 1, buffers);
	ID3D11Buffer* sharedBuffers[2] = { globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
	context->CSSetConstantBuffers(5, 2, sharedBuffers);

	ID3D11SamplerState* samplers[2] = { globals::deferred->linearSampler, distantShadowMap.GetComparisonSampler() };
	context->CSSetSamplers(0, 2, samplers);

	ID3D11ShaderResourceView* depthSrv = Util::GetCurrentSceneDepthSRV(false);
	ID3D11UnorderedAccessView* nullUav = nullptr;

	if (useShadowMap) {
		ID3D11ShaderResourceView* srvs[4] = { depthSrv, contactShadowsCopyTexture->srv.get(), nullptr, distantShadowMap.GetSRV() };
		context->CSSetShaderResources(0, 4, srvs);

		ID3D11UnorderedAccessView* uav = screenSpaceShadowsTexture->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(distantShadowMapCS, nullptr, 0);
		context->Dispatch(((uint)renderSize.x + 7u) >> 3, ((uint)renderSize.y + 7u) >> 3, 1);
	} else {
		context->CSSetShaderResources(0, 1, &depthSrv);

		ID3D11UnorderedAccessView* uav = distantHalfTexture->uav.get();
		context->CSSetUnorderedAccessViews(1, 1, &uav, nullptr);
		context->CSSetShader(distantTraceCS, nullptr, 0);
		context->Dispatch((data.HalfSize[0] + 7u) >> 3, (data.HalfSize[1] + 7u) >> 3, 1);

		context->CSSetUnorderedAccessViews(1, 1, &nullUav, nullptr);

		ID3D11ShaderResourceView* srvs[3] = { depthSrv, contactShadowsCopyTexture->srv.get(), distantHalfTexture->srv.get() };
		context->CSSetShaderResources(0, 3, srvs);

		uav = screenSpaceShadowsTexture->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(distantResolveCS, nullptr, 0);
		context->Dispatch(((uint)renderSize.x + 7u) >> 3, ((uint)renderSize.y + 7u) >> 3, 1);
	}

	ID3D11ShaderResourceView* nullSrvs[4]{ nullptr, nullptr, nullptr, nullptr };
	context->CSSetShaderResources(0, 4, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	buffers[0] = nullptr;
	context->CSSetConstantBuffers(1, 1, buffers);
	ID3D11SamplerState* nullSamplers[2]{ nullptr, nullptr };
	context->CSSetSamplers(0, 2, nullSamplers);
	context->CSSetShader(nullptr, nullptr, 0);

	if (globals::state->frameAnnotations)
		globals::state->EndPerfEvent();
	globals::profiler->EndPass();
}

void ScreenSpaceShadows::Prepass()
{
	auto context = globals::d3d::context;

	float white[4] = { 1, 1, 1, 1 };
	context->ClearUnorderedAccessViewFloat(screenSpaceShadowsTexture->uav.get(), white);

	if (auto sky = globals::game::sky)
		if (sky->mode.get() == RE::Sky::Mode::kFull) {
			const bool hasContactShadows = bendSettings.Enable;
			if (hasContactShadows)
				DrawShadows();
			if (distantSettings.Enable && !globals::state->isMapMenuOpen)
				DrawDistantShadows(hasContactShadows);
		}

	auto view = screenSpaceShadowsTexture->srv.get();
	context->PSSetShaderResources(45, 1, &view);
}

void ScreenSpaceShadows::LoadSettings(json& o_json)
{
	bendSettings = o_json;

	distantSettings = {};
	if (o_json.contains(DistantSettingsKey) && o_json[DistantSettingsKey].is_object())
		distantSettings = o_json[DistantSettingsKey];

	const DistantSettings defaults{};
	auto sanitize = [](float value, float fallback, float lo, float hi) { return std::clamp(std::isfinite(value) ? value : fallback, lo, hi); };
	distantSettings.SampleCount = std::clamp(distantSettings.SampleCount, DistantMinSampleCount, DistantMaxSampleCount);
	distantSettings.MaxRayLength = sanitize(distantSettings.MaxRayLength, defaults.MaxRayLength, DistantMinRayLength, DistantMaxRayLength);
	distantSettings.Intensity = sanitize(distantSettings.Intensity, defaults.Intensity, 0.0f, 1.0f);
	distantSettings.Thickness = sanitize(distantSettings.Thickness, defaults.Thickness, DistantMinThickness, DistantMaxThickness);
	distantSettings.Method = std::min(distantSettings.Method, static_cast<uint>(DistantMethod::ScreenSpace));
	if (std::find(std::begin(DistantMapResolutions), std::end(DistantMapResolutions), distantSettings.MapResolution) == std::end(DistantMapResolutions))
		distantSettings.MapResolution = defaults.MapResolution;
	distantSettings.MapRange = sanitize(distantSettings.MapRange, defaults.MapRange, DistantMinMapRange, DistantMaxMapRange);
	distantSettings.MinCasterSize = sanitize(distantSettings.MinCasterSize, defaults.MinCasterSize, 0.0f, DistantMaxCasterSize);
	distantSettings.UpdateInterval = sanitize(distantSettings.UpdateInterval, defaults.UpdateInterval, 0.0f, DistantMaxUpdateInterval);
	distantSettings.FilterRadius = sanitize(distantSettings.FilterRadius, defaults.FilterRadius, 0.0f, DistantMaxFilterRadius);
	distantSettings.Bias = sanitize(distantSettings.Bias, defaults.Bias, 0.0f, DistantMaxBias);
}

void ScreenSpaceShadows::SaveSettings(json& o_json)
{
	o_json = bendSettings;
	o_json[DistantSettingsKey] = distantSettings;
}

void ScreenSpaceShadows::RestoreDefaultSettings()
{
	bendSettings = {};
	distantSettings = {};
}

bool ScreenSpaceShadows::HasShaderDefine(RE::BSShader::Type)
{
	return true;
}

void ScreenSpaceShadows::SetupResources()
{
	raymarchCB = new ConstantBuffer(ConstantBufferDesc<RaymarchCB>(), "SSS::RaymarchCB");
	distantShadowsCB = new ConstantBuffer(ConstantBufferDesc<DistantShadowsCB>(), "SSS::DistantShadowsCB");

	{
		auto device = globals::d3d::device;

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		const float farDepth = globals::features::reverseZ.IsActive() ? 0.0f : 1.0f;
		samplerDesc.BorderColor[0] = farDepth;
		samplerDesc.BorderColor[1] = farDepth;
		samplerDesc.BorderColor[2] = farDepth;
		samplerDesc.BorderColor[3] = farDepth;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &pointBorderSampler));
		Util::SetResourceName(pointBorderSampler, "SSS::PointBorderSampler");
	}

	{
		auto renderer = globals::game::renderer;
		auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

		shadowMask.texture->GetDesc(&texDesc);
		shadowMask.SRV->GetDesc(&srvDesc);

		// Store normal front-facing visibility and the reversed depth-offset
		// visibility used by back-facing SSS/transmission receivers.
		texDesc.Format = DXGI_FORMAT_R8G8_UNORM;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		srvDesc.Format = texDesc.Format;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		screenSpaceShadowsTexture = new Texture2D(texDesc, "SSS::ShadowTexture");
		screenSpaceShadowsTexture->CreateSRV(srvDesc);
		screenSpaceShadowsTexture->CreateUAV(uavDesc);

		D3D11_TEXTURE2D_DESC copyDesc = texDesc;
		copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		contactShadowsCopyTexture = new Texture2D(copyDesc, "SSS::ContactShadowsCopy");
		contactShadowsCopyTexture->CreateSRV(srvDesc);

		D3D11_TEXTURE2D_DESC halfDesc = texDesc;
		halfDesc.Width = (texDesc.Width + 1) / 2;
		halfDesc.Height = (texDesc.Height + 1) / 2;
		halfDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		D3D11_SHADER_RESOURCE_VIEW_DESC halfSrvDesc = srvDesc;
		halfSrvDesc.Format = halfDesc.Format;
		D3D11_UNORDERED_ACCESS_VIEW_DESC halfUavDesc = uavDesc;
		halfUavDesc.Format = halfDesc.Format;
		distantHalfTexture = new Texture2D(halfDesc, "SSS::DistantHalfOcclusion");
		distantHalfTexture->CreateSRV(halfSrvDesc);
		distantHalfTexture->CreateUAV(halfUavDesc);
	}
}
#undef I18N_KEY_PREFIX
