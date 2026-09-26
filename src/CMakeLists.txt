# RT-safe core: pure C++20, zero JUCE dependencies. Header-only for now.
add_library(onda_core INTERFACE)
target_include_directories(onda_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(onda_core INTERFACE cxx_std_20)

juce_add_gui_app(onda
    VERSION ${PROJECT_VERSION}
    PRODUCT_NAME "Onda"
    COMPANY_NAME "Onda Audio"
    BUNDLE_ID "com.ondaaudio.onda"
    MICROPHONE_PERMISSION_ENABLED FALSE
    MICROPHONE_PERMISSION_TEXT "Onda does not use the microphone")

target_sources(onda PRIVATE
    app/Main.cpp
    app/MainComponent.cpp
    platform/log/Log.cpp
    platform/config/Config.cpp
    platform/audio/AudioEngine.cpp)

target_include_directories(onda PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(onda PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="${PROJECT_NAME}"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(onda PRIVATE
    onda_core
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
