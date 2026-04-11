cmake_minimum_required(VERSION 3.14)
project(picmerge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    add_compile_options(/W3 /permissive- /utf-8)
    add_compile_options($<$<CONFIG:Release>:/O2>)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
    add_compile_options($<$<CONFIG:Release>:-O2>)
endif()

add_executable(picmerge
    src/main.cpp
    src/image_io.cpp
    src/row_signature.cpp
    src/bar_detector.cpp
    src/sticky_header.cpp
    src/overlap_finder.cpp
    src/stitcher.cpp
)

target_include_directories(picmerge PRIVATE
    src
    third_party/stb
)

# On some older GCC versions, <filesystem> requires linking stdc++fs explicitly.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(picmerge PRIVATE stdc++fs)
endif()
