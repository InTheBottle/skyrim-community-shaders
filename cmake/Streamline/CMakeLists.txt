cmake_minimum_required(VERSION 3.15)

set(StreamlinePath "../../extern/Streamline")
project(Streamline)

set(SOURCES
  ${StreamlinePath}/include/sl.h
  ${StreamlinePath}/include/sl_consts.h
  ${StreamlinePath}/include/sl_dlss.h
  ${StreamlinePath}/include/sl_dlss_g.h
  ${StreamlinePath}/include/sl_helpers.h
  ${StreamlinePath}/include/sl_helpers_vk.h
  ${StreamlinePath}/include/sl_hooks.h
  ${StreamlinePath}/include/sl_matrix_helpers.h
  ${StreamlinePath}/include/sl_nis.h
  ${StreamlinePath}/include/sl_nrd.h
  ${StreamlinePath}/include/sl_nrd.h
  ${StreamlinePath}/include/sl_reflex.h
  ${StreamlinePath}/include/sl_result.h
  ${StreamlinePath}/include/sl_security.h
  ${StreamlinePath}/include/sl_struct.h
  ${StreamlinePath}/include/sl_template.h
  ${StreamlinePath}/include/sl_version.h
  ${StreamlinePath}/lib/x64/sl.interposer.lib
)

add_library(${PROJECT_NAME} STATIC ${SOURCES})

target_compile_features(${PROJECT_NAME} PUBLIC cxx_std_23)

target_include_directories(
  ${PROJECT_NAME} PUBLIC ${StreamlinePath} ${StreamlinePath}/include)
