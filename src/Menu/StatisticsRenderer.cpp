#include "StatisticsRenderer.h"

#include <imgui.h>

#include "Globals.h"
#include "State.h"

void StatisticsRenderer::RenderStatistics()
{
	auto& timers = (*globals::gpuTimers);

	float currentTime = static_cast<float>(ImGui::GetTime());
	float deltaTime = currentTime - lastFrameTime;
	lastFrameTime = currentTime;
	timeSinceLastUpdate += deltaTime;

	if (timeSinceLastUpdate >= 1.0f) {
		timeSinceLastUpdate = 0.0f;
		cachedTotalTimeMs = timers.GetTotalTimeMs();

		cachedGroups.clear();
		std::unordered_map<std::string, size_t> groupIndex;

		for (const auto& result : timers.GetResults()) {
			if (!result.valid)
				continue;

			auto pos = result.name.find("::");
			if (pos != std::string::npos) {
				std::string groupName = result.name.substr(0, pos);
				std::string passLabel = result.name.substr(pos + 2);

				auto it = groupIndex.find(groupName);
				if (it == groupIndex.end()) {
					groupIndex[groupName] = cachedGroups.size();
					cachedGroups.push_back({ groupName });
				}

				auto& group = cachedGroups[groupIndex[groupName]];
				group.totalMs += result.gpuTimeMs;
				group.totalAvgMs += result.avgMs;
				group.totalP95Ms += result.p95Ms;
				group.totalP99Ms += result.p99Ms;
				group.passes.push_back({ passLabel, result.gpuTimeMs, result.avgMs, result.p95Ms, result.p99Ms });
			} else {
				groupIndex[result.name] = cachedGroups.size();
				cachedGroups.push_back({ result.name, result.gpuTimeMs, result.avgMs, result.p95Ms, result.p99Ms, {} });
			}
		}
	}

	ImGui::Text("GPU Render Pass Timings");
	ImGui::Separator();

	if (cachedGroups.empty()) {
		ImGui::TextDisabled("No timing data available (enter game world)");
		return;
	}

	float availHeight = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();

	if (ImGui::BeginTable("##GPUTimers", 6,
			ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_ScrollY,
			ImVec2(0.0f, availHeight))) {
		ImGui::TableSetupScrollFreeze(0, 1);
		ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthFixed, 65.0f);
		ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthFixed, 55.0f);
		ImGui::TableSetupColumn("P95", ImGuiTableColumnFlags_WidthFixed, 55.0f);
		ImGui::TableSetupColumn("P99", ImGuiTableColumnFlags_WidthFixed, 55.0f);
		ImGui::TableSetupColumn("%%", ImGuiTableColumnFlags_WidthFixed, 45.0f);
		ImGui::TableHeadersRow();

		for (const auto& group : cachedGroups) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();

			if (group.passes.empty()) {
				ImGui::TreeNodeEx(group.name.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", group.totalMs);
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", group.totalAvgMs);
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", group.totalP95Ms);
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", group.totalP99Ms);
				ImGui::TableNextColumn();
				if (cachedTotalTimeMs > 0.0f)
					ImGui::Text("%5.1f", (group.totalMs / cachedTotalTimeMs) * 100.0f);
			} else {
				bool open = ImGui::TreeNodeEx(group.name.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", group.totalMs);
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", group.totalAvgMs);
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", group.totalP95Ms);
				ImGui::TableNextColumn();
				ImGui::Text("%.3f", group.totalP99Ms);
				ImGui::TableNextColumn();
				if (cachedTotalTimeMs > 0.0f)
					ImGui::Text("%5.1f", (group.totalMs / cachedTotalTimeMs) * 100.0f);
				if (open) {
					for (const auto& pass : group.passes) {
						ImGui::TableNextRow();
						ImGui::TableNextColumn();
						ImGui::TreeNodeEx(pass.label.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen);
						ImGui::TableNextColumn();
						ImGui::Text("%.3f", pass.ms);
						ImGui::TableNextColumn();
						ImGui::Text("%.3f", pass.avgMs);
						ImGui::TableNextColumn();
						ImGui::Text("%.3f", pass.p95Ms);
						ImGui::TableNextColumn();
						ImGui::Text("%.3f", pass.p99Ms);
						ImGui::TableNextColumn();
						if (cachedTotalTimeMs > 0.0f)
							ImGui::Text("%5.1f", (pass.ms / cachedTotalTimeMs) * 100.0f);
					}
					ImGui::TreePop();
				}
			}
		}
		ImGui::EndTable();
	}

	ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "Total: %.3f ms", cachedTotalTimeMs);
}

void StatisticsRenderer::RenderFeatureTimers(const std::string& featurePrefix)
{
	auto& timers = (*globals::gpuTimers);
	const auto& results = timers.GetResults();

	struct Entry
	{
		std::string label;
		float ms;
		float avgMs;
		float p95Ms;
		float p99Ms;
	};

	std::vector<Entry> entries;
	float totalMs = 0.0f;
	float totalAvg = 0.0f;
	float totalP95 = 0.0f;
	float totalP99 = 0.0f;

	std::string prefix = featurePrefix + "::";
	for (const auto& r : results) {
		if (!r.valid || !r.name.starts_with(prefix))
			continue;
		std::string label = r.name.substr(prefix.size());
		entries.push_back({ label, r.gpuTimeMs, r.avgMs, r.p95Ms, r.p99Ms });
		totalMs += r.gpuTimeMs;
		totalAvg += r.avgMs;
		totalP95 += r.p95Ms;
		totalP99 += r.p99Ms;
	}

	if (entries.empty()) {
		ImGui::TextDisabled("No GPU timing data");
		return;
	}

	if (ImGui::BeginTable("##FeatureTimers", 5, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
		ImGui::TableSetupColumn("Pass", ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("Current", ImGuiTableColumnFlags_WidthFixed, 60.0f);
		ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthFixed, 55.0f);
		ImGui::TableSetupColumn("P95", ImGuiTableColumnFlags_WidthFixed, 55.0f);
		ImGui::TableSetupColumn("P99", ImGuiTableColumnFlags_WidthFixed, 55.0f);
		ImGui::TableHeadersRow();

		for (const auto& e : entries) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::Text("%s", e.label.c_str());
			ImGui::TableNextColumn();
			ImGui::Text("%.3f", e.ms);
			ImGui::TableNextColumn();
			ImGui::Text("%.3f", e.avgMs);
			ImGui::TableNextColumn();
			ImGui::Text("%.3f", e.p95Ms);
			ImGui::TableNextColumn();
			ImGui::Text("%.3f", e.p99Ms);
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "Total");
		ImGui::TableNextColumn();
		ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "%.3f", totalMs);
		ImGui::TableNextColumn();
		ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "%.3f", totalAvg);
		ImGui::TableNextColumn();
		ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "%.3f", totalP95);
		ImGui::TableNextColumn();
		ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.6f, 1.0f), "%.3f", totalP99);

		ImGui::EndTable();
	}
}
