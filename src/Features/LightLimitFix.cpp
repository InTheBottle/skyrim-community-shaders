#include "LightLimitFix.h"
#include "Effects11.h"
#include "InverseSquareLighting.h"
#include "LinearLighting.h"

#include "I18n/I18n.h"
#include "Menu/ThemeManager.h"
#include "Shadercache.h"
#include "State.h"
#include "Utils/ExternalEmittance.h"
#include "Utils/MathUtils.h"

#include <numbers>

#define I18N_KEY_PREFIX "feature.light_limit_fix."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LightLimitFix::Settings,
	EnableParticleLights,
	EnableParticleLightsCulling,
	EnableLightsVisualisation,
	LightsVisualisationMode,
	EnableContactShadows,
	ContactShadowMaxSteps,
	ContactShadowMaxDistance,
	ContactShadowStride,
	ContactShadowThickness,
	ContactShadowDepthFade,
	ContactShadowStrength,
	EnableLocalShadows,
	LocalShadowSlots,
	LocalShadowResolution,
	LocalShadowSamples,
	LocalShadowFilterRadius,
	LocalShadowBiasScale)

static constexpr uint CLUSTER_MAX_LIGHTS = 128;

void LightLimitFix::DrawSettings()
{
	auto shaderCache = globals::shaderCache;

	ImGui::Checkbox(T(TKEY("enable_particle_lights"), "Enable Particle Lights"), &settings.EnableParticleLights);

	ImGui::Spacing();

	ImGui::SeparatorText(T(TKEY("local_shadows"), "Local Light Shadows"));

	ImGui::Checkbox(T(TKEY("enable_local_shadows"), "Enable Local Light Shadows"), &settings.EnableLocalShadows);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_local_shadows_tooltip"),
							  "Keeps a cache of the shadow maps the game renders for shadow-casting lights and rotates which lights the game renders each frame.\n"
							  "More than four lights can cast shadows at once at the same rendering cost as vanilla. Moving lights and lights with actors inside their radius are refreshed first."));
	}

	if (settings.EnableLocalShadows) {
		ImGui::SliderInt(T(TKEY("local_shadow_slots"), "Shadow Cache Slots"), (int*)&settings.LocalShadowSlots, MIN_LOCAL_SHADOW_SLOTS, MAX_LOCAL_SHADOW_SLOTS, "%d", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("local_shadow_slots_tooltip"), "Maximum number of lights that can hold a cached shadow map at the same time. Each slot costs video memory at the cache resolution."));
		}

		static const char* resolutionOptions[] = { "512", "1024", "2048" };
		int resolutionIndex = settings.LocalShadowResolution >= 2048 ? 2 : (settings.LocalShadowResolution >= 1024 ? 1 : 0);
		if (ImGui::Combo(T(TKEY("local_shadow_resolution"), "Shadow Cache Resolution"), &resolutionIndex, resolutionOptions, 3))
			settings.LocalShadowResolution = 512u << resolutionIndex;
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("local_shadow_resolution_tooltip"), "Resolution of each cached shadow map. It never exceeds the shadow map resolution set in the game INI."));
		}

		static const char* sampleOptions[] = { "1", "4", "8" };
		int sampleIndex = settings.LocalShadowSamples >= 8 ? 2 : (settings.LocalShadowSamples >= 4 ? 1 : 0);
		if (ImGui::Combo(T(TKEY("local_shadow_samples"), "Shadow Filter Samples"), &sampleIndex, sampleOptions, 3))
			settings.LocalShadowSamples = sampleIndex == 0 ? 1 : (sampleIndex == 1 ? 4 : 8);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("local_shadow_samples_tooltip"), "Filter taps per shadow lookup. More taps give softer edges at a higher cost per shadowed light."));
		}

		ImGui::SliderFloat(T(TKEY("local_shadow_filter_radius"), "Shadow Filter Radius"), &settings.LocalShadowFilterRadius, 0.5f, 4.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("local_shadow_filter_radius_tooltip"), "Softening radius in shadow map texels."));
		}

		ImGui::SliderFloat(T(TKEY("local_shadow_bias"), "Shadow Bias Scale"), &settings.LocalShadowBiasScale, 0.0f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("local_shadow_bias_tooltip"), "Scales the depth bias of every light. Raise it if surfaces show striped self-shadowing, lower it if shadows detach from their casters."));
		}
	}

	ImGui::SeparatorText(T(TKEY("contact_shadows"), "Contact Shadows"));

	ImGui::Checkbox(T(TKEY("enable_contact_shadows"), "Enable Contact Shadows"), &settings.EnableContactShadows);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("%s", T(TKEY("enable_contact_shadows_tooltip"),
							  "Short screen-space shadows traced toward every point light.\n"
							  "They add the contact detail shadow maps miss and give lights without a shadow map a shadow of their own."));
	}

	if (settings.EnableContactShadows) {
		ImGui::SliderInt(T(TKEY("contact_shadow_max_steps"), "Max Steps"), (int*)&settings.ContactShadowMaxSteps, 1, 32, "%d", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("contact_shadow_max_steps_tooltip"), "Samples per light for surfaces near the camera. Above four, rays that never approach an occluder are rejected early, so higher counts cost less than they look."));
		}

		ImGui::SliderFloat(T(TKEY("contact_shadow_max_distance"), "Max Distance"), &settings.ContactShadowMaxDistance, 64.0f, 8192.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("contact_shadow_max_distance_tooltip"), "Distance from the camera at which contact shadows fade out completely."));
		}

		ImGui::SliderFloat(T(TKEY("contact_shadow_stride"), "Stride"), &settings.ContactShadowStride, 0.5f, 16.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("contact_shadow_stride_tooltip"), "Distance between samples near the camera. Longer strides reach further but can skip thin occluders."));
		}

		ImGui::SliderFloat(T(TKEY("contact_shadow_thickness"), "Thickness"), &settings.ContactShadowThickness, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("contact_shadow_thickness_tooltip"), "How quickly a depth difference counts as an occluder. Higher values react to thinner objects."));
		}

		ImGui::SliderFloat(T(TKEY("contact_shadow_depth_fade"), "Depth Fade"), &settings.ContactShadowDepthFade, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("contact_shadow_depth_fade_tooltip"), "How quickly occluders far behind the ray stop counting. Lower values shadow across larger depth gaps."));
		}

		ImGui::SliderFloat(T(TKEY("contact_shadow_strength"), "Strength"), &settings.ContactShadowStrength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	}

	ImGui::Spacing();

	if (ImGui::TreeNodeEx(T(TKEY("statistics"), "Statistics"), ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text(std::format("Clustered Light Count : {}", lightCount).c_str());
		ImGui::Text(std::format("Shadow Casters : {} tracked, {} cached, {} rendered this frame", localShadowStatTracked, localShadowStatCached, localShadowStatRendered).c_str());

		ImGui::TreePop();
	}

	ImGui::SeparatorText(T(TKEY("debug"), "Debug"));

	if (ImGui::TreeNode(T(TKEY("light_limit_vis"), "Light Limit Visualization"))) {
		ImGui::Checkbox(T(TKEY("enable_lights_vis"), "Enable Lights Visualisation"), &settings.EnableLightsVisualisation);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("%s", T(TKEY("enable_lights_vis_tooltip"), "Enables visualization of the light limit\n"));
		}

		{
			static const char* comboOptions[] = { "Light Limit", "Strict Lights Count", "Clustered Lights Count", "Shadow Mask" };
			ImGui::Combo(T(TKEY("lights_vis_mode"), "Lights Visualisation Mode"), (int*)&settings.LightsVisualisationMode, comboOptions, 4);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("%s", T(TKEY("lights_vis_mode_tooltip"),
									  " - Visualise the light limit. Red when the \"strict\" light limit is reached (portal-strict lights).\n"
									  " - Visualise the number of strict lights.\n"
									  " - Visualise the number of clustered lights.\n"
									  " - Visualize the Shadow Mask.\n"));
			}
		}
		currentEnableLightsVisualisation = settings.EnableLightsVisualisation;
		if (previousEnableLightsVisualisation != currentEnableLightsVisualisation) {
			globals::state->SetDefines(settings.EnableLightsVisualisation ? "LLFDEBUG" : "");
			shaderCache->Clear(RE::BSShader::Type::Lighting);
			previousEnableLightsVisualisation = currentEnableLightsVisualisation;
		}

		ImGui::TreePop();
	}
}

