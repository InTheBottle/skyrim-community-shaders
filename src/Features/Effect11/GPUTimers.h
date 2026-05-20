#pragma once

#include <d3d11.h>
#include <string>
#include <vector>
#include <winrt/base.h>

class GPUTimers
{
public:
	static constexpr uint32_t kMaxTimers = 16;
	static constexpr uint32_t kFrameLatency = 3;

	struct TimerResult
	{
		std::string name;
		float gpuTimeMs = 0.0f;
		bool valid = false;
	};

	void Initialize(ID3D11Device* device);
	void Release();

	void BeginFrame(ID3D11DeviceContext* context);
	void BeginTimer(ID3D11DeviceContext* context, const std::string& name);
	void EndTimer(ID3D11DeviceContext* context);
	void EndFrame(ID3D11DeviceContext* context);

	const std::vector<TimerResult>& GetResults() const { return results; }
	float GetTotalTimeMs() const { return totalTimeMs; }

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

	FrameQueries frames[kFrameLatency];
	uint32_t writeFrame = 0;
	uint32_t readFrame = 0;
	uint32_t framesSinceInit = 0;
	bool initialized = false;
	bool frameActive = false;

	std::vector<TimerResult> results;
	float totalTimeMs = 0.0f;

	void CollectResults(ID3D11DeviceContext* context);
};
