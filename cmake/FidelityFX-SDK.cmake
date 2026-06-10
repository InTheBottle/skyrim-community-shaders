set(FFX_API_VK OFF)
set(FFX_API_DX12 OFF)
set(FFX_ALL OFF)
set(FFX_FSR3 ON)
set(FFX_FSR ON)
set(FFX_AUTO_COMPILE_SHADERS 1)

# Platform-detection quirk: extern/FidelityFX-SDK/sdk/CMakeLists.txt picks x64
# via `CMAKE_GENERATOR_PLATFORM STREQUAL "x64" OR CMAKE_EXE_LINKER_FLAGS
# STREQUAL "/machine:x64"` — an EXACT string compare. Single-config generators
# (Ninja) leave CMAKE_GENERATOR_PLATFORM unset, so the ninja preset sets
# CMAKE_EXE_LINKER_FLAGS to exactly "/machine:x64" to satisfy that check. It
# is not a link flag for our DLL (MSVC infers machine type from the objects);
# appending anything else to that variable breaks the STREQUAL and fails the
# FFX configure with "Unsupported target platform".

add_subdirectory(${CMAKE_SOURCE_DIR}/extern/FidelityFX-SDK/sdk)

# Work around an upstream CMake bug in the FFX dx11 backend's compile_shaders():
# it leaks the literal out-variable name (e.g. "FSR2_PERMUTATION_OUTPUTS") into
# the dependency list of the phony ffx_shader_permutations_dx11 custom target.
# The Visual Studio generator silently ignores the bogus file dependencies, but
# strict generators (Ninja) fail with "missing and no known rule to make it".
# The target is pure phony aggregation, so pre-creating empty placeholder files
# at the paths Ninja resolves them to satisfies the dependency without ever
# triggering rebuilds. No effect on the VS generator path.
#
# MAINTENANCE: this list mirrors the compile_shaders() call sites in
# extern/FidelityFX-SDK/sdk/src/backends/dx11/CMakeLists.txt at the currently
# vendored SDK revision. If the submodule is updated and a Ninja build fails
# with "<NAME>_PERMUTATION_OUTPUTS ... missing and no known rule to make it",
# add the new out-variable name here (or remove ones that no longer exist).
if(NOT CMAKE_GENERATOR MATCHES "Visual Studio")
  set(_ffx_dx11_bindir
      "${CMAKE_BINARY_DIR}/extern/FidelityFX-SDK/sdk/src/backends/dx11"
  )
  foreach(
    _ffx_bogus_dep
    FSR1_PERMUTATION_OUTPUTS
    FSR2_PERMUTATION_OUTPUTS
    FSR3UPSCALER_PERMUTATION_OUTPUTS
    FRAMEINTERPOLATION_PERMUTATION_OUTPUTS
    OPTICALFLOW_PERMUTATION_OUTPUTS
  )
    if(NOT EXISTS "${_ffx_dx11_bindir}/${_ffx_bogus_dep}")
      file(
        WRITE "${_ffx_dx11_bindir}/${_ffx_bogus_dep}"
        "placeholder for upstream FFX CMake dependency-name leak; see cmake/FidelityFX-SDK.cmake\n"
      )
    endif()
  endforeach()
endif()

target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE
  ffx_backend_dx11_x64
  ffx_fsr3_x64
)
