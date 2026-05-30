#include "ExtendedEffect.h"

#ifdef ENABLE_ENB_EXTENDER

#include <sstream>

#include "../EffectManager.h"
#include "../PresetManager.h"
#include "../WeatherManager.h"

void ExtendedEffect::Unload()
{
	weatherData.clear();
	bindingCache.clear();
	Effect::Unload();
}

// Technique evaluation

int ExtendedEffect::ResolveTechniqueBinding(const std::string& variableName)
{
	auto cacheIt = bindingCache.find(variableName);
	if (cacheIt != bindingCache.end())
		return cacheIt->second;

	for (int i = 0; i < static_cast<int>(uiVariables.size()); ++i) {
		auto& uiVar = uiVariables[i];
		const std::string& uname = !uiVar.uniqueName.empty() ? uiVar.uniqueName
		                           : !uiVar.group.empty()    ? uiVar.group + "." + uiVar.displayName
		                                                     : uiVar.displayName;
		if (uname == variableName) {
			bindingCache[variableName] = i;
			return i;
		}
	}

	bindingCache[variableName] = -2;
	return -2;
}

bool ExtendedEffect::IsTechniqueEnabled(TechniqueInfo& info)
{
	for (auto& binding : info.bindings) {
		int idx = ResolveTechniqueBinding(binding.variableName);
		if (idx < 0)
			continue;

		auto& uiVar = uiVariables[idx];
		bool val = false;
		switch (uiVar.type) {
		case UIVariableType::Bool: val = uiVar.boolValue; break;
		case UIVariableType::Int: val = uiVar.intValue != 0; break;
		case UIVariableType::Float: val = uiVar.floatValue != 0.0f; break;
		default: val = true; break;
		}

		if (binding.inverted ? !val : val)
			continue;
		return false;
	}
	return true;
}

// Time-of-day interpolation

float ExtendedEffect::GetPeriodWeight(const std::string& period)
{
	auto& cd = EffectManager::GetSingleton().commonData;
	if (period == "Dawn") return cd.timeOfDay1[static_cast<int>(TimeOfDay1Index::Dawn)];
	if (period == "Sunrise") return cd.timeOfDay1[static_cast<int>(TimeOfDay1Index::Sunrise)];
	if (period == "Day") return cd.timeOfDay1[static_cast<int>(TimeOfDay1Index::Day)];
	if (period == "Sunset") return cd.timeOfDay1[static_cast<int>(TimeOfDay1Index::Sunset)];
	if (period == "Dusk") return cd.timeOfDay2[static_cast<int>(TimeOfDay2Index::Dusk)];
	if (period == "Night") return cd.timeOfDay2[static_cast<int>(TimeOfDay2Index::Night)];
	if (period == "Interior") return cd.eInteriorFactor;
	return 0.0f;
}

void ExtendedEffect::ApplyTimeOfDayInterpolation()
{
	struct PeriodVar
	{
		size_t index;
		float weight;
	};
	std::unordered_map<std::string, std::vector<PeriodVar>> baseGroups;

	for (size_t i = 0; i < uiVariables.size(); ++i) {
		auto& uiVar = uiVariables[i];
		if (uiVar.timePeriod.empty() || uiVar.isSeparator || !uiVar.effectVariable)
			continue;
		auto& name = uiVar.name;
		auto& period = uiVar.timePeriod;
		if (name.size() <= period.size() || name.compare(name.size() - period.size(), period.size(), period) != 0)
			continue;
		baseGroups[name.substr(0, name.size() - period.size())].push_back({ i, GetPeriodWeight(period) });
	}

	auto& cd = EffectManager::GetSingleton().commonData;

	for (auto& [baseName, entries] : baseGroups) {
		auto baseVarIt = variables.find(baseName);
		if (baseVarIt == variables.end())
			continue;
		auto* baseVar = baseVarIt->second.get();
		if (!baseVar || !baseVar->IsValid())
			continue;

		auto& sep = uiVariables[entries[0].index].separation;
		if (sep == "ExteriorWeather" && cd.eInteriorFactor > 0.0f)
			continue;

		float totalWeight = 0.0f;
		for (auto& e : entries)
			totalWeight += e.weight;
		if (totalWeight <= 0.0f)
			continue;

		auto& firstVar = uiVariables[entries[0].index];

		if (firstVar.type == UIVariableType::Float) {
			float result = 0.0f;
			for (auto& e : entries)
				result += uiVariables[e.index].floatValue * (e.weight / totalWeight);
			baseVar->AsScalar()->SetFloat(result);
		} else {
			int comps = (firstVar.type == UIVariableType::Float2) ? 2 : (firstVar.type == UIVariableType::Float3) ? 3 : 4;
			float result[4] = {};
			for (auto& e : entries) {
				float w = e.weight / totalWeight;
				for (int c = 0; c < comps; ++c)
					result[c] += uiVariables[e.index].vectorValue[c] * w;
			}
			baseVar->AsVector()->SetFloatVector(result);
		}
	}
}

