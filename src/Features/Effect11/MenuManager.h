#pragma once

#include "EffectManager.h"
#include "WeatherManager.h"

class MenuManager
{
public:
	static MenuManager& GetSingleton();

	// Main UI rendering method
	void RenderImGui();

private:
	// UI section rendering methods
	void RenderEffectsList();
	void RenderSettingsPanel();
	void RenderWeatherControl();
	void RenderDebugControl();
	void RenderPresetsTab();

	// Helper UI methods
	void RenderAllSettings();
	void RenderStatisticsTab();
	std::map<std::string, std::vector<std::string>> GetCategorizedSettings() const;
	std::vector<int> GetActiveTimeOfDayIndices() const;
	float GetTimeOfDayBlendFactor(int timeIndex) const;

	// Cached GPU timing display (updated ~1s)
	std::vector<GPUTimers::TimerResult> cachedTimerResults;
	float cachedTotalTimeMs = 0.0f;
	float timeSinceLastUpdate = 0.0f;
	float lastFrameTime = 0.0f;
};