FetchContent_Declare(Catch2
    URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.9.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(Catch2)

add_executable(onda_tests
    rt_tests.cpp
    engine_tests.cpp
    decoder_tests.cpp
    stress_tests.cpp
    analysis_tests.cpp
    analysis_phase30_tests.cpp
    performance_tests.cpp
    mixer_tests.cpp
    mixer_phase17_tests.cpp
    fx_tests.cpp
    fx_phase16_tests.cpp
    library_tests.cpp
    library_phase18_tests.cpp
    library_phase29_tests.cpp
    ui_tests.cpp
    waveform_phase19_tests.cpp
    performance_phase20_tests.cpp
    controller_tests.cpp
    flx4_tests.cpp
    sampler_tests.cpp
    recording_tests.cpp
    adevice_tests.cpp
    sync_tests.cpp
    stems_phase21_tests.cpp
    key_phase22_tests.cpp
    smart_phase23_tests.cpp
    mixer_phase24_tests.cpp
    mixer_phase32_tests.cpp
  adevice_phase26_tests.cpp
    fx_phase25_tests.cpp
    fx_phase33_tests.cpp
    ctrl_phase27_tests.cpp
    perf_phase28_tests.cpp
    waveform_phase31_tests.cpp)
target_link_libraries(onda_tests PRIVATE onda_core onda_ui onda_ctrl onda_decode Catch2::Catch2WithMain)
target_compile_features(onda_tests PRIVATE cxx_std_20)
onda_copy_runtime_dlls(onda_tests)

list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)
catch_discover_tests(onda_tests)
