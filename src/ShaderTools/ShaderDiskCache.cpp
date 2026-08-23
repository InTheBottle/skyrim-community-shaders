#include "ShaderDiskCache.h"

#include "RuntimeShaderStorage.h"

#include <ShlObj.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "shell32.lib")

namespace ShaderDiskCache
{
	namespace
	{
		constexpr std::size_t SHA256_HEX_LENGTH = 64;
		constexpr std::size_t MAX_DECIMAL_UINT64_LENGTH = 20;
		constexpr std::size_t MAX_REFERENCE_LENGTH =
			SHA256_HEX_LENGTH + 1 + MAX_DECIMAL_UINT64_LENGTH + 1;
		constexpr std::array<std::uint8_t, 4> DXBC_MAGIC{ 'D', 'X', 'B', 'C' };

		std::shared_mutex ProcessMutex;
		std::atomic_uint64_t TemporaryFileSequence = 0;

		struct RecipeReference
		{
			std::string objectSHA256;
			std::uint64_t byteLength = 0;
		};

		enum class EntryState
		{
			Valid,
			Invalid,
			Error
		};

		struct ReferenceInspection
		{
			EntryState state = EntryState::Invalid;
			RecipeReference reference;
		};

		struct ObjectInspection
		{
			EntryState state = EntryState::Invalid;
			std::uint64_t byteLength = 0;
		};

		struct CacheInventory
		{
			std::vector<std::filesystem::path> files;
			std::vector<std::filesystem::path> directories;
		};

		class UniqueHandle
		{
		public:
			explicit UniqueHandle(HANDLE a_handle = INVALID_HANDLE_VALUE) noexcept :
				handle(a_handle)
			{}

			~UniqueHandle()
			{
				Reset();
			}

			UniqueHandle(const UniqueHandle&) = delete;
			UniqueHandle& operator=(const UniqueHandle&) = delete;

			UniqueHandle(UniqueHandle&& a_other) noexcept :
				handle(std::exchange(a_other.handle, INVALID_HANDLE_VALUE))
			{}

			UniqueHandle& operator=(UniqueHandle&& a_other) noexcept
			{
				if (this != std::addressof(a_other)) {
					Reset();
					handle = std::exchange(a_other.handle, INVALID_HANDLE_VALUE);
				}
				return *this;
			}

			[[nodiscard]] HANDLE Get() const noexcept { return handle; }
			[[nodiscard]] explicit operator bool() const noexcept
			{
				return handle != nullptr && handle != INVALID_HANDLE_VALUE;
			}

			void Reset(HANDLE a_handle = INVALID_HANDLE_VALUE) noexcept
			{
				if (*this) {
					CloseHandle(handle);
				}
				handle = a_handle;
			}

		private:
			HANDLE handle = INVALID_HANDLE_VALUE;
		};

		[[nodiscard]] const std::optional<std::wstring>& GetMutationMutexName()
		{
			static const std::optional<std::wstring> name = []() -> std::optional<std::wstring> {
				const auto& root = GetRoot();
				if (root.empty()) {
					return std::nullopt;
				}

				const auto& nativeRoot = root.native();
				if (nativeRoot.size() > (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t)) {
					return std::nullopt;
				}

				const auto rootBytes = std::span(
					reinterpret_cast<const std::uint8_t*>(nativeRoot.data()),
					nativeRoot.size() * sizeof(wchar_t));
				const auto rootSHA256 = ShaderStorage::SHA256(rootBytes);
				if (!rootSHA256) {
					return std::nullopt;
				}

				return L"Global\\CommunityShaders.ShaderCache.v3." +
				       std::wstring(rootSHA256->begin(), rootSHA256->end()) +
				       L".Mutation";
			}();
			return name;
		}

		class NamedMutexLock
		{
		public:
			~NamedMutexLock()
			{
				if (handle) {
					ReleaseMutex(handle);
					CloseHandle(handle);
				}
			}

			NamedMutexLock(const NamedMutexLock&) = delete;
			NamedMutexLock& operator=(const NamedMutexLock&) = delete;

			NamedMutexLock(NamedMutexLock&& a_other) noexcept :
				handle(std::exchange(a_other.handle, nullptr))
			{}

			NamedMutexLock& operator=(NamedMutexLock&&) = delete;

