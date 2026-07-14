#include "Features/RemoteControl/DevBenchBridge.h"

#include "ShadowDeferred.h"
#include "Vanilla/ShadowThreaded.h"
#include "Vanilla/UtilityPassReplica.h"

// Registers our tools into the devbench test bench over its C-ABI. Gated by
// DEVBENCH_BRIDGE_ENABLED (set by CMake when the devbench-api port is available);
// otherwise this file compiles to an empty Install(). Inert at runtime when no
// devbench plugin is present (GetDevBenchInterface001() returns null).
//
// The communityshaders.* tools below expose Community Shaders' graphics-feature, inspect,
// capture, shadercache, and settings operations through the single devbench
// host over both MCP and REST. Each is namespaced to avoid collisions in devbench's
// shared registry; the action / kind / inputSchema shapes are stable so clients can
// rely on them.

#ifdef DEVBENCH_BRIDGE_ENABLED

#	include "Feature.h"
#	include "Features/RenderDoc.h"
#	include "Features/ScreenshotFeature.h"
#	include "Globals.h"
#	include "Profiler.h"
#	include "ShaderCache.h"
#	include "State.h"

#	include <DevBenchAPI.h>
#	include <nlohmann/json.hpp>

#	include <algorithm>
#	include <chrono>
#	include <cstring>
#	include <functional>
#	include <future>
#	include <memory>
#	include <optional>
#	include <stdexcept>

namespace
{
	using json = nlohmann::json;

	// Current render frame, used as a coarse "enqueued at" stamp so callers can poll
	// inspect(kind=state) until frame_count advances past it (i.e. a queued main-thread
	/**
	 * @brief Retrieves the current render frame count.
	 *
	 * Safe to call from any thread.
	 *
	 * @return uint The current frame count, or 0 if global state is unavailable.
	 */
	uint EnqueuedFrame()
	{
		return globals::state ? globals::state->frameCountAtomic.load(std::memory_order_relaxed) : 0u;
	}

	// Shared C-ABI handler body. The whole request — parse, dispatch, dump — is wrapped
	// so NO exception ever crosses the DLL boundary, and a_write is called exactly once.
	// `a_build` is a plain function pointer (no captures) so this composes with the
	/**
	 * @brief Wraps a DevBench tool handler to safely marshal exceptions into JSON error responses.
	 *
	 * Parses JSON arguments from the input string, invokes the builder function, and writes
	 * the resulting JSON response to the sink via the provided callback. All exceptions are
	 * caught and converted to standardized JSON error objects to prevent C++ exceptions from
	 * crossing the DLL boundary.
	 *
	 * @param a_build Function that builds a JSON response from parsed request arguments.
	 * @param a_argsJson JSON-formatted request arguments as a string; null or empty string
	 *                   defaults to an empty JSON object.
	 * @param a_sink Opaque destination handle passed to the write callback.
	 * @param a_write Callback function invoked exactly once to write the serialized JSON response.
	 */
	void RunHandler(json (*a_build)(const json&), const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		json out;
		try {
			json args = json::object();
			if (a_argsJson && *a_argsJson)
				args = json::parse(a_argsJson);  // throws on malformed input
			if (!args.is_object())
				throw std::runtime_error("arguments must be a JSON object");
			out = a_build(args);
		} catch (const std::exception& e) {
			out = json{ { "error", "invalid request" }, { "detail", e.what() } };
		} catch (...) {
			out = json{ { "error", "unknown handler error" } };
		}
		const std::string dumped = out.dump();
		a_write(a_sink, dumped.c_str());
	}

	// Run a task on the main thread and return its result. Bridge handlers run on devbench's
	// listener thread; work that touches state the render/UI thread mutates (A/B fields, JSON
	// snapshots, feature settings, capture triggers) must marshal or it races. Blocks the
	// handler briefly, bounded so a stalled main thread (e.g. mid-load) can't hang it.
	//
	// The body may have SIDE EFFECTS (set/reset apply settings; capture triggers a frame), so a
	// `cancelled` flag is checked at task entry: if we already gave up waiting, the task skips
	// the body rather than mutating state after we reported a timeout. shared_ptr state so a
	// task that runs after we return doesn't dangle. (Best-effort: a task that starts exactly at
	/**
	 * @brief Executes a callable on the main thread and waits for its result.
	 *
	 * Schedules the callable to run on the main thread via SKSE's task interface with a 5-second
	 * timeout. If the timeout elapses, sets an internal flag to prevent delayed side effects.
	 *
	 * @param a_run Callable that returns a JSON result.
	 * @return The JSON result from `a_run()`, or an error JSON object if the task interface is
	 * unavailable, execution times out, or an exception is thrown.
	 */
	json RunOnMainThread(std::function<json()> a_run)
	{
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			return json{ { "error", "SKSE task interface unavailable" } };
		auto prom = std::make_shared<std::promise<json>>();
		auto cancelled = std::make_shared<std::atomic<bool>>(false);
		auto fut = prom->get_future();
		task->AddTask([prom, cancelled, run = std::move(a_run)]() {
			if (cancelled->load(std::memory_order_acquire))
				return;  // handler already timed out and returned — don't run a side-effecting body late
			try {
				prom->set_value(run());
			} catch (const std::exception& e) {
				prom->set_value(json{ { "error", "task threw on main thread" }, { "detail", e.what() } });
			} catch (...) {
				prom->set_value(json{ { "error", "task threw on main thread" }, { "detail", "non-std exception" } });
			}
		});
		if (fut.wait_for(std::chrono::milliseconds(5000)) != std::future_status::ready) {
			cancelled->store(true, std::memory_order_release);
			return json{ { "error", "main thread did not run within 5000ms (mid-load?)" } };
		}
		return fut.get();
	}

