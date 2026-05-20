#include "GPUTimers.h"

void GPUTimers::Initialize(ID3D11Device* device)
{
	Release();

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
	totalTimeMs = 0.0f;
	initialized = false;
}

void GPUTimers::BeginFrame(ID3D11DeviceContext* context)
{
	if (!initialized || frameActive)
		return;

	CollectResults(context);

	auto& frame = frames[writeFrame];
	frame.activeCount = 0;
	frame.inFlight = true;
	frameActive = true;
	context->Begin(frame.disjoint.get());
}

void GPUTimers::BeginTimer(ID3D11DeviceContext* context, const std::string& name)
{
	if (!initialized)
		return;

	if (!frameActive)
		BeginFrame(context);

	auto& frame = frames[writeFrame];
	if (frame.activeCount >= kMaxTimers)
		return;

	auto& timer = frame.timers[frame.activeCount];
	timer.name = name;
	context->End(timer.begin.get());
}

void GPUTimers::EndTimer(ID3D11DeviceContext* context)
{
	if (!initialized || !frameActive)
		return;

	auto& frame = frames[writeFrame];
	if (frame.activeCount >= kMaxTimers)
		return;

	context->End(frame.timers[frame.activeCount].end.get());
	frame.activeCount++;
}

void GPUTimers::EndFrame(ID3D11DeviceContext* context)
{
	if (!initialized || !frameActive)
		return;

	frameActive = false;
	context->End(frames[writeFrame].disjoint.get());
	writeFrame = (writeFrame + 1) % kFrameLatency;
	framesSinceInit++;
}

void GPUTimers::CollectResults(ID3D11DeviceContext* context)
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

	results.clear();
	totalTimeMs = 0.0f;

	if (disjointData.Disjoint)
		return;

	double ticksToMs = 1000.0 / static_cast<double>(disjointData.Frequency);

	for (uint32_t i = 0; i < frame.activeCount; i++) {
		auto& timer = frame.timers[i];
		UINT64 tsBegin = 0, tsEnd = 0;

		if (context->GetData(timer.begin.get(), &tsBegin, sizeof(tsBegin), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			continue;
		if (context->GetData(timer.end.get(), &tsEnd, sizeof(tsEnd), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
			continue;

		TimerResult result;
		result.name = timer.name;
		result.gpuTimeMs = static_cast<float>(static_cast<double>(tsEnd - tsBegin) * ticksToMs);
		result.valid = true;
		totalTimeMs += result.gpuTimeMs;
		results.push_back(std::move(result));
	}
}