			[[nodiscard]] static std::optional<NamedMutexLock> Acquire()
			{
				const auto& name = GetMutationMutexName();
				if (!name) {
					return std::nullopt;
				}

				const auto handle = CreateMutexW(nullptr, FALSE, name->c_str());
				if (!handle) {
					return std::nullopt;
				}

				const auto waitResult = WaitForSingleObject(handle, INFINITE);
				if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
					CloseHandle(handle);
					return std::nullopt;
				}

				return NamedMutexLock(handle);
			}

		private:
			explicit NamedMutexLock(HANDLE a_handle) noexcept :
				handle(a_handle)
			{}

			HANDLE handle = nullptr;
		};

		[[nodiscard]] bool IsValidSHA256(std::string_view a_value) noexcept
		{
			if (a_value.size() != SHA256_HEX_LENGTH) {
				return false;
			}

			return std::ranges::all_of(a_value, [](const char a_character) {
				return (a_character >= '0' && a_character <= '9') ||
				       (a_character >= 'a' && a_character <= 'f');
			});
		}

		[[nodiscard]] bool HasDXBCMagic(std::span<const std::uint8_t> a_data) noexcept
		{
			return a_data.size() >= DXBC_MAGIC.size() &&
			       std::ranges::equal(DXBC_MAGIC, a_data.first(DXBC_MAGIC.size()));
		}

		[[nodiscard]] std::optional<std::filesystem::path> BuildContentPath(
			std::string_view a_category,
			std::string_view a_sha256,
			std::string_view a_extension)
		{
			if (GetRoot().empty() || !IsValidSHA256(a_sha256)) {
				return std::nullopt;
			}

			return GetRoot() /
			       std::filesystem::path(a_category) /
			       "sha256" /
			       std::string(a_sha256.substr(0, 2)) /
			       (std::string(a_sha256.substr(2)) + std::string(a_extension));
		}

		[[nodiscard]] bool IsMissing(const std::error_code& a_error) noexcept
		{
			return a_error == std::errc::no_such_file_or_directory;
		}

		[[nodiscard]] ReferenceInspection InspectReference(const std::filesystem::path& a_path)
		{
			std::error_code error;
			const auto status = std::filesystem::symlink_status(a_path, error);
			if (error) {
				return { .state = IsMissing(error) ? EntryState::Invalid : EntryState::Error };
			}
			if (!std::filesystem::is_regular_file(status)) {
				return { .state = EntryState::Invalid };
			}

			const auto fileSize = std::filesystem::file_size(a_path, error);
			if (error) {
				return { .state = IsMissing(error) ? EntryState::Invalid : EntryState::Error };
			}
			if (fileSize == 0 || fileSize > MAX_REFERENCE_LENGTH) {
				return { .state = EntryState::Invalid };
			}

			std::ifstream stream(a_path, std::ios::binary);
			if (!stream) {
				return { .state = EntryState::Error };
			}

			std::string contents(static_cast<std::size_t>(fileSize), '\0');
			stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
			if (!stream || contents.size() < SHA256_HEX_LENGTH + 3 ||
				contents[SHA256_HEX_LENGTH] != '\n' || contents.back() != '\n') {
				return { .state = EntryState::Invalid };
			}

			ReferenceInspection result{ .state = EntryState::Valid };
			result.reference.objectSHA256.assign(contents.data(), SHA256_HEX_LENGTH);
			if (!IsValidSHA256(result.reference.objectSHA256)) {
				return { .state = EntryState::Invalid };
			}

			const std::string_view decimalLength(
				contents.data() + SHA256_HEX_LENGTH + 1,
				contents.size() - SHA256_HEX_LENGTH - 2);
			if (decimalLength.empty() || (decimalLength.size() > 1 && decimalLength.front() == '0')) {
				return { .state = EntryState::Invalid };
			}

			const auto parseResult = std::from_chars(
				decimalLength.data(),
				decimalLength.data() + decimalLength.size(),
				result.reference.byteLength);
			if (parseResult.ec != std::errc{} ||
				parseResult.ptr != decimalLength.data() + decimalLength.size() ||
				result.reference.byteLength < DXBC_MAGIC.size()) {
				return { .state = EntryState::Invalid };
			}

			return result;
		}