	// ---- feature: list / get / set / reset / toggle -----------------------------------

	// Build one feature entry, including restart-gated metadata so `list` answers
	/**
	 * @brief Constructs a JSON object describing a feature's state and any pending restart-required changes.
	 *
	 * Includes feature metadata (name, version, category, load state, etc.) and, if the feature 
	 * has restart-required fields, an array of those fields with their pending status determined 
	 * by comparing boot values against current live settings.
	 *
	 * @return json Feature entry with metadata and optional restart fields array.
	 */
	json FeatureEntry(Feature* f)
	{
		json entry{
			{ "name", f->GetName() },
			{ "shortName", f->GetShortName() },
			{ "loaded", f->loaded },
			{ "version", f->version },
			{ "category", std::string(f->GetCategory()) },
			{ "isCore", f->IsCore() },
			{ "inMenu", f->IsInMenu() },
		};

		const auto fields = f->GetRestartRequiredFields();
		if (!fields.empty()) {
			json restartFields = json::array();
			const auto* liveBase = reinterpret_cast<const unsigned char*>(f->GetSettingsBlob());
			const size_t liveSize = f->GetSettingsBlobSize();
			for (const auto& field : fields) {
				bool pending = false;
				// Compare against remaining bytes (not offset+size, which can overflow and turn
				// a bad metadata entry into an out-of-bounds memcmp).
				if (liveBase && field.jsonKey && field.size != 0 &&
					field.offset <= liveSize && field.size <= liveSize - field.offset) {
					const void* boot = f->GetBootValue(field.jsonKey);
					if (boot && std::memcmp(boot, liveBase + field.offset, field.size) != 0)
						pending = true;
				}
				restartFields.push_back(json{
					{ "key", field.jsonKey ? field.jsonKey : "" },
					{ "label", field.label ? field.label : "" },
					{ "pending", pending },
				});
			}
			entry["restartFields"] = restartFields;
		}
		return entry;
	}

