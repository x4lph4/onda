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

# --- ffmpeg (decoder backend) ----------------------------------------------
# Dev files live under third_party/ffmpeg (gyan.dev full-shared build,
# gitignored — download via the URL in docs/adr or any shared dev build).
set(ONDA_FFMPEG_ROOT "${CMAKE_SOURCE_DIR}/third_party/ffmpeg" CACHE PATH "ffmpeg dev root")
find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h
          PATHS "${ONDA_FFMPEG_ROOT}/include" REQUIRED)
set(ONDA_FFMPEG_LIBS "")
foreach(flib avformat avcodec avutil swresample)
    find_library(FFMPEG_LIB_${flib} NAMES ${flib}
                 PATHS "${ONDA_FFMPEG_ROOT}/lib" REQUIRED)
    list(APPEND ONDA_FFMPEG_LIBS ${FFMPEG_LIB_${flib}})
endforeach()

file(GLOB ONDA_FFMPEG_DLLS "${ONDA_FFMPEG_ROOT}/bin/*.dll")

# Copies ffmpeg runtime DLLs next to a target's output binary.
function(onda_copy_runtime_dlls target)
    if(ONDA_FFMPEG_DLLS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ONDA_FFMPEG_DLLS}
                    $<TARGET_FILE_DIR:${target}>)
    endif()
endfunction()

add_subdirectory(src)

if(ONDA_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
