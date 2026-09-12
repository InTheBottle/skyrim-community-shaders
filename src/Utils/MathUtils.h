#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

namespace Util
{
	/** @brief Mixes a 32-bit value into a running 64-bit hash. */
	inline std::uint64_t HashCombine(std::uint64_t h, std::uint32_t v) noexcept
	{
		return h ^ (static_cast<std::uint64_t>(v) + 0x9e3779b9ull + (h << 6) + (h >> 2));
	}

	/** @brief Mixes a float into a running 64-bit hash via its bit pattern. */
	inline std::uint64_t HashCombineFloat(std::uint64_t h, float f) noexcept
	{
		return HashCombine(h, std::bit_cast<std::uint32_t>(f));
	}

	/** @brief Rounds a value to the nearest multiple of step, collapsing sub-step jitter before hashing. */
	inline float QuantizeFloat(float f, float step) noexcept
	{
		return std::round(f / step) * step;
	}
}
