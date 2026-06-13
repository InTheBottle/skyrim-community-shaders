#pragma once

#include <cstddef>

namespace Util::Settings
{
	// Type-erased field descriptor for restart-gated settings.
	//
	// `jsonKey` must match the NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE field name so
	// MCP/RemoteControl can refer to it without per-feature glue.
	struct RestartFieldInfo
	{
		const char* jsonKey = nullptr;
		const char* label = nullptr;
		size_t offset = 0;
		size_t size = 0;
	};
}