void LightLimitFix::DrawOverlay()
{
	if (!settings.EnableLightsVisualisation)
		return;
	const float pos = ThemeManager::Constants::OVERLAY_WINDOW_POSITION * Util::GetUIScale();
	ImGui::SetNextWindowPos(ImVec2(pos, pos), ImGuiCond_Always);
	ImGui::Begin("##LLFDebug", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
	Util::Text::Error("%s", T(TKEY("debug_feature_enabled"), "DEBUG FEATURE - LIGHT LIMIT VISUALISATION ENABLED"));
	ImGui::End();
}

LightLimitFix::PerFrame LightLimitFix::GetCommonBufferData()
{
	PerFrame perFrame{};
	perFrame.EnableLightsVisualisation = settings.EnableLightsVisualisation;
	perFrame.LightsVisualisationMode = settings.LightsVisualisationMode;
	std::copy(clusterSize, clusterSize + 3, perFrame.ClusterSize);

	auto sanitize = [](float a_value, float a_min, float a_max) {
		return std::isfinite(a_value) ? std::clamp(a_value, a_min, a_max) : a_min;
	};

	perFrame.EnableContactShadows = settings.EnableContactShadows;
	perFrame.ContactShadowMaxSteps = std::clamp<uint>(settings.ContactShadowMaxSteps, 1u, 32u);
	perFrame.ContactShadowMaxDistance = sanitize(settings.ContactShadowMaxDistance, 64.0f, 8192.0f);
	perFrame.ContactShadowStride = sanitize(settings.ContactShadowStride, 0.5f, 16.0f);
	perFrame.ContactShadowThickness = sanitize(settings.ContactShadowThickness, 0.0f, 1.0f);
	perFrame.ContactShadowDepthFade = sanitize(settings.ContactShadowDepthFade, 0.0f, 1.0f);
	perFrame.ContactShadowStrength = sanitize(settings.ContactShadowStrength, 0.0f, 1.0f);

	const float texelSize = 1.0f / static_cast<float>(std::max(localShadowCacheResolution, 1u));
	perFrame.EnableLocalShadows = settings.EnableLocalShadows && localShadowCache != nullptr;
	perFrame.LocalShadowSamples = settings.LocalShadowSamples >= 8 ? 8 : (settings.LocalShadowSamples >= 4 ? 4 : 1);
	perFrame.LocalShadowFilterRadius = sanitize(settings.LocalShadowFilterRadius, 0.0f, 8.0f) * texelSize;
	perFrame.LocalShadowTexelSize = texelSize;
	return perFrame;
}

void LightLimitFix::SetupResources()
{
	float2 screenSize{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight };
	clusterSize[0] = ((uint)screenSize.x + 63) / 64;
	clusterSize[1] = ((uint)screenSize.y + 63) / 64;
	clusterSize[2] = 32;
	uint clusterCount = clusterSize[0] * clusterSize[1] * clusterSize[2];

	{
		std::vector<std::pair<const char*, const char*>> clusterDefines;
		clusterBuildingCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\LightLimitFix\\ClusterBuildingCS.hlsl", clusterDefines, "cs_5_0");
		clusterCullingCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\LightLimitFix\\ClusterCullingCS.hlsl", clusterDefines, "cs_5_0");

		lightBuildingCB = new ConstantBuffer(ConstantBufferDesc<LightBuildingCB>());
		lightCullingCB = new ConstantBuffer(ConstantBufferDesc<LightCullingCB>());

		localShadowCopyCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\LightLimitFix\\LocalShadowCopyCS.hlsl", clusterDefines, "cs_5_0");
		localShadowCopyCB = new ConstantBuffer(ConstantBufferDesc<LocalShadowCopyCB>());
	}

	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DEFAULT;
		sbDesc.CPUAccessFlags = 0;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.FirstElement = 0;
		uavDesc.Buffer.Flags = 0;

		std::uint32_t numElements = clusterCount;

		sbDesc.StructureByteStride = sizeof(ClusterAABB);
		sbDesc.ByteWidth = sizeof(ClusterAABB) * numElements;
		clusters = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::Clusters");
		srvDesc.Buffer.NumElements = numElements;
		clusters->CreateSRV(srvDesc);
		uavDesc.Buffer.NumElements = numElements;
		clusters->CreateUAV(uavDesc);

		numElements = 1;
		sbDesc.StructureByteStride = sizeof(uint32_t);
		sbDesc.ByteWidth = sizeof(uint32_t) * numElements;
		lightIndexCounter = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::LightIndexCounter");
		srvDesc.Buffer.NumElements = numElements;
		lightIndexCounter->CreateSRV(srvDesc);
		uavDesc.Buffer.NumElements = numElements;
		lightIndexCounter->CreateUAV(uavDesc);

		numElements = clusterCount * CLUSTER_MAX_LIGHTS;
		sbDesc.StructureByteStride = sizeof(uint32_t);
		sbDesc.ByteWidth = sizeof(uint32_t) * numElements;
		lightIndexList = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::LightIndexList");
		srvDesc.Buffer.NumElements = numElements;
		lightIndexList->CreateSRV(srvDesc);
		uavDesc.Buffer.NumElements = numElements;
		lightIndexList->CreateUAV(uavDesc);

		numElements = clusterCount;
		sbDesc.StructureByteStride = sizeof(LightGrid);
		sbDesc.ByteWidth = sizeof(LightGrid) * numElements;
		lightGrid = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::LightGrid");
		srvDesc.Buffer.NumElements = numElements;
		lightGrid->CreateSRV(srvDesc);
		uavDesc.Buffer.NumElements = numElements;
		lightGrid->CreateUAV(uavDesc);
	}

	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DYNAMIC;
		sbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sbDesc.StructureByteStride = sizeof(LightData);
		sbDesc.ByteWidth = sizeof(LightData) * MAX_LIGHTS;
		lights = eastl::make_unique<Buffer>(sbDesc, nullptr, "LLF::Lights");

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;
		srvDesc.Buffer.NumElements = MAX_LIGHTS;
		lights->CreateSRV(srvDesc);
	}

	{
		strictLightDataCB = new ConstantBuffer(ConstantBufferDesc<StrictLightDataCB>());
	}
}

void LightLimitFix::SaveSettings(json& o_json)
{
	o_json = settings;
}

void LightLimitFix::LoadSettings(json& o_json)
{
	settings = o_json;
}

void LightLimitFix::RestoreDefaultSettings()
{
	settings = {};
}

RE::NiNode* GetParentRoomNode(RE::NiAVObject* object)
{
	if (object == nullptr) {
		return nullptr;
	}

	static const auto* roomRtti = REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSMultiBoundRoom }.get();
	static const auto* portalRtti = REL::Relocation<const RE::NiRTTI*>{ RE::NiRTTI_BSPortalSharedNode }.get();

	const auto* rtti = object->GetRTTI();
	if (rtti == roomRtti || rtti == portalRtti) {
		return static_cast<RE::NiNode*>(object);
	}

	return GetParentRoomNode(object->parent);
}

void LightLimitFix::BSLightingShader_SetupGeometry_Before(RE::BSRenderPass* a_pass)
{
	auto shaderCache = globals::shaderCache;

	if (!shaderCache->IsEnabled())
		return;

	strictLightDataTemp.NumStrictLights = 0;
	strictLightDataTemp.ShadowBitMask = 0;
	strictLightDataTemp.FirstPerson = 0;

	strictLightDataTemp.RoomIndex = -1;
	if (!roomNodes.empty()) {
		if (RE::NiNode* roomNode = GetParentRoomNode(a_pass->geometry)) {
			if (auto it = roomNodes.find(roomNode); it != roomNodes.cend()) {
				strictLightDataTemp.RoomIndex = it->second;
			}
		}
	}
}

void LightLimitFix::BSLightingShader_SetupGeometry_GeometrySetupConstantPointLights(RE::BSRenderPass* a_pass)
{
	auto& isl = globals::features::inverseSquareLighting;

	auto accumulator = *globals::game::currentAccumulator.get();
	bool inWorld = accumulator->GetRuntimeData().activeShadowSceneNode == globals::game::smState->shadowSceneNode[0];

	// The first-person pass rebases the camera's posAdjust, so shadow-space projections
	// cannot reconstruct absolute world space from it; carry the true world eye instead.
	const bool firstPerson = inWorld && (Util::GetEyePosition() - eyePositionCached).SqrLength() > 1.0f;
	strictLightDataTemp.FirstPerson = firstPerson ? 1u : 0u;
	strictLightDataTemp.WorldEyePosition = { eyePositionCached.x, eyePositionCached.y, eyePositionCached.z, 0.0f };

	strictLightDataTemp.NumStrictLights = inWorld ? 0 : (a_pass->numLights - 1);

	uint32_t writeIdx = 0;
	for (uint32_t i = 0; i < strictLightDataTemp.NumStrictLights; i++) {
		auto bsLight = a_pass->sceneLights[i + 1];
		if (!bsLight)
			continue;
		auto niLight = bsLight->light.get();
		if (!niLight)
			continue;

		auto& runtimeData = niLight->GetLightRuntimeData();

		LightData light{};
		light.color = { runtimeData.diffuse.red, runtimeData.diffuse.green, runtimeData.diffuse.blue };
		light.lightFlags = std::bit_cast<LightFlags>(runtimeData.ambient.red);

		if (isl.loaded) {
			isl.ProcessLight(light, bsLight, niLight);
		} else {
			light.radius = runtimeData.radius.x;
			// light.color *= runtimeData.fade;
			light.fade = runtimeData.fade;
		}

		light.fade *= bsLight->lodDimmer;

		auto& effects11 = globals::features::effects11;
		if (inWorld && effects11.enableEffect)
			effects11.OverridePointLightColor(light.color);

		SetLightPosition(light, niLight->world.translate, inWorld);

		if (i < a_pass->numShadowLights) {
			auto* shadowLight = static_cast<RE::BSShadowLight*>(bsLight);
			light.lightFlags.set(LightFlags::ShadowCaster);
			TryAssignShadowMask(light, shadowLight);
		}

		strictLightDataTemp.StrictLights[writeIdx++] = light;
	}
	strictLightDataTemp.NumStrictLights = writeIdx;

	for (uint32_t i = 0; i < a_pass->numShadowLights; i++) {
		auto bsLight = a_pass->sceneLights[i + 1];
		if (!bsLight)
			continue;
		auto* shadowLight = static_cast<RE::BSShadowLight*>(bsLight);
		const auto maskIndex = GetShadowMaskIndex(shadowLight);
		if (maskIndex < SHADOW_MASK_CHANNEL_COUNT)
			strictLightDataTemp.ShadowBitMask |= (1u << maskIndex);
	}
}

