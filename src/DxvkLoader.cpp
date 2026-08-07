#include "DxvkLoader.h"

#include <filesystem>

namespace DxvkLoader
{
	namespace
	{
		bool g_attempted = false;
		bool g_loaded = false;
		decltype(&D3D11CreateDeviceAndSwapChain) g_d3d11Create = nullptr;
		decltype(&CreateDXGIFactory) g_createFactory = nullptr;
	}

	// Directory holding all staged renderer runtime DLLs, resolved relative to this
	// plugin's own module so it works regardless of the process CWD or a mod
	// manager's virtual file system. CommunityShaders.dll lives in
	// .../SKSE/Plugins, so the DLLs sit in .../SKSE/Plugins/CommunityShaders/bin.
	std::filesystem::path GetRuntimeDir()
	{
		HMODULE self = nullptr;
		if (!::GetModuleHandleExW(
				GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				reinterpret_cast<LPCWSTR>(&GetRuntimeDir),
				&self)) {
			return {};
		}
		wchar_t buf[MAX_PATH]{};
		const DWORD n = ::GetModuleFileNameW(self, buf, MAX_PATH);
		if (n == 0 || n >= MAX_PATH) {
			return {};
		}
		return std::filesystem::path(buf).parent_path() / L"CommunityShaders" / L"bin";
	}

	bool NativeModeRequested()
	{
		// CS_NATIVE_D3D11=1 forces the system D3D11 runtime instead of the bundled DXVK
		// (and disables the Vulkan-only upscaler stack). Used to validate the shadow-deferred
		// path on native drivers, which handle deferred command lists robustly where DXVK's
		// deferred context mis-handles the engine's shared dynamic buffers.
		static const bool s_native = [] {
			char buf[8] = {};
			return GetEnvironmentVariableA("CS_NATIVE_D3D11", buf, sizeof(buf)) && buf[0] == '1';
		}();
		return s_native;
	}

	bool Load()
	{
		if (g_attempted) {
			return g_loaded;
		}
		g_attempted = true;

		if (NativeModeRequested()) {
			logger::info("[DXVK] CS_NATIVE_D3D11=1 -- skipping DXVK, using the system D3D11 runtime");
			return false;
		}

		const auto dir = GetRuntimeDir();
		if (dir.empty()) {
			logger::error("[DXVK] Could not resolve plugin directory for DXVK DLLs");
			return false;
		}

		const auto dxgiPath = (dir / L"dxvk_dxgi.dll").wstring();
		const auto d3d11Path = (dir / L"dxvk_d3d11.dll").wstring();

		// Load dxgi first: dxvk_d3d11.dll imports dxvk_dxgi.dll (DXVK is built with the dxvk_ name prefix),
		// and the loader binds that import to the already-mapped module by base name.
		const HMODULE dxgiMod = ::LoadLibraryExW(dxgiPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!dxgiMod) {
			const DWORD err = ::GetLastError();
			logger::error("[DXVK] Failed to load dxvk_dxgi.dll from '{}' (error {})", dir.string(), err);
			return false;
		}
		const HMODULE d3d11Mod = ::LoadLibraryExW(d3d11Path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
		if (!d3d11Mod) {
			const DWORD err = ::GetLastError();
			logger::error("[DXVK] Failed to load dxvk_d3d11.dll from '{}' (error {})", dir.string(), err);
			return false;
		}

		g_d3d11Create = reinterpret_cast<decltype(g_d3d11Create)>(::GetProcAddress(d3d11Mod, "D3D11CreateDeviceAndSwapChain"));
		g_createFactory = reinterpret_cast<decltype(g_createFactory)>(::GetProcAddress(dxgiMod, "CreateDXGIFactory"));

		if (!g_d3d11Create || !g_createFactory) {
			logger::error("[DXVK] Resolved DXVK DLLs but missing exports (d3d11create={}, createfactory={})",
				g_d3d11Create != nullptr, g_createFactory != nullptr);
			return false;
		}

		// Seed the present mode to ASYNC (stock DXVK behavior). The fork's frame-gen present
		// ownership defaults to SYNCHRONOUS present, and the only writers of the flag live in
		// the Upscaling feature (Upscaling::Load seeds the saved setting; FrameGenController
		// tracks the FG lifecycle) -- so with Upscaling absent/disabled NOTHING pushed async and
		// every no-upscaler DXVK user was silently vsync-capped at the display refresh (measured
		// dead-flat 165.1 fps on a 165Hz panel; dxvk.conf dxgi.syncInterval=0 cannot override
		// it). Idempotent: Upscaling/FrameGenController overwrite this whenever FG engages.
		if (auto setSync = reinterpret_cast<void (*)(uint32_t)>(::GetProcAddress(d3d11Mod, "dxvkSetSyncPresent")))
			setSync(0u);
		else
			logger::warn("[DXVK] dxvkSetSyncPresent export missing -- present stays at the fork default (sync)");

		logger::info("[DXVK] Loaded DXVK from '{}' (dxvk_d3d11.dll + dxvk_dxgi.dll)", dir.string());
		g_loaded = true;
		return true;
	}

	bool IsLoaded() { return g_loaded; }
	decltype(&D3D11CreateDeviceAndSwapChain) GetD3D11CreateDeviceAndSwapChain() { return g_d3d11Create; }
	decltype(&CreateDXGIFactory) GetCreateDXGIFactory() { return g_createFactory; }
}
