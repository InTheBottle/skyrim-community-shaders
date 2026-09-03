#pragma once

/**
 * @brief Renders the compact text-only menu header: title and global action buttons.
 *
 * The header carries no imagery. When the window is docked the title bar already
 * names the window, so only the action row is drawn.
 */
class MenuHeaderRenderer
{
public:
	/**
	 * @brief Renders the menu header area.
	 *
	 * @param isDocked True if the menu window is docked into a tab bar.
	 */
	static void RenderHeader(bool isDocked);
};
