FetchContent_Declare(Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.9.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(Catch2)

add_executable(onda_tests rt_tests.cpp)
target_link_libraries(onda_tests PRIVATE onda_core Catch2::Catch2WithMain)
target_compile_features(onda_tests PRIVATE cxx_std_20)

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)
catch_discover_tests(onda_tests)