void LightLimitFix::BSLightingShader_SetupGeometry_After(RE::BSRenderPass*)
{
	auto shaderCache = globals::shaderCache;
	auto context = globals::d3d::context;
	auto smState = globals::game::smState;

	if (!shaderCache->IsEnabled())
		return;

	auto accumulator = *globals::game::currentAccumulator.get();

	auto shadowSceneNode = smState->shadowSceneNode[0];

	const auto isEmpty = strictLightDataTemp.NumStrictLights == 0;
	const bool isWorld = accumulator->GetRuntimeData().activeShadowSceneNode == shadowSceneNode;
	const auto roomIndex = strictLightDataTemp.RoomIndex;
	const auto shadowBitMask = strictLightDataTemp.ShadowBitMask;
	const bool isFirstPerson = strictLightDataTemp.FirstPerson != 0;

	if (!isEmpty || (isEmpty && !wasEmpty) || isWorld != wasWorld || previousRoomIndex != roomIndex || shadowBitMask != previousShadowBitMask || isFirstPerson != wasFirstPerson) {
		strictLightDataCB->Update(strictLightDataTemp);
		wasEmpty = isEmpty;
		wasWorld = isWorld;
		previousRoomIndex = roomIndex;
		previousShadowBitMask = shadowBitMask;
		wasFirstPerson = isFirstPerson;
	}

	if (frameChecker.IsNewFrame()) {
		ID3D11Buffer* buffer = { strictLightDataCB->CB() };
		context->PSSetConstantBuffers(3, 1, &buffer);
	}
}

void LightLimitFix::SetLightPosition(LightLimitFix::LightData& a_light, RE::NiPoint3 a_initialPosition, bool a_cached)
{
	RE::NiPoint3 eyePosition;

	if (a_cached) {
		eyePosition = eyePositionCached;
	} else {
		eyePosition = Util::GetEyePosition();
	}

	auto worldPos = a_initialPosition - eyePosition;
	a_light.positionWS.data.x = worldPos.x;
	a_light.positionWS.data.y = worldPos.y;
	a_light.positionWS.data.z = worldPos.z;
}

void LightLimitFix::Prepass()
{
	auto context = globals::d3d::context;

	auto state = globals::state;

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "LightLimitFix Prepass");
	state->BeginPerfEvent("LightLimitFix Prepass");
	UpdateLights();

	ID3D11ShaderResourceView* views[3]{};
	views[0] = lights->srv.get();
	views[1] = lightIndexList->srv.get();
	views[2] = lightGrid->srv.get();
	context->PSSetShaderResources(35, ARRAYSIZE(views), views);

	if (settings.EnableLocalShadows && localShadowCache)
		BindLocalShadowResources();

	state->EndPerfEvent();
}

bool LightLimitFix::IsValidLight(RE::BSLight* a_light)
{
	return a_light && !a_light->light->GetFlags().any(RE::NiAVObject::Flag::kHidden);
}

bool LightLimitFix::IsGlobalLight(RE::BSLight* a_light)
{
	return !(a_light->portalStrict || !a_light->portalGraph);
}

void LightLimitFix::PostPostLoad()
{
	particleLightConfigs.Load();
	Hooks::Install();
}

void LightLimitFix::DataLoaded()
{
	auto iMagicLightMaxCount = globals::game::gameSettingCollection->GetSetting("iMagicLightMaxCount");
	iMagicLightMaxCount->data.i = MAXINT32;
	logger::info("[LLF] Unlocked magic light limit");
}

void LightLimitFix::ClearShaderCache()
{
	if (clusterBuildingCS) {
		clusterBuildingCS->Release();
		clusterBuildingCS = nullptr;
	}
	if (clusterCullingCS) {
		clusterCullingCS->Release();
		clusterCullingCS = nullptr;
	}
	if (localShadowCopyCS) {
		localShadowCopyCS->Release();
		localShadowCopyCS = nullptr;
	}
	std::vector<std::pair<const char*, const char*>> clusterDefines;
	clusterBuildingCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\LightLimitFix\\ClusterBuildingCS.hlsl", clusterDefines, "cs_5_0");
	clusterCullingCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\LightLimitFix\\ClusterCullingCS.hlsl", clusterDefines, "cs_5_0");
	localShadowCopyCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\LightLimitFix\\LocalShadowCopyCS.hlsl", clusterDefines, "cs_5_0");
}

void LightLimitFix::UpdateLights()
{
	auto smState = globals::game::smState;
	auto& isl = globals::features::inverseSquareLighting;

	auto shadowSceneNode = smState->shadowSceneNode[0];

	// Cache camera position from the FrameBuffer snapshot; shadowState::posAdjust can be stale in first-person

	{
		auto eyePosition = globals::game::frameBufferCached.GetCameraPosAdjust();
		eyePositionCached = { eyePosition.x, eyePosition.y, eyePosition.z };
	}

	eastl::vector<LightData> lightsData{};
	lightsData.reserve(MAX_LIGHTS);

	// Process point lights

	roomNodes.clear();

	auto addRoom = [&](RE::NiNode* node, LightData& light) {
		uint8_t roomIndex = 0;
		if (auto it = roomNodes.find(node); it == roomNodes.cend()) {
			roomIndex = static_cast<uint8_t>(roomNodes.size());
			roomNodes.insert_or_assign(node, roomIndex);
		} else {
			roomIndex = it->second;
		}
		light.roomFlags.SetBit(roomIndex, 1);
	};

	auto addLight = [&](const RE::NiPointer<RE::BSLight>& e) {
		if (auto bsLight = e.get()) {
			if (auto niLight = bsLight->light.get()) {
				if (IsValidLight(bsLight)) {
					auto& runtimeData = niLight->GetLightRuntimeData();

					LightData light{};
					light.color = { runtimeData.diffuse.red, runtimeData.diffuse.green, runtimeData.diffuse.blue };
					light.lightFlags = std::bit_cast<LightFlags>(runtimeData.ambient.red);

					if (isl.loaded) {
						isl.ProcessLight(light, bsLight, niLight);
					} else {
						light.radius = runtimeData.radius.x;
						// light.color *= runtimeData.fade;
						light.fade = runtimeData.fade;
					}

					light.fade *= bsLight->lodDimmer;

					auto& effects11 = globals::features::effects11;
					if (effects11.enableEffect)
						effects11.OverridePointLightColor(light.color);

					if (!IsGlobalLight(bsLight)) {
						// List of BSMultiBoundRooms affected by a light
						for (const auto& roomPtr : bsLight->rooms) {
							addRoom(roomPtr, light);
						}
						// List of BSPortals affected by a light
						for (const auto& portalPtr : bsLight->portals) {
							addRoom(portalPtr->portalSharedNode.get(), light);
						}
						light.lightFlags.set(LightFlags::PortalStrict);
					}

					if (bsLight->IsShadowLight()) {
						auto* shadowLight = static_cast<RE::BSShadowLight*>(bsLight);
						light.lightFlags.set(LightFlags::ShadowCaster);
						if (settings.EnableLocalShadows && localShadowCache) {
							if (auto* caster = FindLocalShadowCaster(shadowLight); caster && caster->slice >= 0 && caster->lastRenderedFrame != 0) {
								light.localShadowIndex = static_cast<uint32_t>(caster->slice);
								light.lightFlags.set(LightFlags::LocalShadow);
							}
						} else {
							TryAssignShadowMask(light, shadowLight);
						}
					}

					SetLightPosition(light, niLight->world.translate);

					if ((light.color.x + light.color.y + light.color.z) * light.fade > 1e-4 && light.radius > 1e-4) {
						lightsData.push_back(light);
					}
				}
			}
		}
	};

	for (auto& e : shadowSceneNode->GetRuntimeData().activeLights) {
		addLight(e);
	}
	for (auto& e : shadowSceneNode->GetRuntimeData().activeShadowLights) {
		addLight(e);
	}

	AddParticleLightsToBuffer(lightsData);

	auto context = globals::d3d::context;

	lightCount = std::min((uint)lightsData.size(), MAX_LIGHTS);

	D3D11_MAPPED_SUBRESOURCE mapped;
	DX::ThrowIfFailed(context->Map(lights->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
	size_t bytes = sizeof(LightData) * lightCount;
	memcpy_s(mapped.pData, bytes, lightsData.data(), bytes);
	context->Unmap(lights->resource.get(), 0);

	UpdateStructure();
}

void LightLimitFix::UpdateStructure()
{
	auto context = globals::d3d::context;

	lightsNear = *globals::game::cameraNear;
	lightsFar = *globals::game::cameraFar;

	auto renderSize = Util::ConvertToDynamic(float2{ (float)globals::game::graphicsState->screenWidth, (float)globals::game::graphicsState->screenHeight });
	clusterSize[0] = ((uint)renderSize.x + 63) / 64;
	clusterSize[1] = ((uint)renderSize.y + 63) / 64;
	clusterSize[2] = 32;

	{
		LightBuildingCB updateData{};
		updateData.LightsNear = lightsNear;
		updateData.LightsFar = lightsFar;
		std::copy(clusterSize, clusterSize + 3, updateData.ClusterSize);

		lightBuildingCB->Update(updateData);

		ID3D11Buffer* buffer = lightBuildingCB->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);

		ID3D11UnorderedAccessView* clusters_uav = clusters->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &clusters_uav, nullptr);

		context->CSSetShader(clusterBuildingCS, nullptr, 0);
		globals::profiler->BeginPass("LightLimitFix::ClusterBuild");
		context->Dispatch(clusterSize[0], clusterSize[1], clusterSize[2]);
		globals::profiler->EndPass();

		ID3D11UnorderedAccessView* null_uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
	}

	{
		LightCullingCB updateData{};
		updateData.LightCount = lightCount;
		std::copy(clusterSize, clusterSize + 3, updateData.ClusterSize);

		lightCullingCB->Update(updateData);

		UINT counterReset[4] = { 0, 0, 0, 0 };
		context->ClearUnorderedAccessViewUint(lightIndexCounter->uav.get(), counterReset);

		ID3D11Buffer* buffer = lightCullingCB->CB();
		context->CSSetConstantBuffers(0, 1, &buffer);

		ID3D11ShaderResourceView* srvs[] = { clusters->srv.get(), lights->srv.get() };
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11UnorderedAccessView* uavs[] = { lightIndexCounter->uav.get(), lightIndexList->uav.get(), lightGrid->uav.get() };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		context->CSSetShader(clusterCullingCS, nullptr, 0);
		globals::profiler->BeginPass("LightLimitFix::ClusterCull");
		context->Dispatch((clusterSize[0] + 15) / 16, (clusterSize[1] + 15) / 16, (clusterSize[2] + 3) / 4);
		globals::profiler->EndPass();
	}

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11Buffer* null_buffer = nullptr;
	context->CSSetConstantBuffers(0, 1, &null_buffer);

	ID3D11ShaderResourceView* null_srvs[2] = { nullptr };
	context->CSSetShaderResources(0, 2, null_srvs);

	ID3D11UnorderedAccessView* null_uavs[3] = { nullptr };
	context->CSSetUnorderedAccessViews(0, 3, null_uavs, nullptr);
}

