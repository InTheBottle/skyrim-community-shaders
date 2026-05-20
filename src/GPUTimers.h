#pragma once

#include <d3d11.h>
#include <functional>
#include <string>
#include <vector>
#include <winrt/base.h>

class GPUTimers
{
public:
	static constexpr uint32_t kMaxTimers = 128;
	static constexpr uint32_t kFrameLatency = 3;
	static constexpr uint32_t kHistorySize = 300;

	using PerfEventCallback = std::function<void(std::string_view)>;

	struct TimerResult
	{
		std::string name;
		float gpuTimeMs = 0.0f;
		float avgMs = 0.0f;
		float p95Ms = 0.0f;
		float p99Ms = 0.0f;
		bool valid = false;
	};

	void Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
	void Release();

	void SetPerfEventCallbacks(PerfEventCallback beginCb, PerfEventCallback endCb)
	{
		beginPerfEvent = std::move(beginCb);
		endPerfEvent = std::move(endCb);
	}

	void BeginFrame();
	void BeginPass(const std::string& name);
	void EndPass();
	void EndFrame();

	const std::vector<TimerResult>& GetResults() const { return results; }
	float GetTotalTimeMs() const { return totalTimeMs; }

	void ClearTimers()
	{
		results.clear();
		knownTimers.clear();
		totalTimeMs = 0.0f;
	}

	void ClearTimersForFeature(const std::string& featureName)
	{
		std::erase_if(knownTimers, [&featureName](const KnownTimer& kt) {
			return kt.name.starts_with(featureName + "::");
		});
	}

private:
	struct FrameQueries
	{
		winrt::com_ptr<ID3D11Query> disjoint;
		struct TimerPair
		{
			winrt::com_ptr<ID3D11Query> begin;
			winrt::com_ptr<ID3D11Query> end;
			std::string name;
		};
		std::vector<TimerPair> timers;
		uint32_t activeCount = 0;
		bool inFlight = false;
	};

	ID3D11DeviceContext* context = nullptr;

	FrameQueries frames[kFrameLatency];
	uint32_t writeFrame = 0;
	uint32_t readFrame = 0;
	uint32_t framesSinceInit = 0;
	bool initialized = false;
	bool frameActive = false;

	PerfEventCallback beginPerfEvent;
	PerfEventCallback endPerfEvent;

	std::vector<TimerResult> results;

	struct KnownTimer
	{
		std::string name;
		float lastMs = 0.0f;
		float history[kHistorySize]{};
		uint32_t historyHead = 0;
		uint32_t historyCount = 0;

		void PushSample(float ms)
		{
			history[historyHead] = ms;
			historyHead = (historyHead + 1) % kHistorySize;
			if (historyCount < kHistorySize)
				historyCount++;
			lastMs = ms;
		}

		float GetAverage() const;
		float GetPercentile(float p) const;
	};
	std::vector<KnownTimer> knownTimers;
	float totalTimeMs = 0.0f;

	void CollectResults();
};