// Weather blending

void ExtendedEffect::LoadWeatherData()
{
	weatherData.clear();

	std::string section = GetName();
	std::transform(section.begin(), section.end(), section.begin(), ::toupper);

	auto& weatherManager = WeatherManager::GetSingleton();
	const auto& weatherEntries = weatherManager.GetWeatherEntries();

	for (const auto& [key, entry] : weatherEntries) {
		std::filesystem::path filePath = std::filesystem::absolute(PresetManager::GetSingleton().GetENBSeriesPath() / entry.fileName);
		if (!std::filesystem::exists(filePath))
			continue;

		std::string filePathStr = filePath.string();

		WeatherValues values;
		for (const auto& uiVar : uiVariables) {
			if (uiVar.isSeparator || uiVar.isLabel)
				continue;
			if (!uiVar.effectVariable && !uiVar.isDefine)
				continue;

			std::string iniKey = GetVariableIniKey(uiVar);
			if (iniKey.empty())
				continue;

			if (IsPerComponentVector(uiVar)) {
				static const char* suffixes[] = { "X", "Y", "Z", "W" };
				int comps = (uiVar.type == UIVariableType::Float2) ? 2 : (uiVar.type == UIVariableType::Float3) ? 3 : 4;
				for (int c = 0; c < comps; ++c) {
					std::string compKey = iniKey + suffixes[c];
					char buffer[256];
					DWORD result = GetPrivateProfileStringA(section.c_str(), compKey.c_str(), "", buffer, sizeof(buffer), filePathStr.c_str());
					if (result > 0)
						values[compKey] = buffer;
				}
			} else {
				char buffer[1024];
				DWORD result = GetPrivateProfileStringA(section.c_str(), iniKey.c_str(), "", buffer, sizeof(buffer), filePathStr.c_str());
				if (result > 0)
					values[iniKey] = buffer;
			}
		}

		if (!values.empty()) {
			for (uint32_t weatherID : entry.weatherIDs)
				weatherData[weatherID] = values;
		}
	}

	if (!weatherData.empty())
		logger::info("[ExtendedEffect] Loaded weather data for '{}' ({} weathers)", GetName(), weatherData.size());
}