void LightLimitFix::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	auto& singleton = globals::features::lightLimitFix;
	singleton.BSLightingShader_SetupGeometry_Before(Pass);
	func(This, Pass, RenderFlags);
	singleton.BSLightingShader_SetupGeometry_After(Pass);
}

void LightLimitFix::Hooks::BSEffectShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	func(This, Pass, RenderFlags);
	ExternalEmittance::UpdatePermutation(Pass);
	auto& singleton = globals::features::lightLimitFix;
	singleton.BSLightingShader_SetupGeometry_Before(Pass);
	singleton.BSLightingShader_SetupGeometry_After(Pass);
};

void LightLimitFix::Hooks::BSWaterShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	func(This, Pass, RenderFlags);
	auto& singleton = globals::features::lightLimitFix;
	singleton.BSLightingShader_SetupGeometry_Before(Pass);
	singleton.BSLightingShader_SetupGeometry_After(Pass);
};

namespace
{
	struct VertexColor
	{
		std::uint8_t data[4];
	};

	bool TryGetAlphaWeightedVertexColor(const std::uint8_t* a_rawVertexData, std::uint32_t a_vertexSize, std::uint32_t a_colorOffset, std::uint32_t a_vertexCount, VertexColor& a_outVertexColor)
	{
		if (!a_rawVertexData || a_vertexSize < sizeof(VertexColor) || a_vertexCount == 0)
			return false;
		if (a_colorOffset > (a_vertexSize - sizeof(VertexColor)))
			return false;

		float weightedR = 0.f, weightedG = 0.f, weightedB = 0.f;
		float totalAlpha = 0.f;
		std::uint8_t maxAlpha = 0;

		for (std::uint32_t v = 0; v < a_vertexCount; ++v) {
			const auto byteOffset = static_cast<std::size_t>(a_vertexSize) * v + a_colorOffset;
			const auto* vertex = reinterpret_cast<const VertexColor*>(a_rawVertexData + byteOffset);
			float alpha = vertex->data[3];
			weightedR += vertex->data[0] * alpha;
			weightedG += vertex->data[1] * alpha;
			weightedB += vertex->data[2] * alpha;
			totalAlpha += alpha;
			if (vertex->data[3] > maxAlpha)
				maxAlpha = vertex->data[3];
		}

		if (totalAlpha == 0.f)
			return false;

		a_outVertexColor.data[0] = static_cast<std::uint8_t>(std::min(weightedR / totalAlpha, 255.f));
		a_outVertexColor.data[1] = static_cast<std::uint8_t>(std::min(weightedG / totalAlpha, 255.f));
		a_outVertexColor.data[2] = static_cast<std::uint8_t>(std::min(weightedB / totalAlpha, 255.f));
		a_outVertexColor.data[3] = maxAlpha;
		return true;
	}

	RE::NiColorA BuildEffectMaterialEmissiveTint(RE::BSEffectShaderMaterial* a_material, RE::BSEffectShaderProperty* a_shaderProperty)
	{
		RE::NiColorA tint{
			a_material->baseColor.red * a_material->baseColorScale,
			a_material->baseColor.green * a_material->baseColorScale,
			a_material->baseColor.blue * a_material->baseColorScale,
			1.0f
		};
		if (auto emittance = a_shaderProperty->emittanceColor) {
			tint.red *= emittance->red;
			tint.green *= emittance->green;
			tint.blue *= emittance->blue;
		}
		return tint;
	}

	std::optional<std::string> GetLowercaseStem(const char* a_path)
	{
		std::filesystem::path p(a_path);
		auto stem = p.stem().string();
		if (stem.empty())
			return std::nullopt;
		std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return stem;
	}
}

void LightLimitFix::ParticleLightConfigStore::Load()
{
	configs.clear();

	configs["default"] = ParticleLightConfig{};
	logger::info("[LLF] Particle lights config conflict policy: first-win");

	if (std::filesystem::exists("Data\\ParticleLights")) {
		logger::info("[LLF] Loading particle lights configs");

		auto iniFiles = clib_util::distribution::get_configs("Data\\ParticleLights", "", ".ini");
		std::sort(iniFiles.begin(), iniFiles.end());

		if (iniFiles.empty()) {
			logger::warn("[LLF] No .ini files in Data\\ParticleLights");
			return;
		}

		logger::info("[LLF] {} matching inis found", iniFiles.size());

		for (auto& path : iniFiles) {
			logger::info("[LLF] loading ini: {}", path);

			CSimpleIniA ini;
			ini.SetUnicode();
			ini.SetMultiKey();

			if (const auto rc = ini.LoadFile(path.c_str()); rc < 0) {
				logger::error("\t\t[LLF] couldn't read INI");
				continue;
			}

			ParticleLightConfig data{};
			data.cull = ini.GetBoolValue("Light", "Cull", false);

			const auto filename = GetLowercaseStem(path.c_str());
			if (!filename)
				continue;

			if (configs.contains(*filename)) {
				logger::warn("[LLF] Duplicate config '{}'; keeping first, ignoring {}", *filename, path);
				continue;
			}

			logger::debug("[LLF] Inserting {}", *filename);
			configs.emplace(*filename, data);
		}
	}
}

