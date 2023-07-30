#pragma once
#include "API/CSAPI.h"
#include <shared_mutex>

// https://github.com/Pentalimbed/cathub/blob/cathub-ng/src/menu.h
class CSInterface : public CSAPI
{
public:
    static CSInterface* GetSingleton()
    {
        static CSInterface face;
        return std::addressof(face);
    }

    inline virtual REL::Version GetVersion() override { return API_VER; }
    virtual ImGuiContext*       GetContext() override;
    virtual void                AddMenu(std::string name, std::function<void()> draw_func) override;

    std::shared_mutex mutex;

    struct Menu
	{
		std::string Name;
		std::function<void()> Function;
    };

	std::vector<Menu> menus;
};