	/**
	 * @brief Dispatches feature management operations (list, get, set, reset, toggle).
	 * 
	 * Validates parameters and executes the requested feature operation, returning
	 * success or error details in JSON format.
	 * 
	 * @param a_args JSON object specifying the action and operation parameters.
	 * @return JSON object containing the operation result or error information.
	 */
	json BuildFeatureResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string("list"));

		if (action == "list") {
			// Marshal: FeatureEntry reads Feature::loaded and restart-gated settings bytes that
			// main-thread toggles / settings-loads mutate.
			return RunOnMainThread([]() {
				json out = json::array();
				for (auto* f : Feature::GetFeatureList())
					out.push_back(FeatureEntry(f));
				return out;
			});
		}

		const std::string shortName = a_args.value("shortName", std::string{});

		if (action == "toggle") {
			// Match over the full feature list (NOT FindFeatureByShortName, which only
			// matches *loaded* features — that makes toggle one-way: you could disable a
			// feature but never re-enable it).
			Feature* target = nullptr;
			if (!shortName.empty()) {
				for (auto* f : Feature::GetFeatureList()) {
					if (f->GetShortName() == shortName) {
						target = f;
						break;
					}
				}
			}
			if (!target)
				return json{ { "error", "unknown or missing shortName" }, { "shortName", shortName } };
			auto* task = SKSE::GetTaskInterface();
			if (!task)
				return json{ { "error", "SKSE task interface unavailable" }, { "shortName", shortName } };
			const uint frame = EnqueuedFrame();
			// If `enabled` is omitted we flip the CURRENT value — but that read must happen on
			// the main thread INSIDE the task: computing !target->loaded here on the listener
			// thread lets concurrent toggles all observe the same stale value and enqueue
			// identical results. With an explicit `enabled`, apply it verbatim.
			// Threading contract: Feature::loaded is a public flag the render pipeline reads
			// per-frame via ForEachLoadedFeature without synchronization, hot-toggled by direct
			// assignment — touch it ONLY on the main thread. The applied value is reported via
			// the communityshaders.feature.changed event (authoritative; the response can't know an
			// implicit flip's result synchronously).
			const bool hasExplicit = a_args.contains("enabled");
			const bool explicitVal = a_args.value("enabled", false);
			task->AddTask([target, hasExplicit, explicitVal, shortName]() {
				const bool applied = hasExplicit ? explicitVal : !target->loaded;
				target->loaded = applied;
				if (auto* dvb = DevBenchAPI::GetDevBenchInterface001()) {
					const std::string payload = json{ { "shortName", shortName }, { "enabled", applied } }.dump();
					dvb->EmitEvent("communityshaders.feature.changed", payload.c_str());
				}
			});
			json r{ { "action", "toggle" }, { "shortName", shortName }, { "queued", true }, { "enqueued_at_frame", frame } };
			if (hasExplicit)
				r["requested"] = explicitVal;  // implicit flip's result arrives via the event
			return r;
		}

		if (shortName.empty())
			return json{ { "error", "missing required parameter 'shortName'" }, { "action", action } };

		// get / set / reset operate on a loaded feature. FindFeatureByShortName filters on
		// Feature::loaded, which queued toggle tasks mutate on the main thread — so resolve the
		// target INSIDE the main-thread path for each action, never on the listener thread.

		if (action == "get") {
			return RunOnMainThread([shortName]() -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				json blob;
				feature->SaveSettings(blob);
				return blob;
			});
		}

		// set / reset resolve + apply on the main thread and report the real outcome: an invalid
		// shortName must NOT come back as a fake success. Synchronous (LoadSettings is fast), so
		// the lookup race is avoided AND the caller learns whether the mutation actually applied.
		if (action == "set") {
			if (!a_args.contains("settings") || !a_args["settings"].is_object())
				return json{ { "error", "missing required object parameter 'settings'" } };
			json blob = a_args["settings"];
			return RunOnMainThread([blob, shortName]() mutable -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				try {
					feature->LoadSettings(blob);
					logger::info("DevBenchBridge: feature(set, {}) applied", shortName);
					return json{ { "action", "set" }, { "shortName", shortName }, { "applied", true } };
				} catch (const std::exception& e) {
					return json{ { "error", "LoadSettings threw" }, { "shortName", shortName }, { "detail", e.what() } };
				}
			});
		}

		if (action == "reset") {
			return RunOnMainThread([shortName]() -> json {
				auto* feature = Feature::FindFeatureByShortName(shortName);
				if (!feature)
					return json{ { "error", "feature not found or not loaded" }, { "shortName", shortName } };
				try {
					feature->RestoreDefaultSettings();
					logger::info("DevBenchBridge: feature(reset, {}) applied", shortName);
					return json{ { "action", "reset" }, { "shortName", shortName }, { "applied", true } };
				} catch (const std::exception& e) {
					return json{ { "error", "RestoreDefaultSettings threw" }, { "shortName", shortName }, { "detail", e.what() } };
				}
			});
		}

		return json{ { "error", "unknown action (list|get|set|reset|toggle)" }, { "action", action } };
	}

	/**
	 * @brief Handles feature-related DevBench tool requests.
	 *
	 * Routes feature operations (list, get, set, reset, toggle) through the standard
	 * handler wrapper to ensure exceptions do not cross the DLL boundary and exactly
	 * one response is written to the output sink.
	 */
	void FeatureToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildFeatureResult, a_argsJson, a_sink, a_write);
	}

	/**
	 * @brief Builds inspection data for the specified kind query (state, profiler, or shader cache).
	 *
	 * @param a_args JSON object containing the inspection query; must include a required `kind` field.
	 *               Optional `filter` parameter narrows profiler results to entries whose name contains the filter string.
	 * @return JSON object with the requested inspection data, or an error object if `kind` is missing or unsupported.
	 *         For `kind="state"`, includes plugin name and frame count.
	 *         For `kind="profiler"`, includes GPU/CPU timing statistics and per-pass history samples.
	 *         For `kind="shadercache"`, includes compilation status and task/failure counts.
	 */

	json BuildInspectResult(const json& a_args)
	{
		const std::string kind = a_args.value("kind", std::string{});
		if (kind.empty())
			return json{ { "error", "missing required parameter 'kind'" } };

		if (kind == "state") {
			return json{
				{ "plugin", "CommunityShaders" },
				{ "frame_count", EnqueuedFrame() },
				{ "draw_submits", globals::state ? globals::state->drawSubmitsLastFrame.load(std::memory_order_relaxed) : 0u },
				{ "draw_submits_by_type", [] {
					 static constexpr const char* kTypeNames[] = { "none", "grass", "sky", "water", "bloodsplatter",
						 "imagespace", "lighting", "effect", "utility", "distanttree", "particle", "total_enum" };
					 json byType = json::object();
					 if (globals::state)
						 for (std::size_t i = 0; i <= RE::BSShader::Type::Total; ++i)
							 byType[kTypeNames[i]] = globals::state->drawSubmitsByTypeLastFrame[i].load(std::memory_order_relaxed);
					 return byType;
				 }() },
				{ "frame_time_by_type", [] {
					 static constexpr const char* kTypeNames[] = { "none", "grass", "sky", "water", "bloodsplatter",
						 "imagespace", "lighting", "effect", "utility", "distanttree", "particle", "total_enum" };
					 json byType = json::object();
					 if (globals::state) {
						 // Per-type timing is otherwise only populated while the perf overlay is
						 // on screen; flip this so headless A/B runs read it. Sticky after the
						 // first poll -- a few QueryPerformanceCounter calls per draw.
						 globals::state->benchForceFrameTiming.store(true, std::memory_order_relaxed);
						 for (std::size_t i = 0; i <= RE::BSShader::Type::Total; ++i)
							 byType[kTypeNames[i]] = globals::state->smoothFrameTimePerType[i];
					 }
					 return byType;
				 }() },
			};
		}
		if (kind == "profiler") {
			const std::string filter = a_args.value("filter", std::string{});
			return RunOnMainThread([filter]() -> json {
				auto* profiler = globals::profiler;
				if (!profiler)
					return json{ { "error", "profiler unavailable" } };
				json passes = json::array();
				for (const auto& r : profiler->GetResults()) {
					if (!r.valid)
						continue;
					if (!filter.empty() && r.name.find(filter) == std::string::npos)
						continue;
					json entry{
						{ "name", r.name },
						{ "gpuMs", r.gpuTimeMs },
						{ "gpuAvgMs", r.avgMs },
						{ "gpuP95Ms", r.p95Ms },
						{ "gpuP99Ms", r.p99Ms },
						{ "cpuMs", r.cpuTimeMs },
						{ "cpuAvgMs", r.cpuAvgMs },
					};
					json hist = json::array();
					for (uint32_t i = 0; i < r.historyCount; ++i)
						hist.push_back(r.GetHistorySample(i));
					entry["gpuHistory"] = hist;
					passes.push_back(entry);
				}
				return json{
					{ "totalGpuMs", profiler->GetTotalTimeMs() },
					{ "totalCpuMs", profiler->GetCpuTotalTimeMs() },
					{ "frame_count", EnqueuedFrame() },
					{ "passes", passes },
				};
			});
		}
		if (kind == "shadercache") {
			// Built from thread-safe ShaderCache accessors. Poll completedTasks against a
			// pre-deploy snapshot to know a hot-reloaded shader finished; a rising
			// failedTasks / currentFailedCount surfaces an otherwise-invisible failed compile.
			auto* cache = globals::shaderCache;
			if (!cache)
				return json{ { "error", "shader cache unavailable" } };
			return json{
				{ "compiling", cache->IsCompiling() },
				{ "completedTasks", cache->GetCompletedTasks() },
				{ "totalTasks", cache->GetTotalTasks() },
				{ "failedTasks", cache->GetFailedTasks() },
				{ "currentFailedCount", cache->GetCurrentFailedCount() },
				{ "frame_count", EnqueuedFrame() },
			};
		}
		return json{ { "error", "unknown kind" }, { "kind", kind }, { "supported", json::array({ "state", "shadercache", "profiler" }) } };
	}

	/**
	 * @brief Handles DevBench inspection requests for plugin state, profiler metrics, and shader cache status.
	 */
	void InspectToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildInspectResult, a_argsJson, a_sink, a_write);
	}

	/**
	 * @brief Dispatches shader cache operations (clear or deleteDisk) to the main thread.
	 *
	 * Validates that the shader cache and task interface are available, then enqueues
	 * the requested operation on the main thread. Returns immediately with a queued status
	 * or an error if preconditions are not met.
	 *
	 * @param a_args JSON object containing an optional "action" field ("clear" or "deleteDisk").
	 * @return JSON object with `{ action, queued, enqueued_at_frame, note }` on success,
	 *         or `{ error, ... }` if the shader cache is unavailable, task interface is unavailable,
	 *         or action is unrecognized.
	 */

	json BuildShadercacheResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		auto* cache = globals::shaderCache;
		if (!cache)
			return json{ { "error", "shader cache unavailable" } };
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			return json{ { "error", "SKSE task interface unavailable" } };
		const uint frame = EnqueuedFrame();

		// Mutating cache ops touch the live ShaderCache (and, for deleteDisk, the filesystem)
		// — marshal to the main thread. Recompiles are observable via inspect(kind=shadercache)
		// + the communityshaders.shaderRecompiled event. NOTE clear vs deleteDisk: clear only drops
		// the in-memory maps, so with the disk cache enabled shaders reload from Data/ShaderCache
		// rather than recompiling — only deleteDisk guarantees a cold recompile.
		if (action == "clear") {
			task->AddTask([cache]() { cache->Clear(); });
			return json{ { "action", "clear" }, { "queued", true }, { "enqueued_at_frame", frame }, { "note", "in-memory cache dropped; shaders reload from the disk cache if present, else recompile (use deleteDisk to force a cold recompile)" } };
		}
		if (action == "deleteDisk") {
			// Delete on disk AND drop the in-memory cache — otherwise existing variants keep
			// serving from memory and the promised full recompile never happens (mirrors
			// PerformClearShaderCache and the ShaderCache invalidation path).
			task->AddTask([cache]() {
				cache->DeleteDiskCache();
				cache->Clear();
			});
			return json{ { "action", "deleteDisk" }, { "queued", true }, { "enqueued_at_frame", frame }, { "note", "on-disk + in-memory shader cache cleared; a full recompile follows (cold-compile benchmark)" } };
		}
		return json{ { "error", "unknown action (clear|deleteDisk)" }, { "action", action } };
	}

	/**
	 * @brief Processes DevBench requests for shader cache operations.
	 */
	void ShadercacheToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildShadercacheResult, a_argsJson, a_sink, a_write);
	}

	/**
	 * @brief Builds a JSON response for a capture request.
	 *
	 * Processes RenderDoc or screenshot capture operations based on the `kind` parameter.
	 * Validates feature availability and queues the capture on the main thread.
	 *
	 * @param a_args JSON object containing `kind` (required) and optional `frames` for RenderDoc.
	 * @return JSON response indicating queued state or error details.
	 */

	json BuildCaptureResult(const json& a_args)
	{
		const std::string kind = a_args.value("kind", std::string{});
		if (kind.empty())
			return json{ { "error", "missing required parameter 'kind'" } };
		const uint frame = EnqueuedFrame();

		if (kind == "renderdoc") {
			uint32_t frameCount = 1;
			if (a_args.contains("frames") && a_args["frames"].is_number())
				frameCount = static_cast<uint32_t>(std::clamp(a_args["frames"].get<int>(), 1, 120));
			// All on the main thread: Feature::loaded is mutated by queued toggle tasks there,
			// and TriggerCapture invalidates RenderDoc's non-atomic capture-list cache the
			// UI/render thread reads.
			return RunOnMainThread([frameCount, frame]() -> json {
				auto* renderDoc = &globals::features::renderDoc;
				if (!renderDoc->loaded)
					return json{ { "error", "RenderDoc feature is not loaded" }, { "hint", "communityshaders.feature(action='list') shows RenderDoc.loaded" } };
				if (!renderDoc->IsAvailable())
					return json{ { "error", "RenderDoc API not available — attach RenderDoc or load the in-app DLL" } };
				if (frameCount == 1)
					renderDoc->TriggerCapture();
				else
					renderDoc->TriggerMultiFrameCapture(frameCount);
				return json{ { "queued", true }, { "kind", "renderdoc" }, { "frames", frameCount }, { "enqueued_at_frame", frame } };
			});
		}

		if (kind == "screenshot") {
			// loaded read on the main thread (toggle tasks mutate it); the request flag is atomic.
			return RunOnMainThread([frame]() -> json {
				auto* shot = &globals::features::screenshotFeature;
				if (!shot->loaded)
					return json{ { "error", "Screenshot feature is not loaded" } };
				shot->captureRequested.store(true, std::memory_order_release);
				return json{ { "queued", true }, { "kind", "screenshot" }, { "enqueued_at_frame", frame } };
			});
		}

		return json{ { "error", "unknown kind" }, { "kind", kind }, { "supported", json::array({ "renderdoc", "screenshot" }) } };
	}

	/**
	 * @brief Processes capture requests for RenderDoc and screenshot operations.
	 *
	 * DevBench tool handler that triggers RenderDoc frame captures or screenshot
	 * captures based on the provided `kind` parameter.
	 */
	void CaptureToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildCaptureResult, a_argsJson, a_sink, a_write);
	}

	/**
	 * @brief Builds a JSON response for global settings operations.
	 *
	 * Enqueues a main-thread task to perform the specified action on the global Community Shaders
	 * configuration. Returns immediately with queuing status and frame information.
	 *
	 * @param a_args JSON object containing an "action" field: `save`, `load`, or `reset`.
	 * @return JSON object with `action`, `queued`, and `enqueued_at_frame` on success;
	 *         `error` and optionally `action` on validation failure.
	 */

	json BuildSettingsResult(const json& a_args)
	{
		const std::string action = a_args.value("action", std::string{});
		if (action.empty())
			return json{ { "error", "missing required parameter 'action'" } };
		auto* state = globals::state;
		if (!state)
			return json{ { "error", "State singleton unavailable" } };
		auto* task = SKSE::GetTaskInterface();
		if (!task)
			return json{ { "error", "SKSE task interface unavailable" } };
		const uint frame = EnqueuedFrame();

		// State::Save/Load read and write the on-disk USER config and touch every feature's
		// settings; both must run on the main thread for the same reason feature(set) does.
		// Contain failures inside the task: RunHandler's guard no longer applies once this runs
		// on SKSE's queue, so a malformed config / Save|Load throw would unwind on the game
		// thread after we already replied "queued". Degrade to a logged error instead.
		if (action == "save") {
			task->AddTask([state]() {
				try {
					state->Save(State::ConfigMode::USER);
					logger::info("DevBenchBridge: settings(save) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: settings(save) failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: settings(save) failed (unknown)");
				}
			});
			return json{ { "action", "save" }, { "queued", true }, { "enqueued_at_frame", frame } };
		}
		if (action == "load") {
			task->AddTask([state]() {
				try {
					state->Load(State::ConfigMode::USER, /*allowReload=*/true);
					logger::info("DevBenchBridge: settings(load) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: settings(load) failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: settings(load) failed (unknown)");
				}
			});
			return json{ { "action", "load" }, { "queued", true }, { "enqueued_at_frame", frame } };
		}
		if (action == "reset") {
			// Restore every feature to its defaults, then persist. Mirrors what the UI's
			// global reset does: per-feature RestoreDefaultSettings followed by a Save.
			task->AddTask([state]() {
				for (auto* f : Feature::GetFeatureList()) {
					try {
						f->RestoreDefaultSettings();
					} catch (const std::exception& e) {
						logger::error("DevBenchBridge: settings(reset) {} threw: {}", f->GetShortName(), e.what());
					} catch (...) {
						logger::error("DevBenchBridge: settings(reset) {} threw (unknown)", f->GetShortName());
					}
				}
				try {
					state->Save(State::ConfigMode::USER);
					logger::info("DevBenchBridge: settings(reset) applied");
				} catch (const std::exception& e) {
					logger::error("DevBenchBridge: settings(reset) save failed: {}", e.what());
				} catch (...) {
					logger::error("DevBenchBridge: settings(reset) save failed (unknown)");
				}
			});
			return json{ { "action", "reset" }, { "queued", true }, { "enqueued_at_frame", frame } };
		}
		return json{ { "error", "unknown action (save|load|reset)" }, { "action", action } };
	}

	/**
	 * @brief Processes DevBench settings tool requests (save, load, reset).
	 */
	json BuildUtilReResult(const json& a_args)
	{
		auto* replica = UtilityPassReplica::GetSingleton();
		const std::string action = a_args.value("action", std::string{ "get" });
		if (action == "get") {
			return json{
				{ "mode", static_cast<std::uint32_t>(replica->GetMode()) },
				{ "hooksInstalled", replica->HooksInstalled() },
			};
		}
		if (action == "set") {
			if (!a_args.contains("mode"))
				return json{ { "error", "missing required parameter 'mode'" } };
			const auto m = a_args["mode"].get<std::uint32_t>();
			if (m > 2)
				return json{ { "error", "mode must be 0 (off), 1 (compare) or 2 (replace)" } };
			if (!replica->SetMode(static_cast<UtilityPassReplica::Mode>(m)))
				return json{ { "error", "hooks not installed -- launch with CS_UTIL_RE_MODE set" } };
			return json{ { "action", "set" }, { "mode", m } };
		}
		// Rerunnable vanilla-parity validation gate: `reset` zeroes the compare counters so
		// a fresh run starts clean; `stats` reads back the structural-divergence report with
		// the first-diverging-pass pinpoint (class + technique + call index + field). A gate
		// script (tools/shadow-parity.ps1) does: set mode 1 -> reset -> run N frames in
		// motion -> stats -> assert diverged==0, and on failure the pinpoint names the pass.
		if (action == "reset") {
			replica->ResetValidation();
			return json{ { "action", "reset" }, { "mode", static_cast<std::uint32_t>(replica->GetMode()) } };
		}
		if (action == "stats") {
			const auto r = replica->GetValidationReport();
			static constexpr const char* kClassNames[3] = { "trishape", "subindex", "skinned" };
			static constexpr const char* kFieldNames[5] = { "kind", "slot", "a", "b", "c" };
			json out{
				{ "mode", static_cast<std::uint32_t>(replica->GetMode()) },
				{ "compared", r.compared },
				{ "diverged", r.diverged },
				{ "divergedTrishape", r.divergedTrishape },
				{ "divergedSubIndex", r.divergedSubIndex },
				{ "divergedSkinned", r.divergedSkinned },
				{ "unsupported", r.unsupported },
				{ "parity", r.diverged == 0 },
			};
			if (r.haveFirstDiverge) {
				out["firstDivergingPass"] = json{
					{ "class", r.firstClass < 3 ? kClassNames[r.firstClass] : "?" },
					{ "technique", fmt::format("0x{:X}", r.firstTechnique) },
					{ "engineCalls", r.firstEngineCalls },
					{ "replicaCalls", r.firstReplicaCalls },
					{ "sizeMismatch", r.firstSizeMismatch },
					{ "diffIndex", r.firstDiffIndex },
					{ "diffField", r.firstDiffField < 5 ? kFieldNames[r.firstDiffField] : "?" },
				};
			}
			return out;
		}
		return json{ { "error", "unknown action" }, { "supported", json::array({ "get", "set", "reset", "stats" }) } };
	}

	void UtilReToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildUtilReResult, a_argsJson, a_sink, a_write);
	}

	json BuildShadowDeferredResult(const json& a_args)
	{
		auto* sd = ShadowDeferred::GetSingleton();
		const std::string action = a_args.value("action", std::string{ "get" });
		if (action == "get") {
			return json{
				{ "mode", static_cast<std::uint32_t>(sd->GetMode()) },
				{ "hooksInstalled", sd->HooksInstalled() },
			};
		}
		if (action == "set") {
			if (!a_args.contains("mode"))
				return json{ { "error", "missing required parameter 'mode'" } };
			const auto m = a_args["mode"].get<std::uint32_t>();
			if (m > 1)
				return json{ { "error", "mode must be 0 (engine) or 1 (deferred)" } };
			if (!sd->SetMode(static_cast<ShadowDeferred::Mode>(m)))
				return json{ { "error", "hooks not installed -- launch with CS_SHADOW_DEFERRED=1" } };
			return json{ { "action", "set" }, { "mode", m } };
		}
		return json{ { "error", "unknown action" }, { "supported", json::array({ "get", "set" }) } };
	}

	void ShadowDeferredToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildShadowDeferredResult, a_argsJson, a_sink, a_write);
	}

	json BuildShadowMtResult(const json& a_args)
	{
		auto* st = ShadowThreaded::GetSingleton();
		const std::string action = a_args.value("action", std::string{ "get" });
		if (action == "get") {
			return json{
				{ "mode", static_cast<std::uint32_t>(st->GetMode()) },
				{ "hooksInstalled", st->HooksInstalled() },
			};
		}
		if (action == "set") {
			if (!a_args.contains("mode"))
				return json{ { "error", "missing required parameter 'mode'" } };
			const auto m = a_args["mode"].get<std::uint32_t>();
			if (m > 9)
				return json{ { "error", "mode must be 0..9" } };
			if (!st->SetMode(static_cast<ShadowThreaded::Mode>(m)))
				return json{ { "error", "hooks not installed -- launch with CS_SHADOW_MT>=1" } };
			return json{ { "action", "set" }, { "mode", m } };
		}
		// Post-RenderShadowmaps state validation (leak detector). `stateval` with optional
		// {"enable":true|false} arms/disarms it and always returns the counters. Protocol: arm, hold
		// mode 0 for >=8 frames (baseline learn), switch to the replica mode, read back -- nonzero
		// divergences/canaryHits = our path leaks state into the rest of the frame.
		if (action == "stateval") {
			if (a_args.contains("enable"))
				st->stateValidationRequested.store(a_args["enable"].get<bool>(), std::memory_order_relaxed);
			if (a_args.contains("selftest"))
				st->stateValSelftest.store(a_args["selftest"].get<bool>(), std::memory_order_relaxed);
			const auto r = st->StateValReport();
			return json{
				{ "action", "stateval" },
				{ "enabled", st->stateValidationRequested.load(std::memory_order_relaxed) },
				{ "selftest", st->stateValSelftest.load(std::memory_order_relaxed) },
				{ "baselineFrames", r[0] },
				{ "checkedFrames", r[1] },
				{ "divergences", r[2] },
				{ "canaryHits", r[3] },
			};
		}
		// Instanced-path command-validation counters (always-on): pass-conservation invariants +
		// the F16C-pack-vs-engine-reference compare. Nonzero violations/mismatches = broken submission.
		if (action == "instval") {
			const auto r = UtilityPassReplica::GetSingleton()->InstValReport();
			return json{
				{ "action", "instval" },
				{ "mapsValidated", r[0] },
				{ "invariantViolations", r[1] },
				{ "packChecks", r[2] },
				{ "packMismatches", r[3] },
			};
		}
		return json{ { "error", "unknown action" }, { "supported", json::array({ "get", "set", "stateval", "instval" }) } };
	}

	void ShadowMtToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildShadowMtResult, a_argsJson, a_sink, a_write);
	}

	void SettingsToolHandler(void*, const char* a_argsJson, void* a_sink, DevBenchAPI::WriteFn a_write)
	{
		RunHandler(&BuildSettingsResult, a_argsJson, a_sink, a_write);
	}
}