void ExtendedEffect::ApplyWeatherBlending(float blendFactor, uint32_t currentWeatherID, uint32_t lastWeatherID)
{
	if (weatherData.empty())
		return;

	auto currentIt = weatherData.find(currentWeatherID);
	auto lastIt = weatherData.find(lastWeatherID);

	if (currentIt == weatherData.end() && lastIt == weatherData.end())
		return;

	auto safeStof = [](const std::string& s, float fallback) -> float {
		try { return std::stof(s); } catch (...) { return fallback; }
	};

	for (auto& uiVar : uiVariables) {
		if (uiVar.isSeparator || uiVar.isLabel)
			continue;
		if (!uiVar.effectVariable && !uiVar.isDefine)
			continue;

		std::string iniKey = GetVariableIniKey(uiVar);
		if (iniKey.empty())
			continue;

		switch (uiVar.type) {
		case UIVariableType::Float:
			{
				auto getVal = [&](const WeatherValues* vals) -> float {
					if (!vals) return uiVar.floatValue;
					auto it = vals->find(iniKey);
					if (it == vals->end()) return uiVar.floatValue;
					return safeStof(it->second, uiVar.floatValue);
				};

				float currentVal = getVal(currentIt != weatherData.end() ? &currentIt->second : nullptr);
				float lastVal = getVal(lastIt != weatherData.end() ? &lastIt->second : nullptr);
				uiVar.floatValue = lastVal + blendFactor * (currentVal - lastVal);
				if (uiVar.effectVariable)
					uiVar.effectVariable->AsScalar()->SetFloat(uiVar.floatValue);
				break;
			}
		case UIVariableType::Float2:
		case UIVariableType::Float3:
		case UIVariableType::Float4:
			{
				int comps = (uiVar.type == UIVariableType::Float2) ? 2 : (uiVar.type == UIVariableType::Float3) ? 3 : 4;
				bool perComp = IsPerComponentVector(uiVar);

				auto parseVec = [&](const WeatherValues* vals, float* out) {
					if (!vals) {
						memcpy(out, uiVar.vectorValue, sizeof(float) * comps);
						return;
					}
					if (perComp) {
						static const char* suffixes[] = { "X", "Y", "Z", "W" };
						for (int c = 0; c < comps; ++c) {
							auto it = vals->find(iniKey + suffixes[c]);
							out[c] = (it != vals->end()) ? safeStof(it->second, uiVar.vectorValue[c]) : uiVar.vectorValue[c];
						}
					} else {
						auto it = vals->find(iniKey);
						if (it != vals->end()) {
							std::stringstream ss(it->second);
							std::string item;
							for (int c = 0; c < comps && std::getline(ss, item, ','); ++c)
								out[c] = safeStof(item, uiVar.vectorValue[c]);
						} else {
							memcpy(out, uiVar.vectorValue, sizeof(float) * comps);
						}
					}
				};

				float currentVals[4] = {}, lastVals[4] = {};
				parseVec(currentIt != weatherData.end() ? &currentIt->second : nullptr, currentVals);
				parseVec(lastIt != weatherData.end() ? &lastIt->second : nullptr, lastVals);

				for (int c = 0; c < comps; ++c)
					uiVar.vectorValue[c] = lastVals[c] + blendFactor * (currentVals[c] - lastVals[c]);

				if (uiVar.effectVariable)
					uiVar.effectVariable->AsVector()->SetFloatVector(uiVar.vectorValue);
				break;
			}
		default:
			break;
		}
	}
}

void ExtendedEffect::SyncWeatherDataFromUI(uint32_t weatherID)
{
	auto weatherIt = weatherData.find(weatherID);
	if (weatherIt == weatherData.end())
		return;

	auto& values = weatherIt->second;

	for (const auto& uiVar : uiVariables) {
		if (uiVar.isSeparator || uiVar.isLabel)
			continue;
		if (!uiVar.effectVariable && !uiVar.isDefine)
			continue;

		std::string iniKey = GetVariableIniKey(uiVar);
		if (iniKey.empty() || values.find(iniKey) == values.end())
			continue;

		switch (uiVar.type) {
		case UIVariableType::Float:
			values[iniKey] = std::to_string(uiVar.floatValue);
			break;
		case UIVariableType::Float2:
		case UIVariableType::Float3:
		case UIVariableType::Float4:
			{
				int comps = (uiVar.type == UIVariableType::Float2) ? 2 : (uiVar.type == UIVariableType::Float3) ? 3 : 4;
				if (IsPerComponentVector(uiVar)) {
					static const char* suffixes[] = { "X", "Y", "Z", "W" };
					for (int c = 0; c < comps; ++c) {
						std::string compKey = iniKey + suffixes[c];
						if (values.find(compKey) != values.end())
							values[compKey] = std::to_string(uiVar.vectorValue[c]);
					}
				} else {
					std::string val;
					for (int c = 0; c < comps; ++c) {
						if (c > 0) val += ", ";
						val += std::to_string(uiVar.vectorValue[c]);
					}
					values[iniKey] = val;
				}
				break;
			}
		default:
			break;
		}
	}
}

#endif
