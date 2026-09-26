# sqlite3 amalgamation (public domain, third_party, gitignored)
add_library(sqlite3 STATIC ../third_party/sqlite/sqlite3.c)
target_include_directories(sqlite3 PUBLIC ../third_party/sqlite)
target_compile_definitions(sqlite3 PRIVATE SQLITE_ENABLE_FTS5=1)

# RT-safe core: pure C++20, zero JUCE, zero ffmpeg.
add_library(onda_core STATIC
    core/audio/AudioFifo.cpp
    core/dsp/WsolaStretcher.cpp
    core/engine/DeckEngine.cpp
    core/mixer/MixerEngine.cpp
    core/engine/TrackLoader.cpp
    core/analysis/OnsetDetector.cpp
    core/analysis/BpmAnalyzer.cpp
    core/analysis/BeatGridEngine.cpp
    core/analysis/KeyAnalyzer.cpp
    core/analysis/WaveformGenerator.cpp
    core/analysis/MusicAnalysisEngine.cpp
    core/analysis/AnalysisQueue.cpp
    core/analysis/AnalysisService.cpp
    core/data/Database.cpp
    core/data/AnalysisStore.cpp
    core/library/TrackRepository.cpp
    core/library/TrackImporter.cpp
    core/library/LibraryScanner.cpp
    core/library/PlaylistManager.cpp
    core/library/HistoryManager.cpp
    core/library/MusicLibraryEngine.cpp
    core/performance/PerformanceEngine.cpp
    core/sampler/SamplerEngine.cpp)
target_include_directories(onda_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(onda_core PUBLIC sqlite3)
target_compile_features(onda_core PUBLIC cxx_std_20)

# --- UI core (JUCE-free): commands, shortcuts, theme, models --------------
# Widgets live in the app target; everything testable stays JUCE-free.
add_library(onda_ui STATIC
    ui/ThemeEngine.cpp
    ui/ShortcutManager.cpp
    ui/TrackTableModel.cpp
    ui/EngineCommands.cpp)
target_include_directories(onda_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(onda_ui PUBLIC onda_core)
target_compile_features(onda_ui PUBLIC cxx_std_20)

# --- controller core (JUCE-free): MIDI model, mapping, profiles ----------
add_library(onda_ctrl STATIC
    controller/MiniJson.cpp
    controller/MappingEngine.cpp
    controller/ControllerProfile.cpp
    controller/DeviceManager.cpp
    controller/FeedbackEngine.cpp
    controller/ControllerDiagnostics.cpp
    controller/ProfileManager.cpp
    controller/ControllerEngine.cpp
    controller/flx4/Flx4Inventory.cpp
    controller/flx4/Flx4Profile.cpp)
target_include_directories(onda_ctrl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(onda_ctrl PUBLIC onda_core onda_ui)
target_compile_features(onda_ctrl PUBLIC cxx_std_20)

# --- ffmpeg decoder -------------------------------------------------------
add_library(onda_decode STATIC
    platform/decode/FFmpegDecoder.cpp
    platform/metadata/FFmpegMetadataReader.cpp)
target_include_directories(onda_decode PRIVATE "${FFMPEG_INCLUDE_DIR}")
target_link_libraries(onda_decode PUBLIC onda_core ${ONDA_FFMPEG_LIBS})

# --- app ------------------------------------------------------------------
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
    app/ui/EngineBridge.cpp
    platform/midi/JuceMidi.cpp
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
    onda_ui
    onda_ctrl
    onda_decode
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

onda_copy_runtime_dlls(onda)
