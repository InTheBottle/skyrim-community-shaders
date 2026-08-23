#pragma once

#include <REL/REL.h>
#include <SKSE/Version.h>

namespace Util::VersionedRelocation
{
	/**
	 * Select a value for SE, pre-1.7 AE, 1.7.99-or-newer AE, or VR.
	 *
	 * CommonLib's three-way relocation helpers distinguish SE, AE, and VR, but
	 * Skyrim 1.7.99 also moved a number of call sites within otherwise stable AE
	 * functions. Keeping the extra version split here avoids treating every AE
	 * executable as though it used the 1.7 layout.
	 */
	template <class T>
	[[nodiscard]] T Select(T a_se, T a_ae, T a_ae1799, T a_vr) noexcept
	{
		if (REL::Module::IsAE()) {
			return REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_7_99) ? a_ae1799 : a_ae;
		}
		return REL::Module::IsVR() ? a_vr : a_se;
	}

	template <class T>
	[[nodiscard]] T Select(T a_seAndVR, T a_ae, T a_ae1799) noexcept
	{
		return Select(a_seAndVR, a_ae, a_ae1799, a_seAndVR);
	}

	/** Resolve an Address Library ID whose numeric ID changed in Skyrim 1.7.99. */
	[[nodiscard]] inline std::uintptr_t ResolveID(
		std::uint64_t a_se,
		std::uint64_t a_ae,
		std::uint64_t a_ae1799,
		std::uint64_t a_vr)
	{
		if (REL::Module::IsAE() && REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_7_99)) {
			return REL::ID(a_ae1799).address();
		}
		return REL::RelocationID(a_se, a_ae, a_vr).address();
	}

	[[nodiscard]] inline std::uintptr_t ResolveID(
		std::uint64_t a_seAndVR,
		std::uint64_t a_ae,
		std::uint64_t a_ae1799)
	{
		return ResolveID(a_seAndVR, a_ae, a_ae1799, a_seAndVR);
	}
}
