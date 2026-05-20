#pragma once

#include <string>
#include <vector>

class StatisticsRenderer
{
public:
	static void RenderStatistics();
	static void RenderFeatureTimers(const std::string& featurePrefix);

private:
	static inline float cachedTotalTimeMs = 0.0f;
	static inline float timeSinceLastUpdate = 0.0f;
	static inline float lastFrameTime = 0.0f;

	struct PassEntry
	{
		std::string label;
		float ms;
		float avgMs;
		float p95Ms;
		float p99Ms;
	};
	struct GroupEntry
	{
		std::string name;
		float totalMs = 0.0f;
		float totalAvgMs = 0.0f;
		float totalP95Ms = 0.0f;
		float totalP99Ms = 0.0f;
		std::vector<PassEntry> passes;
	};
	static inline std::vector<GroupEntry> cachedGroups;
};