		[[nodiscard]] std::optional<RecipeReference> ReadReference(const std::filesystem::path& a_path)
		{
			const auto inspection = InspectReference(a_path);
			return inspection.state == EntryState::Valid ?
			           std::optional<RecipeReference>(inspection.reference) :
			           std::nullopt;
		}

		[[nodiscard]] std::string SerializeReference(const RecipeReference& a_reference)
		{
			std::array<char, MAX_DECIMAL_UINT64_LENGTH> decimalBuffer{};
			const auto conversion = std::to_chars(
				decimalBuffer.data(),
				decimalBuffer.data() + decimalBuffer.size(),
				a_reference.byteLength);

			std::string result;
			result.reserve(SHA256_HEX_LENGTH + 1 + MAX_DECIMAL_UINT64_LENGTH + 1);
			result.append(a_reference.objectSHA256);
			result.push_back('\n');
			if (conversion.ec == std::errc{}) {
				result.append(decimalBuffer.data(), conversion.ptr);
			}
			result.push_back('\n');
			return result;
		}

		[[nodiscard]] ObjectInspection InspectObject(
			const std::filesystem::path& a_path,
			std::string_view a_expectedSHA256)
		{
			if (!IsValidSHA256(a_expectedSHA256)) {
				return { .state = EntryState::Invalid };
			}

			std::error_code error;
			const auto status = std::filesystem::symlink_status(a_path, error);
			if (error) {
				return { .state = IsMissing(error) ? EntryState::Invalid : EntryState::Error };
			}
			if (!std::filesystem::is_regular_file(status)) {
				return { .state = EntryState::Invalid };
			}

			const auto fileSize = std::filesystem::file_size(a_path, error);
			if (error) {
				return { .state = IsMissing(error) ? EntryState::Invalid : EntryState::Error };
			}
			if (fileSize < DXBC_MAGIC.size() ||
				fileSize > (std::numeric_limits<std::uint64_t>::max)()) {
				return { .state = EntryState::Invalid };
			}

			std::ifstream stream(a_path, std::ios::binary);
			if (!stream) {
				return { .state = EntryState::Error };
			}

			std::array<std::uint8_t, DXBC_MAGIC.size()> magic{};
			stream.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
			if (!stream || !std::ranges::equal(magic, DXBC_MAGIC)) {
				return { .state = EntryState::Invalid };
			}

			const auto actualSHA256 = ShaderStorage::SHA256File(a_path);
			if (!actualSHA256) {
				return { .state = EntryState::Error };
			}
			if (*actualSHA256 != a_expectedSHA256) {
				return { .state = EntryState::Invalid };
			}

			return {
				.state = EntryState::Valid,
				.byteLength = static_cast<std::uint64_t>(fileSize)
			};
		}

		[[nodiscard]] bool ValidateObject(
			const std::filesystem::path& a_path,
			std::string_view a_expectedSHA256,
			std::uint64_t a_expectedByteLength)
		{
			const auto inspection = InspectObject(a_path, a_expectedSHA256);
			return inspection.state == EntryState::Valid &&
			       inspection.byteLength == a_expectedByteLength;
		}

		[[nodiscard]] bool ValidateRecipe(
			const std::filesystem::path& a_recipePath,
			std::string_view a_expectedObjectSHA256,
			std::uint64_t a_expectedByteLength)
		{
			const auto reference = ReadReference(a_recipePath);
			if (!reference ||
				reference->objectSHA256 != a_expectedObjectSHA256 ||
				reference->byteLength != a_expectedByteLength) {
				return false;
			}

			const auto objectPath = GetObjectPath(reference->objectSHA256);
			return objectPath && ValidateObject(*objectPath, reference->objectSHA256, reference->byteLength);
		}

		[[nodiscard]] bool EnsureParentDirectory(const std::filesystem::path& a_path)
		{
			std::error_code error;
			std::filesystem::create_directories(a_path.parent_path(), error);
			if (error) {
				return false;
			}

			return std::filesystem::is_directory(a_path.parent_path(), error) && !error;
		}