LightLimitFix::VertexColorCacheEntry LightLimitFix::GetParticleLightConfig(RE::BSRenderPass* a_pass)
{
	if (!a_pass || !a_pass->geometry || !a_pass->shaderProperty)
		return {};

	if (!settings.EnableParticleLights)
		return {};

	auto shaderProperty = a_pass->shaderProperty->GetRTTI() == globals::rtti::BSEffectShaderPropertyRTTI.get() ?
	                          static_cast<RE::BSEffectShaderProperty*>(a_pass->shaderProperty) :
	                          nullptr;
	if (!shaderProperty || shaderProperty->lightData)
		return {};

	auto material = static_cast<RE::BSEffectShaderMaterial*>(shaderProperty->GetMaterial());
	if (!material)
		return {};

	auto parent = a_pass->geometry->parent;
	if (!parent || parent->GetRTTI() != globals::rtti::NiBillboardNodeRTTI.get())
		return {};

	auto* node = a_pass->geometry;

	{
		std::shared_lock lock{ particleLightsMutex };
		auto it = vertexColorCache.find(node);
		if (it != vertexColorCache.end()) {
			return it->second;
		}
	}

	auto cacheInvalid = [&](RE::BSGeometry* a_node) {
		VertexColorCacheEntry invalid{};
		invalid.valid = false;
		std::unique_lock lock{ particleLightsMutex };
		vertexColorCache[a_node] = invalid;
		return invalid;
	};

	if (material->sourceTexturePath.empty())
		return cacheInvalid(node);

	auto textureName = GetLowercaseStem(material->sourceTexturePath.c_str());
	if (!textureName)
		return cacheInvalid(node);

	auto& configs = particleLightConfigs.configs;
	auto configIt = configs.find(*textureName);
	if (configIt == configs.end())
		return cacheInvalid(node);

	ParticleLightConfig config = configIt->second;

	VertexColorCacheEntry entry{};
	entry.valid = true;
	entry.applyEffectMaterialTint = true;
	entry.config = config;
	entry.baseColor = { 1, 1, 1, 1 };
	bool hasVertexTint = false;
	if (auto rendererData = a_pass->geometry->GetGeometryRuntimeData().rendererData) {
		if (auto triShape = a_pass->geometry->AsTriShape()) {
			const std::uint32_t vertexSize = rendererData->vertexDesc.GetSize();
			if (rendererData->vertexDesc.HasFlag(RE::BSGraphics::Vertex::Flags::VF_COLORS) && rendererData->rawVertexData && vertexSize > 0u) {
				const std::uint32_t offset = rendererData->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::Attribute::VA_COLOR);
				const std::uint32_t vertexCount = static_cast<std::uint32_t>(triShape->GetTrishapeRuntimeData().vertexCount);

				VertexColor weightedVC{};
				if (TryGetAlphaWeightedVertexColor(rendererData->rawVertexData, vertexSize, offset, vertexCount, weightedVC)) {
					entry.baseColor.red *= weightedVC.data[0] / 255.f;
					entry.baseColor.green *= weightedVC.data[1] / 255.f;
					entry.baseColor.blue *= weightedVC.data[2] / 255.f;
					hasVertexTint = true;
					if (shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kVertexAlpha))
						entry.baseColor.alpha *= weightedVC.data[3] / 255.f;
				}
			}
		}
	}

	if (!hasVertexTint) {
		entry.baseColor = BuildEffectMaterialEmissiveTint(material, shaderProperty);
		entry.applyEffectMaterialTint = false;
	}

	{
		std::unique_lock lock{ particleLightsMutex };
		vertexColorCache[node] = entry;
	}
	return entry;
}

bool LightLimitFix::QueueParticleLight(RE::BSRenderPass* a_pass, VertexColorCacheEntry& a_reference)
{
	if (!a_pass || !a_pass->geometry || !a_pass->shaderProperty)
		return false;

	auto shaderProperty = a_pass->shaderProperty->GetRTTI() == globals::rtti::BSEffectShaderPropertyRTTI.get() ?
	                          static_cast<RE::BSEffectShaderProperty*>(a_pass->shaderProperty) :
	                          nullptr;
	if (!shaderProperty)
		return false;

	auto material = static_cast<RE::BSEffectShaderMaterial*>(shaderProperty->GetMaterial());
	if (!material)
		return false;

	RE::NiColorA color = a_reference.baseColor;
	if (a_reference.applyEffectMaterialTint) {
		color.red *= material->baseColor.red * material->baseColorScale;
		color.green *= material->baseColor.green * material->baseColorScale;
		color.blue *= material->baseColor.blue * material->baseColorScale;

		if (auto emittance = shaderProperty->emittanceColor) {
			color.red *= emittance->red;
			color.green *= emittance->green;
			color.blue *= emittance->blue;
		}
	}

	ResolvedParticleLight resolved;
	resolved.position = a_pass->geometry->world.translate;
	resolved.color = color;
	resolved.radius = a_pass->geometry->worldBound.radius;

	std::unique_lock lock{ particleLightsMutex };
	queuedParticleLights.push_back(resolved);

	return true;
}

bool LightLimitFix::CheckParticleLights(RE::BSRenderPass* a_pass, uint32_t)
{
	if (!a_pass || !a_pass->geometry || !a_pass->shaderProperty)
		return true;

	using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
	if (!a_pass->shaderProperty->flags.all(Flag::kSoftEffect, Flag::kZBufferTest))
		return true;

	auto* alphaProperty = static_cast<RE::NiAlphaProperty*>(a_pass->geometry->GetGeometryRuntimeData().alphaProperty.get());
	if (!alphaProperty || alphaProperty->alphaFlags != 4109)
		return true;

	auto reference = GetParticleLightConfig(a_pass);
	if (reference.valid) {
		if (QueueParticleLight(a_pass, reference))
			return !(settings.EnableParticleLightsCulling && reference.config.cull);
	}
	return true;
}

void LightLimitFix::AddParticleLightsToBuffer(eastl::vector<LightData>& a_lightsData)
{
	if (!settings.EnableParticleLights)
		return;

	static float& lightFadeStart = *reinterpret_cast<float*>(REL::RelocationID(527668, 414582).address());
	static float& lightFadeEnd = *reinterpret_cast<float*>(REL::RelocationID(527669, 414583).address());

	std::unique_lock lock{ particleLightsMutex };

	currentParticleLights.clear();
	std::swap(currentParticleLights, queuedParticleLights);

	auto& effects11 = globals::features::effects11;

	for (const auto& pl : currentParticleLights) {
		if (a_lightsData.size() >= MAX_LIGHTS)
			break;

		LightData light{};
		constexpr float invPI = 1.f / std::numbers::pi_v<float>;
		light.color.x = pl.color.red * invPI;
		light.color.y = pl.color.green * invPI;
		light.color.z = pl.color.blue * invPI;
		light.color *= pl.color.alpha;

		if (effects11.enableEffect)
			effects11.OverridePointLightColor(light.color);

		light.radius = pl.radius * 0.5f;

		light.lightFlags.set(LightFlags::Simple);
		SetLightPosition(light, pl.position);

		float distance = (light.positionWS.data.x * light.positionWS.data.x) +
		                 (light.positionWS.data.y * light.positionWS.data.y) +
		                 (light.positionWS.data.z * light.positionWS.data.z) -
		                 (light.radius * light.radius);

		float dimmer = 0.0f;
		if (distance < lightFadeStart || lightFadeEnd == 0.0f || lightFadeEnd <= lightFadeStart)
			dimmer = 1.0f;
		else if (distance <= lightFadeEnd)
			dimmer = 1.0f - ((distance - lightFadeStart) / (lightFadeEnd - lightFadeStart));

		light.fade = dimmer;
		if ((light.color.x + light.color.y + light.color.z) * light.fade > 1e-4 && light.radius > 1e-4) {
			light.invRadius = 1.f / light.radius;
			a_lightsData.push_back(light);
		}
	}
}

template <int N>
void LightLimitFix::Hooks::BSBatchRenderer_RenderPassImmediately<N>::thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
{
	if (globals::features::lightLimitFix.CheckParticleLights(a_pass, a_technique))
		func(a_pass, a_technique, a_alphaTest, a_renderFlags);
}

void LightLimitFix::Hooks::BSGeometry_Destroy::thunk(RE::BSGeometry* This)
{
	{
		std::unique_lock lock{ globals::features::lightLimitFix.particleLightsMutex };
		globals::features::lightLimitFix.vertexColorCache.erase(This);
	}
	func(This);
}

namespace
{
	struct LocalShadowRenderInfo
	{
		uint32_t engineSlice = UINT32_MAX;
		uint32_t maskIndex = 255;
		int32_t renderTarget = -1;
		float biasScale = 0.0f;
		DirectX::XMFLOAT4X4 lightTransform{};
	};

	bool ReadLocalShadowRenderInfo(RE::BSShadowLight* a_light, LocalShadowRenderInfo& a_info)
	{
		auto& runtimeData = a_light->GetRuntimeData();
		if (runtimeData.shadowmapDescriptors.empty())
			return false;
		auto& descriptor = runtimeData.shadowmapDescriptors[0];
		a_info.engineSlice = descriptor.shadowmapIndex;
		a_info.renderTarget = static_cast<int32_t>(descriptor.renderTarget);
		a_info.maskIndex = runtimeData.maskIndex;
		a_info.biasScale = runtimeData.shadowBiasScale;
		static_assert(sizeof(a_info.lightTransform) == sizeof(descriptor.lightTransform));
		std::memcpy(&a_info.lightTransform, &descriptor.lightTransform, sizeof(a_info.lightTransform));
		return true;
	}

	// shadowLightsAccum is a slot-indexed accumulator: an omni light occupies shadowMapCount
	// consecutive entries, and slots past the live count can hold freed pointers.
	bool IsPlausibleShadowLightPtr(std::uintptr_t a_raw) noexcept
	{
		return a_raw >= 0x10000ull && a_raw < 0x0000800000000000ull && (a_raw & 0x7) == 0;
	}

	template <typename Fn>
	void ForEachAccumulatedShadowLight(const RE::BSTArray<RE::BSShadowLight*>& a_accum, Fn&& a_fn)
	{
		const uint32_t count = static_cast<uint32_t>(a_accum.size());
		uint32_t index = 0;
		while (index < count) {
			RE::BSShadowLight* light = a_accum[index];
			if (!IsPlausibleShadowLightPtr(reinterpret_cast<std::uintptr_t>(light)))
				break;
			a_fn(light);
			const uint32_t step = light->shadowMapCount;
			if (step == 0)
				break;
			const uint64_t next = static_cast<uint64_t>(index) + step;
			if (next >= count)
				break;
			index = static_cast<uint32_t>(next);
		}
	}

