cmake_minimum_required(VERSION 3.20)
project(partitionAndFormatOnWindows LANGUAGES CXX)

add_executable(disk_part_fmt
    src/main.cpp
)

target_compile_features(disk_part_fmt PRIVATE cxx_std_17)

if (MSVC)
    target_compile_options(disk_part_fmt PRIVATE /W4 /permissive-)
else()
    target_compile_options(disk_part_fmt PRIVATE -Wall -Wextra -Wpedantic)
endif()
