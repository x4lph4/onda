cmake_minimum_required(VERSION 3.25)
project(Onda VERSION 0.1.0 LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Onda — professional DJ application (GPLv3)
# F2 skeleton: JUCE shell, RT-safe core primitives, device HAL, diagnostics.
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

option(ONDA_BUILD_TESTS "Build unit tests" ON)

include(FetchContent)

FetchContent_Declare(JUCE
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.9.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(JUCE)

add_subdirectory(src)

if(ONDA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
