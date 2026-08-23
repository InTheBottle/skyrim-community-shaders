#include "RuntimeShaderStorage.h"

#include <REL/REL.h>

#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <format>
#include <fstream>
#include <limits>
#include <memory>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace ShaderStorage
{
	namespace
	{
		[[nodiscard]] std::string BytesToHex(std::span<const std::uint8_t> a_data)
		{
			static constexpr char HEX[] = "0123456789abcdef";
			std::string result;
			result.reserve(a_data.size() * 2);
			for (const auto byte : a_data) {
				result.push_back(HEX[byte >> 4]);
				result.push_back(HEX[byte & 0x0F]);
			}
			return result;
		}

		[[nodiscard]] std::string MakeUniquenessToken()
		{
			std::array<std::uint8_t, 16> randomBytes{};
			if (BCryptGenRandom(
					nullptr,
					randomBytes.data(),
					static_cast<ULONG>(randomBytes.size()),
					BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0) {
				return BytesToHex(randomBytes);
			}

			const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::system_clock::now().time_since_epoch());
			return std::format(
				"{:016x}{:08x}{:08x}",
				static_cast<std::uint64_t>(now.count()),
				GetCurrentProcessId(),
				GetCurrentThreadId());
		}

		[[nodiscard]] std::string BuildExecutablePathID(std::string_view a_executableIdentity)
		{
			static constexpr std::string_view UNAVAILABLE_PREFIX = "unavailable-";
			if (a_executableIdentity.starts_with(UNAVAILABLE_PREFIX)) {
				return "u-" + std::string(a_executableIdentity.substr(
								  UNAVAILABLE_PREFIX.size(),
								  EXECUTABLE_PATH_ID_HEX_LENGTH));
			}
			return std::string(a_executableIdentity.substr(0, EXECUTABLE_PATH_ID_HEX_LENGTH));
		}

		class SHA256Hasher
		{
		public:
			SHA256Hasher()
			{
				if (BCryptOpenAlgorithmProvider(std::addressof(algorithm), BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
					return;
				}

				DWORD bytesWritten = 0;
				DWORD objectLength = 0;
				if (BCryptGetProperty(
						algorithm,
						BCRYPT_OBJECT_LENGTH,
						reinterpret_cast<PUCHAR>(std::addressof(objectLength)),
						sizeof(objectLength),
						std::addressof(bytesWritten),
						0) < 0) {
					return;
				}

				if (BCryptGetProperty(
						algorithm,
						BCRYPT_HASH_LENGTH,
						reinterpret_cast<PUCHAR>(std::addressof(hashLength)),
						sizeof(hashLength),
						std::addressof(bytesWritten),
						0) < 0) {
					return;
				}

				hashObject.resize(objectLength);
				if (BCryptCreateHash(
						algorithm,
						std::addressof(hash),
						hashObject.data(),
						static_cast<ULONG>(hashObject.size()),
						nullptr,
						0,
						0) < 0) {
					return;
				}

				valid = true;
			}

			~SHA256Hasher()
			{
				if (hash) {
					BCryptDestroyHash(hash);
				}
				if (algorithm) {
					BCryptCloseAlgorithmProvider(algorithm, 0);
				}
			}

			SHA256Hasher(const SHA256Hasher&) = delete;
			SHA256Hasher& operator=(const SHA256Hasher&) = delete;

			[[nodiscard]] bool IsValid() const noexcept { return valid; }

			bool Update(std::span<const std::uint8_t> a_data)
			{
				if (!valid) {
					return false;
				}

				while (!a_data.empty()) {
					const auto chunkSize = static_cast<ULONG>(std::min<std::size_t>(
						a_data.size(),
						std::numeric_limits<ULONG>::max()));
					if (BCryptHashData(hash, const_cast<PUCHAR>(a_data.data()), chunkSize, 0) < 0) {
						valid = false;
						return false;
					}
					a_data = a_data.subspan(chunkSize);
				}
				return true;
			}

			[[nodiscard]] std::optional<std::string> Finish()
			{
				if (!valid) {
					return std::nullopt;
				}

				std::vector<std::uint8_t> digest(hashLength);
				if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
					return std::nullopt;
				}

				return BytesToHex(digest);
			}

		private:
			BCRYPT_ALG_HANDLE algorithm = nullptr;
			BCRYPT_HASH_HANDLE hash = nullptr;
			std::vector<std::uint8_t> hashObject;
			DWORD hashLength = 0;
			bool valid = false;
		};
	}

	std::string SanitizePathComponent(std::string_view a_component)
	{
		std::string result;
		result.reserve(a_component.size());
		for (const auto value : a_component) {
			const auto character = static_cast<unsigned char>(value);
			result.push_back(std::isalnum(character) || value == '-' || value == '_' || value == '.' ? value : '_');
		}

		if (result.empty() || result == "." || result == "..") {
			return "unknown";
		}
		return result;
	}

	std::filesystem::path BuildRuntimeRoot(const std::filesystem::path& a_base, const RuntimeIdentity& a_identity)
	{
		return a_base /
		       SanitizePathComponent(a_identity.runtimeVersion) /
		       BuildExecutablePathID(a_identity.executableSHA256);
	}

	std::optional<std::filesystem::path> BuildDumpPath(
		const std::filesystem::path& a_dumpRoot,
		std::string_view a_loader,
		std::uint32_t a_descriptor,
		std::string_view a_stage,
		std::string_view a_bytecodeSHA256)
	{
		const auto filename = std::format(
			"{:X}.{}.{}.bin",
			a_descriptor,
			SanitizePathComponent(a_stage),
			SanitizePathComponent(a_bytecodeSHA256.substr(0, BYTECODE_PATH_ID_HEX_LENGTH)));
		const auto loaderBytes = std::span(
			reinterpret_cast<const std::uint8_t*>(a_loader.data()),
			a_loader.size());
		const auto loaderHash = SHA256(loaderBytes);
		if (!loaderHash) {
			return std::nullopt;
		}

		auto readableLoader = SanitizePathComponent(a_loader);
		readableLoader.resize(std::min(readableLoader.size(), LOADER_READABLE_PREFIX_LENGTH));
		const auto loaderDirectory = std::format(
			"{}--{}",
			readableLoader,
			loaderHash->substr(0, LOADER_PATH_ID_HEX_LENGTH));
		return a_dumpRoot / loaderDirectory / filename;
	}

	const RuntimeIdentity& GetRuntimeIdentity()
	{
		static const RuntimeIdentity identity = [] {
			const auto& module = REL::Module::get();
			const auto executablePath = std::filesystem::path(module.filePath().data());
			const auto executableHash = SHA256File(executablePath);
			if (!executableHash) {
				logger::error("Failed to hash game executable {}; shader storage will use an unavailable identity marker", executablePath.string());
			}

			RuntimeIdentity result{
				.runtimeVersion = module.version().string("."),
				.executableSHA256 = executableHash ? *executableHash : "unavailable-" + MakeUniquenessToken()
			};
			// The per-process fallback deliberately prevents cache reuse when the
			// executable cannot be identified. This fails closed instead of risking
			// reuse of bytecode produced for a different executable.
			logger::info(
				"Shader storage identity: runtime {}, executable SHA-256 {}",
				result.runtimeVersion,
				result.executableSHA256);
			return result;
		}();
		return identity;
	}

	const std::string& GetDumpSessionID()
	{
		static const auto session = "s-" + MakeUniquenessToken().substr(0, DUMP_SESSION_ID_HEX_LENGTH);
		return session;
	}

	const std::filesystem::path& GetRuntimeDumpRoot()
	{
		static const auto path = BuildRuntimeRoot("Data/ShaderDump", GetRuntimeIdentity()) / GetDumpSessionID();
		return path;
	}

	std::optional<std::string> SHA256(std::span<const std::uint8_t> a_data)
	{
		SHA256Hasher hasher;
		if (!hasher.IsValid() || !hasher.Update(a_data)) {
			return std::nullopt;
		}
		return hasher.Finish();
	}

	std::optional<std::string> SHA256File(const std::filesystem::path& a_path)
	{
		std::ifstream stream(a_path, std::ios::binary);
		if (!stream) {
			return std::nullopt;
		}

		SHA256Hasher hasher;
		if (!hasher.IsValid()) {
			return std::nullopt;
		}

		std::vector<std::uint8_t> buffer(1024 * 1024);
		while (stream) {
			stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
			const auto bytesRead = stream.gcount();
			if (bytesRead > 0 && !hasher.Update(std::span(buffer.data(), static_cast<std::size_t>(bytesRead)))) {
				return std::nullopt;
			}
		}

		if (!stream.eof()) {
			return std::nullopt;
		}
		return hasher.Finish();
	}
}