namespace DevBenchBridge
{
	/**
	 * @brief Registers Community Shaders tools with the DevBench API if available.
	 *
	 * Registers tools for feature management, engine state inspection, shader cache control,
	 * frame capture, and settings persistence, provided the DevBench interface is available.
	 */
	void Install()
	{
		auto* dvb = DevBenchAPI::GetDevBenchInterface001();
		if (!dvb) {
			logger::info("DevBenchBridge: devbench not present; CS tools not registered");
			return;
		}
		logger::info("DevBenchBridge: devbench build {} present — registering CS tools", dvb->GetBuildNumber());

		// Namespaced tool names — devbench's registry is shared across plugins, so bare
		// names ("feature", "inspect"…) could collide with devbench's own or another mod's.
		// Descriptors preserve the actions/kinds/inputSchema of CS's former embedded server
		// so existing MCP clients keep working under the new prefix.

		static constexpr const char* featureDesc =
			R"({"description":"All Community Shaders graphics-feature operations — enumerate, inspect settings, mutate settings, restore defaults, toggle on/off. Action-dispatched. list: returns an array of {name,shortName,loaded,version,category,isCore,inMenu}; features with restart-gated settings also include restartFields:[{key,label,pending}]. get: params shortName, returns the SaveSettings blob (null if the feature has no override; set/reset then no-op). set: params shortName, settings (object). reset: params shortName, calls RestoreDefaultSettings. toggle: params shortName, enabled (boolean, OPTIONAL — omit to flip the current loaded state); flips Feature::loaded.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["list","get","set","reset","toggle"]},"shortName":{"type":"string"},"settings":{"type":"object"},"enabled":{"type":"boolean"}}}})";
		dvb->RegisterTool("communityshaders.feature", featureDesc, &FeatureToolHandler, nullptr);

		static constexpr const char* inspectDesc =
			R"({"description":"Read non-feature Community Shaders engine state. Kind-dispatched; response is a JSON object. kind=state -> {plugin,frame_count}. kind=shadercache -> {compiling,completedTasks,totalTasks,failedTasks,currentFailedCount,frame_count}. kind=profiler -> {totalGpuMs,totalCpuMs,frame_count,passes:[{name,gpuMs,gpuAvgMs,gpuP95Ms,gpuP99Ms,cpuMs,cpuAvgMs,gpuHistory:[...]}]}; optional filter param to match pass names.","readOnly":true,"inputSchema":{"type":"object","properties":{"kind":{"type":"string","enum":["state","shadercache","profiler"]},"filter":{"type":"string"}},"required":["kind"]}})";
		dvb->RegisterTool("communityshaders.inspect", inspectDesc, &InspectToolHandler, nullptr);

		static constexpr const char* shadercacheDesc =
			R"({"description":"Manage Community Shaders' compiled shader cache. Action-dispatched, fire-and-forget on the main thread. clear: drop the IN-MEMORY cache only; with the disk cache enabled shaders reload from Data/ShaderCache rather than recompiling, so this does NOT guarantee a recompile. deleteDisk: delete the on-disk cache AND drop the in-memory cache, forcing a full cold recompile (use this for compile benchmarks). Watch progress via communityshaders.inspect kind=shadercache and the communityshaders.shaderRecompiled event. Read-only status is communityshaders.inspect kind=shadercache.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["clear","deleteDisk"]}},"required":["action"]}})";
		dvb->RegisterTool("communityshaders.shadercache", shadercacheDesc, &ShadercacheToolHandler, nullptr);

		static constexpr const char* captureDesc =
			R"({"description":"Trigger a frame capture on the next render. Kind-dispatched. kind=renderdoc: RenderDoc multi-frame capture via the in-app API, honors frames (1-120, default 1); RenderDoc must be attached/loaded (check communityshaders.feature list for RenderDoc.loaded). kind=screenshot: lossless screenshot via the Screenshot feature; frames is ignored. Fire-and-forget — no artifact path is returned synchronously.","inputSchema":{"type":"object","properties":{"kind":{"type":"string","enum":["renderdoc","screenshot"]},"frames":{"type":"number"}},"required":["kind"]}})";
		dvb->RegisterTool("communityshaders.capture", captureDesc, &CaptureToolHandler, nullptr);

		static constexpr const char* settingsDesc =
			R"({"description":"Save, load, or reset the GLOBAL Community Shaders user configuration (Data/SKSE/Plugins/CommunityShaders/*.json). Action-dispatched, all fire-and-forget on the main thread. save: persist current settings (State::Save). load: re-read settings from disk and apply (State::Load). reset: restore every feature to its defaults then persist. Use after communityshaders.feature set/reset to make changes durable, or to roll an A/B session back to the saved baseline.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["save","load","reset"]}},"required":["action"]}})";
		dvb->RegisterTool("communityshaders.settings", settingsDesc, &SettingsToolHandler, nullptr);

		static constexpr const char* utilreDesc =
			R"({"description":"Runtime control + rerunnable vanilla-parity validation for the UtilityPassReplica (the utility passes ARE the shadow passes). Requires CS_UTIL_RE_MODE set at boot. get: { mode, hooksInstalled }. set: params mode 0|1|2 (0=off/engine, 1=compare/double-render+diff, 2=replace/replica) -- next-pass, safe mid-session. reset: zero the compare counters for a fresh run. stats: structural-divergence report { compared, diverged, divergedTrishape/SubIndex/Skinned, unsupported, parity, firstDivergingPass{class,technique,diffIndex,diffField,...} } -- the gate reads parity==true. Validation flow: set mode 1 -> reset -> advance N frames in motion -> stats.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["get","set","reset","stats"]},"mode":{"type":"integer"}}}})";
		dvb->RegisterTool("communityshaders.utilre", utilreDesc, &UtilReToolHandler, nullptr);

		static constexpr const char* shadowDeferredDesc =
			R"({"description":"Runtime mode switch for ShadowDeferred (runs the engine's shadow-map commands on a private deferred context). Requires launching with CS_SHADOW_DEFERRED=1 (detour installs at boot). get: returns { mode, hooksInstalled }. set: params mode 0|1 (0=engine renders shadows, 1=deferred replica) -- takes effect next frame, safe mid-session; used for single-session engine-vs-deferred screenshot A/B.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["get","set"]},"mode":{"type":"integer"}}}})";
		dvb->RegisterTool("communityshaders.shadowdeferred", shadowDeferredDesc, &ShadowDeferredToolHandler, nullptr);

		static constexpr const char* shadowMtDesc =
			R"({"description":"Runtime mode switch for ShadowThreaded (shadow-map reimplementation / MT). Requires launching with CS_SHADOW_MT>=1 (detour installs at boot). get: returns { mode, hooksInstalled }. set: params mode 0..9 (0=engine renders shadows = VANILLA, 5=BeginPassReplica full ownership, 7=draw-state MT verify, 9=shadow instancing) -- takes effect next frame, safe mid-session; used for single-session engine-vs-replica screenshot A/B.","inputSchema":{"type":"object","properties":{"action":{"type":"string","enum":["get","set"]},"mode":{"type":"integer"}}}})";
		dvb->RegisterTool("communityshaders.shadowmt", shadowMtDesc, &ShadowMtToolHandler, nullptr);
	}
}

#else

namespace DevBenchBridge
{
	/**
 * @brief Registers Community Shaders tools with the DevBench bridge if available.
 *
 * When the DevBench interface is present, registers tool handlers for feature management,
 * inspection, shader caching, capture operations, and settings persistence.
 * If DevBench is unavailable, no action is taken.
 */
void Install() {}  // inert until built with DEVBENCH_BRIDGE_ENABLED
}

#endif
