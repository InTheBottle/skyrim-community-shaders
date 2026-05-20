#include "GPUTimers.h"

#include <algorithm>
#include <unordered_map>

float GPUTimers::KnownTimer::GetAverage() const
{
	if (historyCount == 0)
		return lastMs;
	float sum = 0.0f;
	for (uint32_t i = 0; i < historyCount; i++)
		sum += history[i];
	return sum / static_cast<float>(historyCount);
}

float GPUTimers::KnownTimer::GetPercentile(float p) const
{
	if (historyCount == 0)
		return lastMs;

	thread_local std::vector<float> sorted;
	sorted.resize(historyCount);
	for (uint32_t i = 0; i < historyCount; i++)
		sorted[i] = history[i];
	std::sort(sorted.begin(), sorted.end());

	float idx = (p / 100.0f) * static_cast<float>(historyCount - 1);
	uint32_t lo = static_cast<uint32_t>(idx);
	uint32_t hi = std::min(lo + 1, historyCount - 1);
	float frac = idx - static_cast<float>(lo);
	return sorted[lo] * (1.0f - frac) + sorted[hi] * frac;
}

void GPUTimers::Initialize(ID3D11Device* device, ID3D11DeviceContext* a_context)
{
	Release();

	context = a_context;

	for (auto& frame : frames) {
		D3D11_QUERY_DESC disjointDesc{};
		disjointDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
		device->CreateQuery(&disjointDesc, frame.disjoint.put());

		frame.timers.resize(kMaxTimers);
		for (auto& timer : frame.timers) {
			D3D11_QUERY_DESC tsDesc{};
			tsDesc.Query = D3D11_QUERY_TIMESTAMP;
			device->CreateQuery(&tsDesc, timer.begin.put());
			device->CreateQuery(&tsDesc, timer.end.put());
		}
		frame.activeCount = 0;
		frame.inFlight = false;
	}

	writeFrame = 0;
	readFrame = 0;
	framesSinceInit = 0;
	initialized = true;
}

void GPUTimers::Release()
{
	for (auto& frame : frames) {
		frame.disjoint = nullptr;
		frame.timers.clear();
		frame.activeCount = 0;
		frame.inFlight = false;
	}
	results.clear();
	knownTimers.clear();
	totalTimeMs = 0.0f;
	initialized = false;
	context = nullptr;
}

void GPUTimers::BeginFrame()
{
	if (!initialized || !context || frameActive)
		return;

	CollectResults();

	auto& frame = frames[writeFrame];
	frame.activeCount = 0;
	frame.inFlight = true;
	frameActive = true;
	context->Begin(frame.disjoint.get());
}

void GPUTimers::BeginPass(const std::string& name)
{
	if (!initialized || !context)
		return;

	if (!frameActive)
		BeginFrame();

	auto& frame = frames[writeFrame];
	if (frame.activeCount >= kMaxTimers)
		return;

	auto& timer = frame.timers[frame.activeCount];
	timer.name = name;
	context->End(timer.begin.get());

	if (beginPerfEvent)
		beginPerfEvent(name);
}

void GPUTimers::EndPass()
{
	if (!initialized || !context || !frameActive)
		return;

	auto& frame = frames[writeFrame];
	if (frame.activeCount >= kMaxTimers)
		return;

	context->End(frame.timers[frame.activeCount].end.get());
	frame.activeCount++;

	if (endPerfEvent)
		endPerfEvent({});
}

void GPUTimers::EndFrame()
{
	if (!initialized || !context || !frameActive)
		return;

	frameActive = false;
	context->End(frames[writeFrame].disjoint.get());
	writeFrame = (writeFrame + 1) % kFrameLatency;
	framesSinceInit++;
}

void GPUTimers::CollectResults()
{
	if (framesSinceInit < kFrameLatency)
		return;

	readFrame = writeFrame;
	auto& frame = frames[readFrame];
	if (!frame.inFlight)
		return;

	D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
	HRESULT hr = context->GetData(frame.disjoint.get(), &disjointData, sizeof(disjointData), D3D11_ASYNC_GETDATA_DONOTFLUSH);
	if (hr != S_OK)
		return;

	frame.inFlight = false;

	std::unordered_map<std::string, float> activeTimers;
	float activeTotalMs = 0.0f;

	if (!disjointData.Disjoint) {
		double ticksToMs = 1000.0 / static_cast<double>(disjointData.Frequency);

		for (uint32_t i = 0; i < frame.activeCount; i++) {
			auto& timer = frame.timers[i];
			UINT64 tsBegin = 0, tsEnd = 0;

			if (context->GetData(timer.begin.get(), &tsBegin, sizeof(tsBegin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				continue;
			if (context->GetData(timer.end.get(), &tsEnd, sizeof(tsEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				continue;

			float ms = static_cast<float>(static_cast<double>(tsEnd - tsBegin) * ticksToMs);
			activeTimers[timer.name] = ms;
			activeTotalMs += ms;

			bool isNew = true;
			for (auto& known : knownTimers) {
				if (known.name == timer.name) {
					isNew = false;
					known.PushSample(ms);
					break;
				}
			}
			if (isNew) {
				KnownTimer kt;
				kt.name = timer.name;
				kt.PushSample(ms);
				knownTimers.push_back(std::move(kt));
			}
		}
	}

	totalTimeMs = activeTotalMs;

	results.clear();
	results.reserve(knownTimers.size());
	for (const auto& known : knownTimers) {
		TimerResult result;
		result.name = known.name;
		auto it = activeTimers.find(known.name);
		if (it != activeTimers.end()) {
			result.gpuTimeMs = it->second;
		} else {
			result.gpuTimeMs = known.lastMs;
		}
		result.avgMs = known.GetAverage();
		result.p95Ms = known.GetPercentile(95.0f);
		result.p99Ms = known.GetPercentile(99.0f);
		result.valid = true;
		results.push_back(std::move(result));
	}
}