	uint64_t ComputeLocalShadowContentHash(RE::BSShadowLight* a_light, RE::NiLight* a_niLight, float a_posStep, float a_radiusAnchor, uint32_t& a_skinnedOut)
	{
		a_skinnedOut = 0;
		uint64_t hash = 0x9e3779b97f4a7c15ull;
		hash = Util::HashCombineFloat(hash, Util::QuantizeFloat(a_radiusAnchor, 1.0f));

		const auto& translate = a_niLight->world.translate;
		hash = Util::HashCombineFloat(hash, Util::QuantizeFloat(translate.x, a_posStep));
		hash = Util::HashCombineFloat(hash, Util::QuantizeFloat(translate.y, a_posStep));
		hash = Util::HashCombineFloat(hash, Util::QuantizeFloat(translate.z, a_posStep));

		const auto& rotate = a_niLight->world.rotate;
		for (uint32_t row = 0; row < 3; row++)
			for (uint32_t column = 0; column < 3; column++)
				hash = Util::HashCombineFloat(hash, Util::QuantizeFloat(rotate.entry[row][column], 0.01f));

		for (auto& geometryPtr : a_light->geomList) {
			auto* geometry = geometryPtr.get();
			if (!geometry)
				continue;
			if (geometry->GetGeometryRuntimeData().skinInstance)
				a_skinnedOut++;
			const auto raw = reinterpret_cast<std::uintptr_t>(geometry);
			hash = Util::HashCombine(hash, static_cast<uint32_t>(raw));
			hash = Util::HashCombine(hash, static_cast<uint32_t>(raw >> 32));
			const auto& bound = geometry->worldBound;
			hash = Util::HashCombineFloat(hash, Util::QuantizeFloat(bound.center.x, a_posStep));
			hash = Util::HashCombineFloat(hash, Util::QuantizeFloat(bound.center.y, a_posStep));
			hash = Util::HashCombineFloat(hash, Util::QuantizeFloat(bound.center.z, a_posStep));
			hash = Util::HashCombineFloat(hash, Util::QuantizeFloat(bound.radius, 1.0f));
		}

		return hash;
	}
}

uint32_t LightLimitFix::GetShadowMaskIndex(RE::BSShadowLight* a_shadowLight)
{
	return a_shadowLight ? a_shadowLight->GetRuntimeData().maskIndex : 255u;
}

void LightLimitFix::TryAssignShadowMask(LightData& a_light, RE::BSShadowLight* a_shadowLight)
{
	const auto maskIndex = GetShadowMaskIndex(a_shadowLight);
	if (maskIndex < SHADOW_MASK_CHANNEL_COUNT) {
		a_light.shadowMaskIndex = maskIndex;
		a_light.lightFlags.set(LightFlags::Shadow);
	}
}

LightLimitFix::LocalShadowCaster* LightLimitFix::FindLocalShadowCaster(RE::BSShadowLight* a_light)
{
	if (auto it = localShadowCasterLookup.find(a_light); it != localShadowCasterLookup.end() && it->second < localShadowCasters.size())
		return &localShadowCasters[it->second];
	return nullptr;
}

void LightLimitFix::ScheduleLocalShadowCasters()
{
	localShadowAllowed.clear();
	localShadowSelecting = false;

	if (!loaded || !settings.EnableLocalShadows || REL::Module::IsVR())
		return;

	auto smState = globals::game::smState;
	if (!smState)
		return;
	auto shadowSceneNode = smState->shadowSceneNode[0];
	if (!shadowSceneNode)
		return;

	ZoneScoped;

	localShadowFrame++;
	const uint32_t frame = localShadowFrame;

	if (frame == 1) {
		auto eyePosition = globals::game::frameBufferCached.GetCameraPosAdjust();
		localShadowCameraPosition = { eyePosition.x, eyePosition.y, eyePosition.z };
	}

	localShadowActorPositions.clear();
	if (auto player = RE::PlayerCharacter::GetSingleton())
		localShadowActorPositions.push_back(player->GetPosition());
	if (auto processLists = RE::ProcessLists::GetSingleton()) {
		for (auto& handle : processLists->highActorHandles) {
			if (auto actor = handle.get())
				localShadowActorPositions.push_back(actor->GetPosition());
		}
	}

	for (auto& entry : shadowSceneNode->GetRuntimeData().activeShadowLights) {
		auto light = entry.get();
		if (!light)
			continue;
		auto niLight = light->light.get();
		if (!niLight)
			continue;

		LocalShadowCaster* caster = FindLocalShadowCaster(light);
		if (!caster) {
			localShadowCasterLookup[light] = static_cast<uint32_t>(localShadowCasters.size());
			caster = &localShadowCasters.emplace_back();
			caster->light = light;
		}

		caster->lastSeenFrame = frame;
		caster->position = niLight->world.translate;
		caster->radius = niLight->GetLightRuntimeData().radius.x;
		caster->hidden = niLight->GetFlags().any(RE::NiAVObject::Flag::kHidden);
		caster->radiusAnchor = caster->radiusAnchor < 0.0f ?
		                           caster->radius :
		                           caster->radiusAnchor + 0.15f * (caster->radius - caster->radiusAnchor);

		caster->dynamic = false;
		const float radiusSquared = caster->radius * caster->radius;
		for (const auto& actorPosition : localShadowActorPositions) {
			if (actorPosition.GetSquaredDistance(caster->position) < radiusSquared) {
				caster->dynamic = true;
				break;
			}
		}

		const float posStep = std::max(caster->radius / static_cast<float>(std::max(localShadowCacheResolution, 1u)), 1.0f);
		const uint32_t geomCount = static_cast<uint32_t>(light->geomList.size());
		if (caster->cachedGeomFrame == 0 || geomCount != caster->cachedGeomCount ||
			frame - caster->cachedGeomFrame >= LOCAL_SHADOW_GEOM_REHASH_INTERVAL) {
			caster->cachedGeomHash = ComputeLocalShadowContentHash(light, niLight, posStep, caster->radiusAnchor, caster->skinnedCasters);
			caster->cachedGeomFrame = frame;
			caster->cachedGeomCount = geomCount;
		}

		uint64_t contentHash = caster->cachedGeomHash;
		if (caster->dynamic) {
			const float actorStep = std::clamp(posStep, 1.0f, 8.0f);
			for (const auto& actorPosition : localShadowActorPositions) {
				if (actorPosition.GetSquaredDistance(caster->position) >= radiusSquared)
					continue;
				contentHash = Util::HashCombineFloat(contentHash, Util::QuantizeFloat(actorPosition.x, actorStep));
				contentHash = Util::HashCombineFloat(contentHash, Util::QuantizeFloat(actorPosition.y, actorStep));
				contentHash = Util::HashCombineFloat(contentHash, Util::QuantizeFloat(actorPosition.z, actorStep));
			}
		}
		caster->contentHash = contentHash;
	}

	for (size_t i = 0; i < localShadowCasters.size();) {
		auto& caster = localShadowCasters[i];
		if (caster.lastSeenFrame == frame) {
			i++;
			continue;
		}
		if (caster.slice >= 0 && static_cast<size_t>(caster.slice) < localShadowSliceOwner.size())
			localShadowSliceOwner[caster.slice] = nullptr;
		localShadowCasterLookup.erase(caster.light);
		if (i + 1 < localShadowCasters.size()) {
			caster = std::move(localShadowCasters.back());
			localShadowCasterLookup[caster.light] = static_cast<uint32_t>(i);
		}
		localShadowCasters.pop_back();
	}

	localShadowStatTracked = static_cast<uint32_t>(localShadowCasters.size());

	uint32_t freeSlices = 0;
	uint32_t oldestSliceAge = 0;
	for (auto owner : localShadowSliceOwner) {
		auto* ownerCaster = owner ? FindLocalShadowCaster(owner) : nullptr;
		if (!ownerCaster) {
			freeSlices++;
			continue;
		}
		oldestSliceAge = std::max(oldestSliceAge, frame - ownerCaster->lastRenderedFrame);
	}
	const bool admitNewCasters = localShadowSliceOwner.empty() || freeSlices > 0 || oldestSliceAge >= LOCAL_SHADOW_EVICT_AGE;

	static eastl::vector<uint32_t> order;
	order.clear();
	bool needsSweep = false;
	for (uint32_t i = 0; i < localShadowCasters.size(); i++) {
		auto& caster = localShadowCasters[i];
		caster.score = -1.0f;
		if (caster.hidden || caster.radius <= 0.0f)
			continue;
		if (caster.lastEvaluatedFrame == 0 || frame - caster.lastEvaluatedFrame > LOCAL_SHADOW_SWEEP_INTERVAL)
			needsSweep = true;
		if (caster.lastEligibleFrame == 0 || frame - caster.lastEligibleFrame > LOCAL_SHADOW_CAMERA_HOLD_FRAMES)
			continue;
		if (frame < caster.rejectUntilFrame)
			continue;
		if (caster.slice < 0 && !admitNewCasters)
			continue;

		const bool everRendered = caster.slice >= 0 && caster.lastRenderedFrame != 0;
		const bool dirty = !everRendered || caster.skinnedCasters > 0 ||
		                   caster.contentHash != caster.renderedContentHash;
		if (!dirty && frame - caster.lastRenderedFrame < LOCAL_SHADOW_CLEAN_REFRESH_FRAMES)
			continue;

		const float staleness = caster.lastRenderedFrame == 0 ? 1000.0f : static_cast<float>(frame - caster.lastRenderedFrame);
		const float distance = caster.position.GetDistance(localShadowCameraPosition);
		const float importance = caster.radius / std::max(distance, caster.radius);
		const float moveThreshold = std::max(12.0f, caster.radius * 0.02f);
		const bool moved = everRendered &&
		                   caster.position.GetSquaredDistance(caster.renderedPosition) > moveThreshold * moveThreshold;

		caster.score = staleness * (0.25f + importance) + (caster.dynamic ? 8.0f : 0.0f) +
		               (moved ? 100000.0f : 0.0f) + (everRendered ? 0.0f : 1000000.0f);
		order.push_back(i);
	}

	size_t engineCapacity = localShadowSunActive ? ENGINE_SHADOW_SLOTS - 1 : ENGINE_SHADOW_SLOTS;
	if (needsSweep && engineCapacity > 1)
		engineCapacity--;
	const size_t allowedCount = std::min<size_t>(order.size(), engineCapacity);
	std::partial_sort(order.begin(), order.begin() + allowedCount, order.end(), [&](uint32_t a_lhs, uint32_t a_rhs) {
		return localShadowCasters[a_lhs].score > localShadowCasters[a_rhs].score;
	});
	for (size_t i = 0; i < allowedCount; i++)
		localShadowAllowed.push_back(localShadowCasters[order[i]].light);

	localShadowSelecting = true;
}

