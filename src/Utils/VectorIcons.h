#pragma once

#include <imgui.h>

/**
 * @brief Resolution-independent line-art icons drawn straight into an ImDrawList.
 *
 * The icons carry no colour of their own: callers pass the tint, so they always
 * match the active theme. Nothing is loaded from disk, so there are no icon
 * assets to ship and no textures to keep alive.
 */
namespace Util::Icons
{
	/** @brief Which icon to draw. */
	enum class Kind
	{
		None,
		Save,     /**< @brief Floppy disk — write settings to disk. */
		Reload,   /**< @brief Circular arrow — re-read settings from disk. */
		Refresh,  /**< @brief Cycle arrows in a ring — rebuild/clear a cache. */
	};

	/** @brief Bottle logo width divided by its height. */
	inline constexpr float kBottleAspect = 0.3689f;

	/**
	 * @brief Draws the bottle logo as line art.
	 * @param drawList Target draw list.
	 * @param topLeft Top-left corner of the logo's bounding box.
	 * @param height Logo height in pixels; width follows from kBottleAspect.
	 * @param color Stroke colour.
	 * @param thickness Stroke width; <= 0 picks a width proportional to height.
	 */
	void DrawBottle(ImDrawList* drawList, const ImVec2& topLeft, float height, ImU32 color, float thickness = 0.0f);

	/**
	 * @brief Draws one of the line-art action icons inside a square box.
	 * @param drawList Target draw list.
	 * @param topLeft Top-left corner of the square.
	 * @param size Edge length of the square in pixels.
	 * @param color Stroke colour.
	 * @param thickness Stroke width; <= 0 picks a width proportional to size.
	 */
	void Draw(ImDrawList* drawList, Kind kind, const ImVec2& topLeft, float size, ImU32 color, float thickness = 0.0f);

	/**
	 * @brief A standard button that shows a line-art icon followed by its label.
	 *
	 * The icon and text are drawn as one centred group, so the button behaves
	 * like any other ImGui button (including a width of -1 to fill the column).
	 *
	 * @param label Button text; also supplies the ImGui ID.
	 * @param kind Icon drawn to the left of the text.
	 * @param size Button size, following ImGui::Button conventions.
	 * @return true on the frame the button is clicked.
	 */
	bool Button(const char* label, Kind kind, const ImVec2& size = ImVec2(0, 0));

	/** @brief Icon button variant that flashes on click, matching Util::ButtonWithFlash. */
	bool ButtonWithFlash(const char* label, Kind kind, const ImVec2& size = ImVec2(0, 0), int flashDurationMs = 200);
}
