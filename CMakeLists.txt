cmake_minimum_required(VERSION 3.16)
project(STAR VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(STAR_BUILD_TESTS "Build standalone regression checks" OFF)
if(STAR_BUILD_TESTS)
    enable_testing()
endif()
add_subdirectory(src)