bool LightLimitFix::FilterLocalShadowCaster(RE::BSShadowLight* a_light, const RE::NiCamera* a_camera, bool a_result)
{
	if (!localShadowSelecting || !a_light)
		return a_result;

	// UpdateCamera's shadow-LOD sub-test zeroes lodDimmer for lights past the (much shorter)
	// shadow distance. Rotation runs it on far more lights than vanilla, and UpdateLights
	// multiplies fade by lodDimmer, so leaving it zeroed renders the light black.
	if (a_light->lodDimmer == 0.0f)
		a_light->lodDimmer = 1.0f;

	if (a_camera)
		localShadowCameraPosition = a_camera->world.translate;

	auto* caster = FindLocalShadowCaster(a_light);
	if (!caster)
		return a_result;

	caster->lastEvaluatedFrame = localShadowFrame;
	if (a_result)
		caster->lastEligibleFrame = localShadowFrame;

	if (!a_result || localShadowAllowed.empty())
		return a_result;

	for (auto allowed : localShadowAllowed) {
		if (allowed == a_light)
			return true;
	}
	return false;
}

void LightLimitFix::ReleaseLocalShadowResources()
{
	localShadowCache = nullptr;
	localShadowBuffer = nullptr;
	localShadowCacheSlots = 0;
	localShadowCacheResolution = 0;
	localShadowEngineResolution = 0;
	localShadowSliceOwner.clear();
	for (auto& caster : localShadowCasters) {
		caster.slice = -1;
		caster.lastRenderedFrame = 0;
		caster.assignedFrame = 0;
	}
}

void LightLimitFix::EnsureLocalShadowResources(ID3D11Texture2D* a_engineShadowMaps)
{
	D3D11_TEXTURE2D_DESC engineDesc{};
	a_engineShadowMaps->GetDesc(&engineDesc);

	const uint32_t engineResolution = std::max(engineDesc.Width, 1u);
	uint32_t cacheResolution = std::min(std::max(settings.LocalShadowResolution, 128u), engineResolution);
	if (engineResolution % cacheResolution != 0)
		cacheResolution = engineResolution;
	const uint32_t slots = std::clamp(settings.LocalShadowSlots, MIN_LOCAL_SHADOW_SLOTS, MAX_LOCAL_SHADOW_SLOTS);

	if (localShadowCache && localShadowBuffer && slots == localShadowCacheSlots && cacheResolution == localShadowCacheResolution && engineResolution == localShadowEngineResolution)
		return;

	ReleaseLocalShadowResources();

	auto device = globals::d3d::device;

	DXGI_FORMAT cacheFormat = DXGI_FORMAT_R16_UNORM;
	UINT formatSupport = 0;
	if (FAILED(device->CheckFormatSupport(cacheFormat, &formatSupport)) || !(formatSupport & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW))
		cacheFormat = DXGI_FORMAT_R32_FLOAT;

	D3D11_TEXTURE2D_DESC texDesc{};
	texDesc.Width = cacheResolution;
	texDesc.Height = cacheResolution;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = slots;
	texDesc.Format = cacheFormat;
	texDesc.SampleDesc.Count = 1;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	localShadowCache = eastl::make_unique<Texture2D>(texDesc, "LightLimitFix::LocalShadowCache");

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = cacheFormat;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
	srvDesc.Texture2DArray.MostDetailedMip = 0;
	srvDesc.Texture2DArray.MipLevels = 1;
	srvDesc.Texture2DArray.FirstArraySlice = 0;
	srvDesc.Texture2DArray.ArraySize = slots;
	localShadowCache->CreateSRV(srvDesc);

	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = cacheFormat;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
	uavDesc.Texture2DArray.MipSlice = 0;
	uavDesc.Texture2DArray.FirstArraySlice = 0;
	uavDesc.Texture2DArray.ArraySize = slots;
	localShadowCache->CreateUAV(uavDesc);

	D3D11_BUFFER_DESC bufferDesc{};
	bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
	bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
	bufferDesc.StructureByteStride = sizeof(LocalShadowData);
	bufferDesc.ByteWidth = slots * sizeof(LocalShadowData);

	D3D11_SHADER_RESOURCE_VIEW_DESC bufferSrvDesc{};
	bufferSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufferSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
	bufferSrvDesc.Buffer.FirstElement = 0;
	bufferSrvDesc.Buffer.NumElements = slots;

	localShadowBuffer = eastl::make_unique<Buffer>(bufferDesc, nullptr, "LightLimitFix::LocalShadowData");
	localShadowBuffer->CreateSRV(bufferSrvDesc);

	localShadowSliceOwner.assign(slots, nullptr);
	localShadowCacheSlots = slots;
	localShadowCacheResolution = cacheResolution;
	localShadowEngineResolution = engineResolution;

	logger::info("[LLF] Local shadow cache: {} slots at {}x{} ({}), engine shadow maps {}x{}", slots, cacheResolution, cacheResolution,
		cacheFormat == DXGI_FORMAT_R16_UNORM ? "R16_UNORM" : "R32_FLOAT", engineResolution, engineResolution);
}

int32_t LightLimitFix::AcquireLocalShadowSlice(RE::BSShadowLight* a_light, uint32_t a_frame)
{
	int32_t evictSlice = -1;
	uint32_t evictFrame = UINT32_MAX;
	for (size_t slice = 0; slice < localShadowSliceOwner.size(); slice++) {
		auto owner = localShadowSliceOwner[slice];
		if (!owner) {
			localShadowSliceOwner[slice] = a_light;
			return static_cast<int32_t>(slice);
		}
		auto* ownerCaster = FindLocalShadowCaster(owner);
		if (!ownerCaster) {
			localShadowSliceOwner[slice] = a_light;
			return static_cast<int32_t>(slice);
		}
		if (ownerCaster->lastRenderedFrame == a_frame)
			continue;
		if (ownerCaster->lastRenderedFrame < evictFrame) {
			evictFrame = ownerCaster->lastRenderedFrame;
			evictSlice = static_cast<int32_t>(slice);
		}
	}

	if (evictSlice < 0)
		return -1;

	if (auto* evicted = FindLocalShadowCaster(localShadowSliceOwner[evictSlice])) {
		evicted->slice = -1;
		evicted->lastRenderedFrame = 0;
		evicted->assignedFrame = 0;
	}
	localShadowSliceOwner[evictSlice] = a_light;
	return evictSlice;
}

