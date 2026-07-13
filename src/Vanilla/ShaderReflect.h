#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

// vanilla:: — the reusable Skyrim SE 1.5.97 reverse-engineering / reimplementation space. This is
// its first shared service: DXBC reflection of the shaders bound at draw time, so the multithreaded
// draw-state validator can compare only the constant-buffer BYTES a shader actually reads and only
// the resource SLOTS it actually binds (the rest is free to differ across threads/contexts).
namespace vanilla
{
	/// Reflection result for one shader, extracted once from its DXBC and cached by ID3D11 shader ptr.
	/// All slots/ranges are in D3D11 register / byte units. "Used" means the FXC reflector flagged the
	/// constant-buffer variable as referenced (D3D_SVF_USED) or listed the resource in BoundResources.
	struct ShaderUsage
	{
		struct Range
		{
			std::uint32_t begin;  // inclusive byte offset
			std::uint32_t end;    // exclusive byte offset
		};
		struct CBUsage
		{
			std::uint32_t      sizeBytes = 0;  // declared cbuffer size
			std::vector<Range> used;           // merged shader-used byte ranges (empty => none used)
			bool               present = false;  // this slot is a cbuffer the shader binds
		};

		std::array<CBUsage, 14> cb{};              // by constant-buffer register slot b0..b13
		std::uint32_t           usedCBMask = 0;    // bit s => CB slot s is a used cbuffer
		std::uint32_t           usedSRVMask = 0;   // bit s => SRV slot s (t#, 0..31) is used
		std::uint32_t           usedSamplerMask = 0;  // bit s => sampler slot s (s#, 0..31) is used
		bool                    valid = false;     // false => reflection unavailable (no bytecode etc.)

		/// FNV-1a hash of ONLY the shader-used bytes of CB slot `slot`, read from `data` (`len` bytes).
		/// Ranges are clamped to `len`. Returns a fixed sentinel when the slot is unused so equal-unused
		/// compares equal. This is the value that goes into the per-draw fingerprint for a CB slot.
		[[nodiscard]] std::uint64_t HashUsedCB(int slot, const std::uint8_t* data, std::size_t len) const;
	};

	/// Process-wide cache mapping a bound ID3D11 vertex/pixel shader pointer to its reflected usage.
	/// Bytecode comes from the CreateVertexShader/CreatePixelShader capture (ShaderBytecodeMap via
	/// Hooks.cpp TryGetShaderBytecode), which the RE draw-state validator enables with CS_RE_REFLECT.
	class ShaderReflect
	{
	public:
		static ShaderReflect* GetSingleton();

		/// Reflect (or return cached) the shader identified by its ID3D11 shader pointer. Returns a
		/// stable pointer into the cache; nullptr only if the pointer is null. The returned usage has
		/// valid=false if the bytecode was not captured or reflection failed (caller should then fall
		/// back to comparing the whole bound CB range, or treat the shader as opaque).
		const ShaderUsage* Get(void* a_shaderPtr);

		/// Whether this run wants shader-bytecode capture enabled (drives the Hooks.cpp install and the
		/// validator). True when CS_RE_REFLECT is set. Read once.
		[[nodiscard]] static bool WantsCapture();

	private:
		ShaderReflect() = default;
		ShaderUsage Reflect(const void* a_dxbc, std::size_t a_len);

		std::unordered_map<void*, ShaderUsage> cache;
		std::mutex                             mutex;  // Get() may be called from worker threads
	};
}
