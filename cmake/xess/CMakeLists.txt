cmake_minimum_required(VERSION 3.21)

add_library(xess INTERFACE)

target_include_directories(
  xess INTERFACE ${CMAKE_SOURCE_DIR}/extern/xess/inc)

target_link_libraries(
  xess INTERFACE ${CMAKE_SOURCE_DIR}/extern/xess/lib/libxess.lib
)
