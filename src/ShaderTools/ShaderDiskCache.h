#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ShaderDiskCache
{
	inline constexpr std::uint32_t SCHEMA_VERSION = 3;

	struct LookupResult
	{
		std::filesystem::path path;
		std::string objectSHA256;
		std::uint64_t byteLength = 0;
	};

	/**
	 * Returns the per-user cache root. An empty path means that LocalAppData
	 * could not be resolved and disk caching is disabled for this process.
	 */
	[[nodiscard]] const std::filesystem::path& GetRoot();

	/** Builds the canonical reference path for a lowercase, full SHA-256 ID. */
	[[nodiscard]] std::optional<std::filesystem::path> GetRecipePath(std::string_view a_recipeSHA256);

	/** Builds the canonical object path for a lowercase, full SHA-256 ID. */
	[[nodiscard]] std::optional<std::filesystem::path> GetObjectPath(std::string_view a_objectSHA256);

	/**
	 * Resolves and fully validates a recipe reference and its immutable DXBC
	 * object. Invalid, truncated, or corrupt entries are treated as misses.
	 */
	[[nodiscard]] std::optional<LookupResult> Lookup(std::string_view a_recipeSHA256);

	/**
	 * Publishes DXBC bytes under a recipe ID. Objects and references are
	 * installed atomically without replacing valid entries.
	 */
	[[nodiscard]] bool Publish(
		std::string_view a_recipeSHA256,
		std::span<const std::uint8_t> a_bytecode);

	/** Removes a recipe and deletes its object only after the final reference is gone. */
	[[nodiscard]] bool RemoveRecipe(std::string_view a_recipeSHA256);

	/**
	 * Removes a recipe only when it still names the object returned by an earlier
	 * lookup. A concurrently repaired or republished reference is left intact.
	 */
	[[nodiscard]] bool RemoveRecipeIfMatches(
		std::string_view a_recipeSHA256,
		std::string_view a_expectedObjectSHA256,
		std::uint64_t a_expectedByteLength);

	/** Batch form of RemoveRecipe; IDs are validated and deduplicated before mutation. */
	[[nodiscard]] bool RemoveRecipes(std::span<const std::string> a_recipeSHA256s);

	/** Removes invalid references, abandoned temporary files, and orphan objects. */
	[[nodiscard]] bool Prune();

	/** Removes the complete v3 cache for the current user. */
	[[nodiscard]] bool Clear();
}