		[[nodiscard]] std::filesystem::path MakeTemporaryPath(const std::filesystem::path& a_destination)
		{
			auto filename = a_destination.filename().wstring();
			filename.append(L".tmp.");
			filename.append(std::to_wstring(GetCurrentProcessId()));
			filename.push_back(L'.');
			filename.append(std::to_wstring(GetCurrentThreadId()));
			filename.push_back(L'.');
			filename.append(std::to_wstring(TemporaryFileSequence.fetch_add(1, std::memory_order_relaxed)));
			return a_destination.parent_path() / filename;
		}

		[[nodiscard]] bool WriteAll(HANDLE a_file, std::span<const std::uint8_t> a_data) noexcept
		{
			while (!a_data.empty()) {
				const auto chunkSize = static_cast<DWORD>((std::min)(a_data.size(),
					static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
				DWORD bytesWritten = 0;
				if (!WriteFile(a_file, a_data.data(), chunkSize, std::addressof(bytesWritten), nullptr) ||
					bytesWritten != chunkSize) {
					return false;
				}
				a_data = a_data.subspan(bytesWritten);
			}
			return true;
		}

		[[nodiscard]] bool AtomicInstall(
			const std::filesystem::path& a_destination,
			std::span<const std::uint8_t> a_contents)
		{
			if (!EnsureParentDirectory(a_destination)) {
				return false;
			}

			for (std::size_t attempt = 0; attempt < 16; ++attempt) {
				const auto temporaryPath = MakeTemporaryPath(a_destination);
				UniqueHandle file(CreateFileW(
					temporaryPath.c_str(),
					GENERIC_WRITE,
					0,
					nullptr,
					CREATE_NEW,
					FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH,
					nullptr));
				if (!file) {
					const auto createError = GetLastError();
					if (createError == ERROR_FILE_EXISTS || createError == ERROR_ALREADY_EXISTS) {
						continue;
					}
					return false;
				}

				const auto wroteContents = WriteAll(file.Get(), a_contents) && FlushFileBuffers(file.Get());
				file.Reset();
				if (!wroteContents) {
					DeleteFileW(temporaryPath.c_str());
					return false;
				}

				if (MoveFileExW(temporaryPath.c_str(), a_destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
					return true;
				}

				DeleteFileW(temporaryPath.c_str());
				return false;
			}

			return false;
		}

		[[nodiscard]] bool DeleteFileIfPresent(const std::filesystem::path& a_path) noexcept
		{
			if (DeleteFileW(a_path.c_str())) {
				return true;
			}

			const auto error = GetLastError();
			return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
		}

		[[nodiscard]] std::optional<std::string> ExtractContentID(
			const std::filesystem::path& a_path,
			std::string_view a_category,
			std::string_view a_extension)
		{
			const auto categoryRoot = GetRoot() / std::filesystem::path(a_category) / "sha256";
			if (a_path.parent_path().parent_path().lexically_normal() != categoryRoot.lexically_normal()) {
				return std::nullopt;
			}

			const auto shard = a_path.parent_path().filename().string();
			const auto filename = a_path.filename().string();
			if (shard.size() != 2 ||
				filename.size() != SHA256_HEX_LENGTH - 2 + a_extension.size() ||
				!std::string_view(filename).ends_with(a_extension)) {
				return std::nullopt;
			}

			auto contentID = shard + filename.substr(0, SHA256_HEX_LENGTH - 2);
			return IsValidSHA256(contentID) ?
			           std::optional<std::string>(std::move(contentID)) :
			           std::nullopt;
		}

		[[nodiscard]] std::optional<CacheInventory> InventoryTree(const std::filesystem::path& a_root)
		{
			CacheInventory result;
			std::error_code error;
			const auto rootStatus = std::filesystem::symlink_status(a_root, error);
			if (error) {
				return IsMissing(error) ? std::optional<CacheInventory>(std::move(result)) : std::nullopt;
			}
			if (rootStatus.type() == std::filesystem::file_type::not_found) {
				return result;
			}
			if (!std::filesystem::is_directory(rootStatus)) {
				return std::nullopt;
			}

			std::filesystem::recursive_directory_iterator iterator(a_root, error);
			const std::filesystem::recursive_directory_iterator end;
			if (error) {
				return std::nullopt;
			}

			while (iterator != end) {
				const auto path = iterator->path();
				const auto status = iterator->symlink_status(error);
				if (error) {
					return std::nullopt;
				}

				if (std::filesystem::is_regular_file(status)) {
					result.files.push_back(path);
				} else if (std::filesystem::is_directory(status)) {
					result.directories.push_back(path);
				}

				iterator.increment(error);
				if (error) {
					return std::nullopt;
				}
			}

			return result;
		}

		[[nodiscard]] bool IsTemporaryFile(const std::filesystem::path& a_path)
		{
			return a_path.filename().wstring().find(L".tmp.") != std::wstring::npos;
		}

		struct ParsedRecipeEntry
		{
			RecipeReference reference;
		};

		[[nodiscard]] bool RemoveRecipesUnlocked(const std::unordered_set<std::string>& a_recipeSHA256s)
		{
			const auto inventory = InventoryTree(GetRoot() / "recipes");
			if (!inventory) {
				return false;
			}

			std::vector<ParsedRecipeEntry> parsedRecipes;
			std::unordered_map<std::string, std::size_t> recipeIndices;
			parsedRecipes.reserve(inventory->files.size());
			recipeIndices.reserve(inventory->files.size());
			for (const auto& path : inventory->files) {
				const auto recipeID = ExtractContentID(path, "recipes", ".ref");
				if (!recipeID) {
					continue;
				}

				const auto referenceInspection = InspectReference(path);
				if (referenceInspection.state == EntryState::Error) {
					return false;
				}
				if (referenceInspection.state != EntryState::Valid) {
					continue;
				}

				const auto index = parsedRecipes.size();
				if (!recipeIndices.try_emplace(*recipeID, index).second) {
					return false;
				}
				parsedRecipes.push_back({ .reference = referenceInspection.reference });
			}

			std::unordered_set<std::string> affectedObjects;
			affectedObjects.reserve(a_recipeSHA256s.size());
			for (const auto& recipeID : a_recipeSHA256s) {
				const auto recipeIterator = recipeIndices.find(recipeID);
				if (recipeIterator != recipeIndices.end()) {
					affectedObjects.insert(parsedRecipes[recipeIterator->second].reference.objectSHA256);
				}
			}

			std::unordered_map<std::string, ObjectInspection> objectInspections;
			std::unordered_map<std::string, std::size_t> validReferenceCounts;
			objectInspections.reserve(affectedObjects.size());
			validReferenceCounts.reserve(affectedObjects.size());
			for (const auto& objectID : affectedObjects) {
				const auto objectPath = GetObjectPath(objectID);
				if (!objectPath) {
					return false;
				}

				const auto inspection = InspectObject(*objectPath, objectID);
				if (inspection.state == EntryState::Error) {
					return false;
				}
				objectInspections.emplace(objectID, inspection);
				if (inspection.state == EntryState::Valid) {
					validReferenceCounts.emplace(objectID, 0);
				}
			}

			for (const auto& recipe : parsedRecipes) {
				const auto inspectionIterator = objectInspections.find(recipe.reference.objectSHA256);
				if (inspectionIterator != objectInspections.end() &&
					inspectionIterator->second.state == EntryState::Valid &&
					inspectionIterator->second.byteLength == recipe.reference.byteLength) {
					++validReferenceCounts[recipe.reference.objectSHA256];
				}
			}

			bool success = true;
			std::unordered_set<std::string> reclaimCandidates;
			reclaimCandidates.reserve(affectedObjects.size());
			for (const auto& recipeID : a_recipeSHA256s) {
				const auto recipePath = GetRecipePath(recipeID);
				if (!recipePath || !DeleteFileIfPresent(*recipePath)) {
					success = false;
					continue;
				}

				const auto recipeIterator = recipeIndices.find(recipeID);
				if (recipeIterator == recipeIndices.end()) {
					continue;
				}

				const auto& removedReference = parsedRecipes[recipeIterator->second].reference;
				const auto inspectionIterator = objectInspections.find(removedReference.objectSHA256);
				if (inspectionIterator == objectInspections.end() ||
					inspectionIterator->second.state != EntryState::Valid ||
					inspectionIterator->second.byteLength != removedReference.byteLength) {
					continue;
				}

				auto countIterator = validReferenceCounts.find(removedReference.objectSHA256);
				if (countIterator == validReferenceCounts.end() || countIterator->second == 0) {
					success = false;
					continue;
				}
				--countIterator->second;
				reclaimCandidates.insert(removedReference.objectSHA256);
			}

			for (const auto& objectID : reclaimCandidates) {
				const auto countIterator = validReferenceCounts.find(objectID);
				if (countIterator == validReferenceCounts.end() || countIterator->second != 0) {
					continue;
				}

				const auto objectPath = GetObjectPath(objectID);
				if (!objectPath || !DeleteFileIfPresent(*objectPath)) {
					success = false;
				}
			}

			return success;
		}

		[[nodiscard]] bool PruneUnlocked()
		{
			const auto inventory = InventoryTree(GetRoot());
			if (!inventory) {
				return false;
			}

			std::vector<std::filesystem::path> invalidReferences;
			std::vector<std::filesystem::path> temporaryFiles;
			std::vector<std::pair<std::filesystem::path, std::optional<std::string>>> objectFiles;
			std::unordered_set<std::string> liveObjects;
			std::unordered_map<std::string, ObjectInspection> objectInspections;

			for (const auto& path : inventory->files) {
				if (IsTemporaryFile(path)) {
					temporaryFiles.push_back(path);
					continue;
				}

				if (path.extension() == ".ref") {
					if (!ExtractContentID(path, "recipes", ".ref")) {
						invalidReferences.push_back(path);
						continue;
					}

					const auto referenceInspection = InspectReference(path);
					if (referenceInspection.state == EntryState::Error) {
						return false;
					}
					if (referenceInspection.state != EntryState::Valid) {
						invalidReferences.push_back(path);
						continue;
					}

					const auto& reference = referenceInspection.reference;
					auto [objectIterator, inserted] = objectInspections.try_emplace(reference.objectSHA256);
					if (inserted) {
						const auto objectPath = GetObjectPath(reference.objectSHA256);
						if (!objectPath) {
							return false;
						}
						objectIterator->second = InspectObject(*objectPath, reference.objectSHA256);
					}

					if (objectIterator->second.state == EntryState::Error) {
						return false;
					}
					if (objectIterator->second.state != EntryState::Valid ||
						objectIterator->second.byteLength != reference.byteLength) {
						invalidReferences.push_back(path);
						continue;
					}

					liveObjects.insert(reference.objectSHA256);
				} else if (path.extension() == ".dxbc") {
					objectFiles.emplace_back(path, ExtractContentID(path, "objects", ".dxbc"));
				}
			}

			std::vector<std::filesystem::path> orphanObjects;
			orphanObjects.reserve(objectFiles.size());
			for (const auto& [path, objectID] : objectFiles) {
				if (!objectID || !liveObjects.contains(*objectID)) {
					orphanObjects.push_back(path);
				}
			}

			bool success = true;
			for (const auto& path : invalidReferences) {
				success = DeleteFileIfPresent(path) && success;
			}
			for (const auto& path : temporaryFiles) {
				success = DeleteFileIfPresent(path) && success;
			}
			for (const auto& path : orphanObjects) {
				success = DeleteFileIfPresent(path) && success;
			}

			auto directories = inventory->directories;
			std::ranges::sort(directories, [](const auto& a_left, const auto& a_right) {
				return a_left.native().size() > a_right.native().size();
			});
			for (const auto& path : directories) {
				std::error_code error;
				static_cast<void>(std::filesystem::remove(path, error));
				if (error && !IsMissing(error) && error != std::errc::directory_not_empty) {
					success = false;
				}
			}

			return success;
		}

		[[nodiscard]] bool EnsureObject(
			const std::filesystem::path& a_objectPath,
			std::string_view a_objectSHA256,
			std::span<const std::uint8_t> a_bytecode)
		{
			const auto byteLength = static_cast<std::uint64_t>(a_bytecode.size());
			const auto existingObject = InspectObject(a_objectPath, a_objectSHA256);
			if (existingObject.state == EntryState::Error) {
				return false;
			}
			if (existingObject.state == EntryState::Valid) {
				// A hash match with a different length is a collision or caller error;
				// never replace the valid immutable object in either case.
				return existingObject.byteLength == byteLength;
			}

			// A valid object is never replaced. An invalid object is safe to discard:
			// no successful lookup can consume it, and the new bytes hash to its ID.
			if (!DeleteFileIfPresent(a_objectPath)) {
				return false;
			}

			if (!AtomicInstall(a_objectPath, a_bytecode)) {
				// Another process that does not share our process-local mutex may have
				// won the no-replace move. Accept it only after full validation.
				return ValidateObject(a_objectPath, a_objectSHA256, byteLength);
			}

			return ValidateObject(a_objectPath, a_objectSHA256, byteLength);
		}

		[[nodiscard]] std::optional<LookupResult> LookupUnlocked(std::string_view a_recipeSHA256)
		{
			const auto recipePath = GetRecipePath(a_recipeSHA256);
			if (!recipePath) {
				return std::nullopt;
			}

			const auto reference = ReadReference(*recipePath);
			if (!reference) {
				return std::nullopt;
			}

			const auto objectPath = GetObjectPath(reference->objectSHA256);
			if (!objectPath || !ValidateObject(*objectPath, reference->objectSHA256, reference->byteLength)) {
				return std::nullopt;
			}

			return LookupResult{
				.path = *objectPath,
				.objectSHA256 = reference->objectSHA256,
				.byteLength = reference->byteLength
			};
		}
	}

	const std::filesystem::path& GetRoot()
	{
		static const std::filesystem::path root = [] {
			PWSTR localAppData = nullptr;
			if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, std::addressof(localAppData))) ||
				!localAppData) {
				return std::filesystem::path{};
			}

			try {
				const std::filesystem::path result =
					std::filesystem::path(localAppData) /
					"CommunityShaders" /
					"ShaderCache" /
					("v" + std::to_string(SCHEMA_VERSION));
				CoTaskMemFree(localAppData);
				return result;
			} catch (...) {
				CoTaskMemFree(localAppData);
				return std::filesystem::path{};
			}
		}();
		return root;
	}

	std::optional<std::filesystem::path> GetRecipePath(std::string_view a_recipeSHA256)
	{
		try {
			return BuildContentPath("recipes", a_recipeSHA256, ".ref");
		} catch (...) {
			return std::nullopt;
		}
	}

	std::optional<std::filesystem::path> GetObjectPath(std::string_view a_objectSHA256)
	{
		try {
			return BuildContentPath("objects", a_objectSHA256, ".dxbc");
		} catch (...) {
			return std::nullopt;
		}
	}

	std::optional<LookupResult> Lookup(std::string_view a_recipeSHA256)
	{
		try {
			std::shared_lock lock(ProcessMutex);
			return LookupUnlocked(a_recipeSHA256);
		} catch (...) {
			return std::nullopt;
		}
	}

	bool Publish(std::string_view a_recipeSHA256, std::span<const std::uint8_t> a_bytecode)
	{
		try {
			if (!IsValidSHA256(a_recipeSHA256) || !HasDXBCMagic(a_bytecode) || GetRoot().empty() ||
				a_bytecode.size() > (std::numeric_limits<std::uint64_t>::max)()) {
				return false;
			}

			const auto objectSHA256 = ShaderStorage::SHA256(a_bytecode);
			if (!objectSHA256 || !IsValidSHA256(*objectSHA256)) {
				return false;
			}

			const auto recipePath = GetRecipePath(a_recipeSHA256);
			const auto objectPath = GetObjectPath(*objectSHA256);
			if (!recipePath || !objectPath) {
				return false;
			}

			const RecipeReference newReference{
				.objectSHA256 = *objectSHA256,
				.byteLength = static_cast<std::uint64_t>(a_bytecode.size())
			};

			std::unique_lock processLock(ProcessMutex);
			const auto crossProcessLock = NamedMutexLock::Acquire();
			if (!crossProcessLock) {
				return false;
			}

			// A valid recipe is immutable. Matching content is an idempotent hit;
			// different valid content exposes a recipe collision and fails closed.
			const auto existingReference = InspectReference(*recipePath);
			if (existingReference.state == EntryState::Error) {
				return false;
			}
			if (existingReference.state == EntryState::Valid) {
				const auto existingObjectPath = GetObjectPath(existingReference.reference.objectSHA256);
				if (!existingObjectPath) {
					return false;
				}

				const auto existingObject = InspectObject(
					*existingObjectPath,
					existingReference.reference.objectSHA256);
				if (existingObject.state == EntryState::Error) {
					return false;
				}
				if (existingObject.state == EntryState::Valid &&
					existingObject.byteLength == existingReference.reference.byteLength) {
					return existingReference.reference.objectSHA256 == newReference.objectSHA256 &&
					       existingReference.reference.byteLength == newReference.byteLength;
				}
			}

			// Invalid references are repairable; objects are installed first so a
			// newly visible reference can never point at a missing object.
			if (!DeleteFileIfPresent(*recipePath) ||
				!EnsureObject(*objectPath, *objectSHA256, a_bytecode)) {
				return false;
			}

			const auto referenceContents = SerializeReference(newReference);
			const auto referenceBytes = std::span(
				reinterpret_cast<const std::uint8_t*>(referenceContents.data()),
				referenceContents.size());
			if (!AtomicInstall(*recipePath, referenceBytes)) {
				// Validate a cross-session winner rather than replacing it.
				return ValidateRecipe(
					*recipePath,
					newReference.objectSHA256,
					newReference.byteLength);
			}

			return ValidateRecipe(
				*recipePath,
				newReference.objectSHA256,
				newReference.byteLength);
		} catch (...) {
			return false;
		}
	}

	bool RemoveRecipe(std::string_view a_recipeSHA256)
	{
		try {
			const std::array<std::string, 1> recipes{ std::string(a_recipeSHA256) };
			return RemoveRecipes(recipes);
		} catch (...) {
			return false;
		}
	}

	bool RemoveRecipeIfMatches(
		std::string_view a_recipeSHA256,
		std::string_view a_expectedObjectSHA256,
		std::uint64_t a_expectedByteLength)
	{
		try {
			if (!IsValidSHA256(a_recipeSHA256) ||
				!IsValidSHA256(a_expectedObjectSHA256) ||
				a_expectedByteLength < DXBC_MAGIC.size() ||
				GetRoot().empty()) {
				return false;
			}

			const auto recipePath = GetRecipePath(a_recipeSHA256);
			if (!recipePath) {
				return false;
			}

			std::unique_lock processLock(ProcessMutex);
			const auto crossProcessLock = NamedMutexLock::Acquire();
			if (!crossProcessLock) {
				return false;
			}

			const auto currentReference = InspectReference(*recipePath);
			if (currentReference.state == EntryState::Error) {
				return false;
			}
			if (currentReference.state != EntryState::Valid ||
				currentReference.reference.objectSHA256 != a_expectedObjectSHA256 ||
				currentReference.reference.byteLength != a_expectedByteLength) {
				return true;
			}

			return RemoveRecipesUnlocked({ std::string(a_recipeSHA256) });
		} catch (...) {
			return false;
		}
	}

	bool RemoveRecipes(std::span<const std::string> a_recipeSHA256s)
	{
		try {
			if (a_recipeSHA256s.empty()) {
				return true;
			}

			std::unordered_set<std::string> uniqueRecipes;
			uniqueRecipes.reserve(a_recipeSHA256s.size());
			for (const auto& recipeID : a_recipeSHA256s) {
				if (!IsValidSHA256(recipeID)) {
					return false;
				}
				uniqueRecipes.insert(recipeID);
			}
			if (GetRoot().empty()) {
				return false;
			}

			std::unique_lock processLock(ProcessMutex);
			const auto crossProcessLock = NamedMutexLock::Acquire();
			return crossProcessLock && RemoveRecipesUnlocked(uniqueRecipes);
		} catch (...) {
			return false;
		}
	}

	bool Prune()
	{
		try {
			if (GetRoot().empty()) {
				return false;
			}

			std::unique_lock processLock(ProcessMutex);
			const auto crossProcessLock = NamedMutexLock::Acquire();
			return crossProcessLock && PruneUnlocked();
		} catch (...) {
			return false;
		}
	}

	bool Clear()
	{
		try {
			if (GetRoot().empty()) {
				return false;
			}

			std::unique_lock processLock(ProcessMutex);
			const auto crossProcessLock = NamedMutexLock::Acquire();
			if (!crossProcessLock) {
				return false;
			}

			std::error_code error;
			std::filesystem::remove_all(GetRoot(), error);
			return !error;
		} catch (...) {
			return false;
		}
	}
}
