#include "PresetManager.h"

PresetManager& PresetManager::GetSingleton()
{
	static PresetManager instance;
	return instance;
}

bool PresetManager::UseDataFolder() const
{
	return std::filesystem::exists("Data\\enbseries.ini") && std::filesystem::exists("Data\\enbseries\\enbeffect.fx");
}

std::filesystem::path PresetManager::GetENBSeriesPath() const
{
	if (UseDataFolder())
		return "Data\\enbseries";
	return "enbseries";
}

std::filesystem::path PresetManager::GetENBSeriesIniPath() const
{
	if (UseDataFolder())
		return "Data\\enbseries.ini";
	return "enbseries.ini";
}
