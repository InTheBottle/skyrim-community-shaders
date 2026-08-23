#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ShaderStorage
{
	inline constexpr std::uint32_t DUMP_SCHEMA_VERSION = 2;
	inline constexpr std::size_t EXECUTABLE_PATH_ID_HEX_LENGTH = 24;
	inline constexpr std::size_t DUMP_SESSION_ID_HEX_LENGTH = 24;
	inline constexpr std::size_t LOADER_READABLE_PREFIX_LENGTH = 12;
	inline constexpr std::size_t LOADER_PATH_ID_HEX_LENGTH = 24;
	inline constexpr std::size_t BYTECODE_PATH_ID_HEX_LENGTH = 24;

	struct RuntimeIdentity
	{
		std::string runtimeVersion;
		std::string executableSHA256;
	};

	[[nodiscard]] std::string SanitizePathComponent(std::string_view a_component);

	[[nodiscard]] std::filesystem::path BuildRuntimeRoot(
		const std::filesystem::path& a_base,
		const RuntimeIdentity& a_identity);
	[[nodiscard]] std::optional<std::filesystem::path> BuildDumpPath(
		const std::filesystem::path& a_dumpRoot,
		std::string_view a_loader,
		std::uint32_t a_descriptor,
		std::string_view a_stage,
		std::string_view a_bytecodeSHA256);

	[[nodiscard]] const RuntimeIdentity& GetRuntimeIdentity();
	[[nodiscard]] const std::string& GetDumpSessionID();
	[[nodiscard]] const std::filesystem::path& GetRuntimeDumpRoot();

	[[nodiscard]] std::optional<std::string> SHA256(std::span<const std::uint8_t> a_data);
	[[nodiscard]] std::optional<std::string> SHA256File(const std::filesystem::path& a_path);
}
