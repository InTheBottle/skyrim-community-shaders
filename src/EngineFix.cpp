#include "EngineFix.h"

#include "EngineFixes/GrassCull.h"
#include "EngineFixes/ShadowmapCascadeCullingFix.h"
#include "EngineFixes/ShadowmapCascadeRasterizerFix.h"

// NOTE: the static local-light shadow cache is NOT an EngineFix -- it is owned by LightLimitFix and installed
// from LightLimitFix::PostPostLoad (src/Features/LightLimitFix/ShadowMapCacheHooks.*).
const std::vector<EngineFix*>& EngineFix::GetOnPostPostLoadFixesList()
{
	static ShadowmapCascadeCullingFix shadowmapCascadeCullingFix;
	static ShadowmapRasterizerFix shadowmapRasterizerFix;
	static GrassCull grassCull;

	static std::vector<EngineFix*> fixes = {
		&shadowmapCascadeCullingFix,
		&shadowmapRasterizerFix,
		&grassCull
	};

	return fixes;
}

const std::vector<EngineFix*>& EngineFix::GetOnDataLoadedFixesList()
{
	static std::vector<EngineFix*> fixes = {};

	return fixes;
}

void EngineFix::InstallFixes(const std::vector<EngineFix*>& fixes)
{
	for (const auto fix : fixes) {
		fix->Install();
		logger::info("[Engine Fixes] Installed {}", fix->GetName());
	}
}

void EngineFix::InstallOnPostPostLoadFixes()
{
	InstallFixes(GetOnPostPostLoadFixesList());
}

void EngineFix::InstallOnDataLoadedFixes()
{
	InstallFixes(GetOnDataLoadedFixesList());
}
