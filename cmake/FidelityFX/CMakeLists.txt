cmake_minimum_required(VERSION 3.15)

set(FidelityFXPath "../../extern/FidelityFX-SDK-DX11")
project(FidelityFX)

set(SOURCES
    ${FidelityFXPath}/DXBC/md5.h
    ${FidelityFXPath}/FidelityFX/gpu/ffx_common_types.h
    ${FidelityFXPath}/FidelityFX/gpu/ffx_core.h
    ${FidelityFXPath}/FidelityFX/gpu/ffx_core_gpu_common.h
    ${FidelityFXPath}/FidelityFX/gpu/ffx_core_gpu_common_half.h
    ${FidelityFXPath}/FidelityFX/gpu/ffx_core_hlsl.h
    ${FidelityFXPath}/FidelityFX/gpu/ffx_core_portability.h
    ${FidelityFXPath}/FidelityFX/host/ffx_assert.h
    ${FidelityFXPath}/FidelityFX/host/ffx_error.h
    ${FidelityFXPath}/FidelityFX/host/ffx_interface.h
    ${FidelityFXPath}/FidelityFX/host/ffx_types.h
    ${FidelityFXPath}/FidelityFX/host/ffx_util.h
    ${FidelityFXPath}/FidelityFX/host/shared/ffx_object_management.h
    ${FidelityFXPath}/DXBC/DXBCChecksum.c
    ${FidelityFXPath}/DXBC/md5.c
    ${FidelityFXPath}/FidelityFX/host/shared/ffx_assert.cpp
    ${FidelityFXPath}/FidelityFX/host/shared/ffx_object_management.cpp
    ${FidelityFXPath}/FidelityFX/host/backends/dx11/ffx_dx11.cpp
    ${FidelityFXPath}/FidelityFX/host/components/frameinterpolation/ffx_frameinterpolation.cpp
    ${FidelityFXPath}/FidelityFX/host/backends/ffx_shader_blobs.cpp)

add_library(${PROJECT_NAME} STATIC ${SOURCES})

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_23)

if(MSVC)
  target_compile_options(
    ${PROJECT_NAME}
    PUBLIC # disable warnings
           /wd4065
           /wd4100
           /wd4189
           /wd4324
           /wd4456
           /wd4505
           /wd4996)
endif()

target_include_directories(
  ${PROJECT_NAME} PUBLIC ${FidelityFXPath} ${FidelityFXPath}/FidelityFX
                         ${FidelityFXPath}/FidelityFX/host/shared)
