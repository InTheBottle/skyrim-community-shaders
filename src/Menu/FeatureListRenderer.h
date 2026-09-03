#pragma once

#include <functional>
#include <map>
#include <string>
#include <variant>
#include <vector>

struct Feature;

/**
 * @brief Renders the two-column feature list and settings panel in the main menu.
 *
 * The left column shows a searchable, categorized list of built-in pages and
 * installed features. The right column displays the settings UI for whichever
 * item is currently selected.
 */
class FeatureListRenderer
{
public:
	/** @brief Describes a built-in (non-feature) menu page with a name and draw callback. */
	struct BuiltInMenu
	{
		std::string name;
		std::function<void()> func;
	};

	/** @brief Represents a collapsible category header in the feature list. */
	struct CategoryHeader
	{
		std::string name;
	};

	/** @brief Variant type representing any entry in the menu list. */
	using MenuFuncInfo = std::variant<BuiltInMenu, std::string, CategoryHeader, Feature*>;

	/**
	 * @brief Renders the full two-column feature list and settings panel.
	 *
	 * Builds the menu list from built-in pages and loaded features, handles
	 * pending feature selection requests, then draws the left-column navigation
	 * and right-column settings content.
	 *
	 * @param selectedMenu Index of the currently selected menu item (updated on selection change).
	 * @param featureSearch Current search filter string (updated by the search input).
	 * @param pendingFeatureSelection Name of a feature to auto-select (cleared after processing).
	 * @param categoryExpansionStates Map of category name to expanded/collapsed state.
	 * @param drawGeneralSettings Callback that renders the General settings page content.
	 * @param drawAdvancedSettings Callback that renders the Advanced settings page content.
	 */
	static void RenderFeatureList(
		size_t& selectedMenu,
		std::string& featureSearch,
		std::string& pendingFeatureSelection,
		std::map<std::string, bool>& categoryExpansionStates,
		const std::function<void()>& drawGeneralSettings,
		const std::function<void()>& drawAdvancedSettings);

	/**
	 * @brief Renders a single feature's settings page (header, action buttons and content).
	 *
	 * Used by the Advanced page to draw utility features as sub-tabs rather than as
	 * entries in the left-hand feature list.
	 *
	 * @param feat The feature whose page should be rendered.
	 */
	static void RenderFeaturePage(Feature* feat);

	/**
	 * @brief Returns the in-menu utility features, sorted by display name.
	 *
	 * These are excluded from the feature list and rendered as Advanced sub-tabs.
	 */
	static std::vector<Feature*> GetUtilityFeatures();

private:
	struct ListMenuVisitor
	{
		size_t listId;
		size_t& selectedMenuRef;
		std::map<std::string, bool>& categoryExpansionStates;

		void operator()(const BuiltInMenu& menu);
		void operator()(const std::string& label);
		void operator()(const CategoryHeader& header);
		void operator()(Feature* feat);

	private:
		/** @brief Marks this entry as selected, recording both its index and its stable key. */
		void Select(const std::string& entryKey);
	};

	struct DrawMenuVisitor
	{
		explicit DrawMenuVisitor(std::string& pendingFeatureSelectionRef) :
			pendingFeatureSelection(pendingFeatureSelectionRef) {}

		void operator()(const BuiltInMenu& menu);
		void operator()(const std::string&);
		void operator()(const CategoryHeader&);
		void operator()(Feature* feat);

	private:
		std::string& pendingFeatureSelection;

		// Helper methods for Feature rendering
		void RenderFeatureHeader(Feature* feat, bool isDisabled, bool isLoaded, bool sceneControlled);
		void RenderFeatureSettings(Feature* feat, bool isDisabled, bool isLoaded, bool hasFailedMessage, bool sceneControlled);
		void RenderReactiveConstraintWarningDialog();
	};

	static std::vector<MenuFuncInfo> BuildMenuList(
		const std::string& featureSearch,
		std::map<std::string, bool>& categoryExpansionStates,
		const std::function<void()>& drawGeneralSettings,
		const std::function<void()>& drawAdvancedSettings);

	/**
	 * @brief Re-derives the selected index from the stable selection key.
	 *
	 * The menu list is rebuilt every frame, so collapsing a category above the
	 * selection would otherwise shift the index onto a different entry.
	 */
	static void ResolveSelectionFromKey(
		const std::vector<MenuFuncInfo>& menuList,
		size_t& selectedMenu);

	static void HandlePendingFeatureSelection(
		std::string& pendingFeatureSelection,
		const std::vector<MenuFuncInfo>& menuList,
		size_t& selectedMenu);

	/** @brief True when the named feature belongs to the Utility category. */
	static bool IsUtilityFeature(const std::string& shortName);

	static void RenderLeftColumn(
		const std::vector<MenuFuncInfo>& menuList,
		size_t& selectedMenu,
		std::string& featureSearch,
		std::map<std::string, bool>& categoryExpansionStates);

	static void RenderRightColumn(
		const std::vector<MenuFuncInfo>& menuList,
		size_t selectedMenu,
		std::string& pendingFeatureSelection);
};