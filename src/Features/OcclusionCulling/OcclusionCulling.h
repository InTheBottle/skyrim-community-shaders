#pragma once

#include "Feature.h"

// -----------------------------------------------------------------------------
// OcclusionCulling — CommunityShaders Feature wrapper around the GPU Hi-Z port.
//
// Installs hooks on BSCullingProcess so that (a) once per frame, before the main
// cull walk, the camera Hi-Z snapshot's view/projection is loaded, and (b) each
// processed scene object is occlusion-tested against the GPU-reduced Hi-Z depth
// and skipped if provably hidden. Distant small objects are additionally dropped
// from the shadow maps. Default-on; SE 1.5.97 ONLY.
// -----------------------------------------------------------------------------

struct OcclusionCulling : public Feature
{
	static OcclusionCulling* GetSingleton()
	{
		static OcclusionCulling singleton;
		return &singleton;
	}

	struct Settings
	{
		bool EnableOcclusionTesting = true;
		// Only objects with at least this world-bound radius are occlusion-tested.
		float OccluderTestMinRadius = 0.0f;
		// Distance-scaled small-object SHADOW cull (radius < min + slope*camDist), never actors.
		// Drops the object's shadow only (mesh still renders); cuts shadow-map draws = the bulk
		// of Utility. Safe (invisible) -> on by default.
		bool  CullSmallShadows = true;
		float SmallShadowMinSize = 0.0f;
		float SmallShadowSlope = 0.03f;  // measured: nets Utility down ~0.3ms vs occlusion-off
	};

	Settings settings;

	virtual std::string GetName() override { return "Occlusion Culling"; }
	virtual std::string GetShortName() override { return "OcclusionCulling"; }

	/** @brief Installs the BSCullingProcess hooks and initializes the Hi-Z runtime. */
	virtual void PostPostLoad() override;

	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	// This feature has no shader .ini, so it is force-loaded in PostPostLoad. Neutralize
	// the disk-cache machinery (which assumes an ini version) so it can't crash/invalidate.
	virtual bool ValidateCache(CSimpleIniA&) override { return true; }
	virtual void WriteDiskCacheInfo(CSimpleIniA&) override {}

	/** @brief Pushes the current settings into the MOC runtime globals. */
	void SyncSettingsToMOC();

	/** @brief True when the master env gate + testing setting are both on. */
	bool IsActive() const;
};
