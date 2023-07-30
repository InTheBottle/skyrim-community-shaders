#include "API/CSInterface.h"

ImGuiContext* CSInterface::GetContext()
{
	return ImGui::GetCurrentContext();
}

void CSInterface::AddMenu(std::string name, std::function<void()> function)
{
	logger::info("Adding menu {}", name);

	std::lock_guard l{ mutex };
	menus.push_back({ name, function });
}
