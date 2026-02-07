cmake_minimum_required(VERSION 3.20)
project(DiskPartFmtTool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (NOT WIN32)
  message(FATAL_ERROR "This sample only builds on Windows.")
endif()

add_executable(disk_part_fmt
  src/main.cpp
)

target_compile_definitions(disk_part_fmt PRIVATE
  UNICODE
  _UNICODE
  _WIN32_DCOM
)

target_link_libraries(disk_part_fmt PRIVATE
  ole32
  oleaut32
  uuid
  vdsuuid
)