void LightLimitFix::CopyLocalShadowMaps()
{
	localShadowStatRendered = 0;
	localShadowStatCached = 0;

	auto smState = globals::game::smState;
	auto renderer = globals::game::renderer;
	if (!smState || !renderer)
		return;
	auto shadowSceneNode = smState->shadowSceneNode[0];
	if (!shadowSceneNode)
		return;

	auto& depthStencil = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS];
	if (!depthStencil.texture || !depthStencil.depthSRV)
		return;

	EnsureLocalShadowResources(depthStencil.texture);
	if (!localShadowCache || !localShadowBuffer || !localShadowCopyCS || !localShadowCopyCB)
		return;

	auto context = globals::d3d::context;
	const uint32_t frame = localShadowFrame;
	auto& runtimeData = shadowSceneNode->GetRuntimeData();
	auto* sunLight = static_cast<RE::BSShadowLight*>(runtimeData.sunShadowDirLight);
	const uint32_t scale = std::max(localShadowEngineResolution / localShadowCacheResolution, 1u);
	const uint32_t groups = (localShadowCacheResolution + 7) / 8;

	bool computeBound = false;
	bool sunSeen = false;
	static bool loggedMapping = false;
	ForEachAccumulatedShadowLight(runtimeData.shadowLightsAccum, [&](RE::BSShadowLight* light) {
		if (light == sunLight) {
			sunSeen = true;
			return;
		}

		auto* caster = FindLocalShadowCaster(light);
		if (!caster || caster->lastRenderedFrame == frame)
			return;

		LocalShadowRenderInfo info{};
		if (!ReadLocalShadowRenderInfo(light, info))
			return;

		uint32_t engineSlice = info.engineSlice < ENGINE_SHADOW_MAP_SLICES ? info.engineSlice : info.maskIndex;
		if (engineSlice >= ENGINE_SHADOW_MAP_SLICES) {
			static uint32_t warnedFrame = 0;
			if (frame - warnedFrame > 600) {
				warnedFrame = frame;
				logger::debug("[LLF] Shadow caster without a usable engine slice (shadowmapIndex {}, maskIndex {}, renderTarget {})", info.engineSlice, info.maskIndex, info.renderTarget);
			}
			return;
		}
		if (!loggedMapping) {
			loggedMapping = true;
			logger::info("[LLF] Local shadow slice mapping: shadowmapIndex {}, maskIndex {}, renderTarget {}, parabolic {}, shadowMapCount {}", info.engineSlice, info.maskIndex, info.renderTarget, light->GetIsParabolicLight(), light->shadowMapCount);
		}

		if (caster->slice < 0) {
			caster->slice = AcquireLocalShadowSlice(light, frame);
			if (caster->slice < 0)
				return;
			caster->assignedFrame = frame;
		}

		if (!computeBound) {
			ID3D11ShaderResourceView* srv = depthStencil.depthSRV;
			ID3D11UnorderedAccessView* uav = localShadowCache->uav.get();
			ID3D11Buffer* buffer = localShadowCopyCB->CB();
			context->CSSetShaderResources(0, 1, &srv);
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetConstantBuffers(0, 1, &buffer);
			context->CSSetShader(localShadowCopyCS, nullptr, 0);
			computeBound = true;
		}

		LocalShadowCopyCB copyData{ engineSlice, static_cast<uint32_t>(caster->slice), scale, localShadowCacheResolution };
		localShadowCopyCB->Update(copyData);
		context->Dispatch(groups, groups, 1);

		DirectX::XMMATRIX projection = DirectX::XMLoadFloat4x4(&info.lightTransform);
		DirectX::XMStoreFloat4x4(&caster->shadowProj, projection);

		uint32_t type = LOCAL_SHADOW_TYPE_SPOT;
		float spotFalloff = 2.0f;
		if (light->GetIsParabolicLight()) {
			type = light->shadowMapCount == 2 ? LOCAL_SHADOW_TYPE_OMNI : LOCAL_SHADOW_TYPE_HEMISPHERE;
		} else if (light->GetIsFrustumLight()) {
			spotFalloff = static_cast<RE::BSShadowFrustumLight*>(light)->GetShadowFrustumLightRuntimeData().falloff;
			if (!std::isfinite(spotFalloff) || spotFalloff <= 0.0f)
				spotFalloff = 2.0f;
		}

		const float biasTexelScale = static_cast<float>(scale);
		caster->shadowParams = { static_cast<float>(type), caster->radius, info.biasScale * 0.00025f * biasTexelScale * std::clamp(settings.LocalShadowBiasScale, 0.0f, 4.0f), 1.0f };
		caster->shadowParams2 = { spotFalloff, 0.0f, 0.0f, 0.0f };
		caster->lastRenderedFrame = frame;
		caster->renderedPosition = caster->position;
		caster->renderedContentHash = caster->contentHash;
		caster->rejectStreak = 0;
		localShadowStatRendered++;
	});

	localShadowSunActive = sunSeen || globals::state->HasDirectionalShadows();

	const uint32_t engineCapacity = localShadowSunActive ? ENGINE_SHADOW_SLOTS - 1 : ENGINE_SHADOW_SLOTS;
	if (localShadowStatRendered < engineCapacity) {
		for (auto* allowed : localShadowAllowed) {
			auto* caster = FindLocalShadowCaster(allowed);
			if (!caster || caster->lastRenderedFrame == frame)
				continue;
			caster->rejectStreak = std::min(caster->rejectStreak + 1, 4u);
			caster->rejectUntilFrame = frame + std::min(15u << caster->rejectStreak, LOCAL_SHADOW_REJECT_MAX_FRAMES);
		}
	}

	if (computeBound) {
		ID3D11ShaderResourceView* nullSrv = nullptr;
		ID3D11UnorderedAccessView* nullUav = nullptr;
		ID3D11Buffer* nullBuffer = nullptr;
		context->CSSetShaderResources(0, 1, &nullSrv);
		context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		context->CSSetConstantBuffers(0, 1, &nullBuffer);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	localShadowUpload.assign(localShadowCacheSlots, LocalShadowData{});
	for (auto& caster : localShadowCasters) {
		if (caster.slice < 0 || caster.lastRenderedFrame == 0 || static_cast<uint32_t>(caster.slice) >= localShadowCacheSlots)
			continue;
		LocalShadowData data{};
		data.ShadowProj = caster.shadowProj;
		data.Params = caster.shadowParams;
		data.Params2 = caster.shadowParams2;
		const bool spot = static_cast<uint32_t>(caster.shadowParams.x) == LOCAL_SHADOW_TYPE_SPOT;
		data.Params.w = spot ? 1.0f : std::min(1.0f, static_cast<float>(frame - caster.assignedFrame + 1) / static_cast<float>(LOCAL_SHADOW_FADE_FRAMES));
		localShadowUpload[caster.slice] = data;
		localShadowStatCached++;
	}

	D3D11_MAPPED_SUBRESOURCE mapped{};
	DX::ThrowIfFailed(context->Map(localShadowBuffer->resource.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
	std::memcpy(mapped.pData, localShadowUpload.data(), localShadowUpload.size() * sizeof(LocalShadowData));
	context->Unmap(localShadowBuffer->resource.get(), 0);
}

void LightLimitFix::BindLocalShadowResources()
{
	if (!localShadowCache || !localShadowBuffer)
		return;
	ID3D11ShaderResourceView* views[2] = { localShadowBuffer->srv.get(), localShadowCache->srv.get() };
	globals::d3d::context->PSSetShaderResources(102, ARRAYSIZE(views), views);
}

void LightLimitFix::EarlyPrepass()
{
	if (!settings.EnableLocalShadows || REL::Module::IsVR()) {
		if (localShadowCache)
			ReleaseLocalShadowResources();
		return;
	}

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "LightLimitFix Local Shadows");
	globals::state->BeginPerfEvent("LightLimitFix Local Shadows");
	CopyLocalShadowMaps();
	BindLocalShadowResources();
	globals::state->EndPerfEvent();
}

void LightLimitFix::Hooks::CalculateActiveShadowCasterLights::thunk()
{
	auto& lightLimitFix = globals::features::lightLimitFix;
	lightLimitFix.ScheduleLocalShadowCasters();
	func();
	lightLimitFix.localShadowSelecting = false;
}

bool LightLimitFix::Hooks::BSShadowParabolicLight_UpdateCamera::thunk(RE::BSShadowLight* This, const RE::NiCamera* a_camera)
{
	const bool result = func(This, a_camera);
	return globals::features::lightLimitFix.FilterLocalShadowCaster(This, a_camera, result);
}

bool LightLimitFix::Hooks::BSShadowFrustumLight_UpdateCamera::thunk(RE::BSShadowLight* This, const RE::NiCamera* a_camera)
{
	const bool result = func(This, a_camera);
	return globals::features::lightLimitFix.FilterLocalShadowCaster(This, a_camera, result);
}

void LightLimitFix::Hooks::Install()
{
	stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	stl::write_vfunc<0x6, BSEffectShader_SetupGeometry>(RE::VTABLE_BSEffectShader[0]);
	stl::write_vfunc<0x6, BSWaterShader_SetupGeometry>(RE::VTABLE_BSWaterShader[0]);

	stl::write_thunk_call<ValidLight1>(REL::RelocationID(100994, 107781).address() + 0x92);
	stl::write_thunk_call<ValidLight2>(REL::RelocationID(100997, 107784).address() + REL::Relocate(0x139, 0x12A));
	stl::write_thunk_call<ValidLight3>(REL::RelocationID(101296, 108283).address() + REL::Relocate(0xB7, 0x7E));

	stl::write_thunk_call<RenderPass1>(REL::RelocationID(100877, 107673).address() + REL::Relocate(0x1E5, 0x1EE));
	stl::write_thunk_call<RenderPass2>(REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F));
	stl::write_thunk_call<RenderPass3>(REL::RelocationID(100871, 107667).address() + REL::Relocate(0xEE, 0xED));
	stl::detour_thunk<BSGeometry_Destroy>(REL::RelocationID(69535, 70936));

	stl::detour_thunk<CalculateActiveShadowCasterLights>(REL::RelocationID(100419, 107137));
	stl::write_vfunc<0x10, BSShadowParabolicLight_UpdateCamera>(RE::VTABLE_BSShadowParabolicLight[0]);
	stl::write_vfunc<0x10, BSShadowFrustumLight_UpdateCamera>(RE::VTABLE_BSShadowFrustumLight[0]);

	logger::info("[LLF] Installed hooks");
}

#undef I18N_KEY_PREFIX
