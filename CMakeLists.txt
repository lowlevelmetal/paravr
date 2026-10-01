cmake_minimum_required(VERSION 3.16)
project(paravr LANGUAGES CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "paravr drives the parallel port through Linux ppdev and only builds on Linux")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra)

include(GNUInstallDirs)

add_subdirectory(src)

option(PARAVR_BUILD_TESTS "Build the hardware-free test suite" ON)
if(PARAVR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
