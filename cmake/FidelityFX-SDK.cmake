set(FFX_API_VK OFF)
set(FFX_API_DX12 OFF)
set(FFX_ALL OFF)
set(FFX_FSR3 ON)
set(FFX_FSR ON)
set(FFX_AUTO_COMPILE_SHADERS 1)

add_subdirectory(${CMAKE_SOURCE_DIR}/extern/FidelityFX-SDK/sdk)

# Work around an upstream CMake bug in the FFX dx11 backend's compile_shaders():
# it leaks the literal out-variable name (e.g. "FSR2_PERMUTATION_OUTPUTS") into
# the dependency list of the phony ffx_shader_permutations_dx11 custom target.
# The Visual Studio generator silently ignores the bogus file dependencies, but
# strict generators (Ninja) fail with "missing and no known rule to make it".
# The target is pure phony aggregation, so pre-creating empty placeholder files
# at the paths Ninja resolves them to satisfies the dependency without ever
# triggering rebuilds. No effect on the VS generator path.
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
