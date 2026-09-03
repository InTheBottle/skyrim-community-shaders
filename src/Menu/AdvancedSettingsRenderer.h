#pragma once

#include <functional>
#include <string>

// Forward declaration
class Menu;

/**
 * @brief Renders the Advanced Settings page of the in-game menu.
 *
 * Provides tabbed sections for developer tools, shader debugging, logging
 * configuration, disable-at-boot toggles, profiling statistics, A/B testing
 * controls, and one sub-tab per Utility-category feature.
 */
class AdvancedSettingsRenderer
{
public:
	/**
	 * @brief Renders the full Advanced Settings tab bar with all sub-sections.
	 *
	 * Draws a tab bar containing Developer, Disable at Boot, Logging, Shader
	 * Debug, Profiling and Testing tabs, followed by one tab per utility feature.
	 *
	 * @param drawDisableAtBootSettings Callback that renders the per-feature
	 *        disable-at-boot checkboxes (provided by the Menu class).
	 */
	static void RenderAdvancedSettings(
		const std::function<void()>& drawDisableAtBootSettings);

	/**
	 * @brief Queues a utility feature's sub-tab to be activated on the next Advanced render.
	 *
	 * Utility features have no entry in the feature list, so navigation requests aimed at
	 * them (for example the CSEditor link in Wetness Effects) are routed here.
	 *
	 * @param featureShortName Short name of the utility feature to focus.
	 */
	static void RequestUtilityTab(const std::string& featureShortName);

private:
	static void RenderUtilityFeatureTabs();
	static void RenderLoggingSection();
	static void RenderShaderDebugSection();
	static void RenderDisableAtBootSection(const std::function<void()>& drawDisableAtBootSettings);
	static void RenderDeveloperSection();
	static void RenderTestingSection();
};