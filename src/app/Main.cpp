#include <juce_gui_extra/juce_gui_extra.h>

#include "MainComponent.h"
#include "../platform/audio/AudioEngine.h"
#include "../platform/audio/JuceAudioBackend.h"
#include "../platform/config/Config.h"
#include "../platform/decode/FFmpegDecoder.h"
#include "../platform/log/Log.h"
#include "../platform/metadata/FFmpegMetadataReader.h"
#include "../core/analysis/AnalysisService.h"
#include "../core/data/Database.h"
#include "../core/data/PerformanceStore.h"
#include "../core/library/MusicLibraryEngine.h"
#include "../core/performance/PerformanceEngine.h"
#include "../core/rec/RecordingEngine.h"
#include "../core/rec/RecordingRepository.h"
#include "../core/sampler/SampleRepository.h"
#include "../core/sampler/SamplerEngine.h"

namespace onda {

class OndaApplication : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise(const juce::String&) override {
        log::init();
        log::info("Onda " + getApplicationVersion() + " starting");

        config_.load();

        const auto dbDir = config_.file().getParentDirectory();
        db_ = std::make_unique<Database>();
        if (!db_->open((dbDir.getChildFile("onda.db").getFullPathName()
                        .toStdString())))
            log::error("Cannot open analysis database");

        // Phase 14 §3: worker count is configurable — 0 = auto
        // (half the cores, capped at 4).
        const int anaWorkers = (int)config_.get(
            juce::Identifier("analysis.workers"), 0);
        analysis_ = std::make_unique<AnalysisService>(
            [] { return std::make_unique<FFmpegDecoder>(); }, *db_,
            anaWorkers);
        analysis_->addListener([](const AnalysisQueue::Notification& n) {
            if (n.event == AnalysisQueue::Event::Failed)
                log::error("Analysis failed: " + juce::String(n.path) + " — " +
                           juce::String(n.error));
        });

        engine_ = std::make_unique<audio::AudioEngine>(
            config_, [] { return std::make_unique<FFmpegDecoder>(); });
        engine_->start();

        perfStore_ = std::make_unique<PerformanceStore>(*db_);
        perf_ = std::make_unique<PerformanceEngine>();
        perf_->setStore(perfStore_.get());
        perf_->setCommandSink([this](const rt::Command& c) {
            engine_->commands().tryPush(c);
        });
        for (int i = 0; i < audio::AudioEngine::kNumDecks; ++i) {
            auto* d = engine_->deckRaw(i);
            perf_->setGridSlot(i, &d->pendingGrid_, &d->retiredGrid_,
                               &engine_->deckStats(i));
        }

        // Phase 7 library on the same database; metadata via ffmpeg,
        // analysis wired so imports enqueue automatically.
        lib_ = std::make_unique<library::MusicLibraryEngine>();
        if (!lib_->open((dbDir.getChildFile("onda.db")
                            .getFullPathName().toStdString())))
            log::error("Cannot open library database");
        else {
            lib_->setMetadataProvider(
                [](const std::string& p, library::TrackMeta& m) {
                    return FFmpegMetadataReader::read(p, m);
                });
            lib_->setAnalysisService(analysis_.get());
            lib_->startWorker();
        }

        // Phase 11: sampler on the shared database + decoder factory.
        // RT engine renders into mixer channel 4 (aux strip).
        sampleRepo_ = std::make_unique<sampler::SampleRepository>(*db_);
        if (db_->isOpen()) sampleRepo_->init();
        sampler_ = std::make_unique<sampler::SamplerEngine>();
        sampler_->init(
            [] { return std::make_unique<FFmpegDecoder>(); });
        sampler_->attachRepository(sampleRepo_.get());
        sampler_->setPorts({ &engine_->commands(),
            [](const std::string& t, const std::string& d, int lvl) {
                if (lvl >= 2)
                    log::warn("Sampler: " + juce::String(t) + " — " +
                              juce::String(d));
            } });
        engine_->attachSampler(&sampler_->rt());
        if (db_->isOpen()) sampler_->loadAll();

        // Phase 12: session recorder on the shared database. The RT
        // tap attaches to AudioEngine inside EngineBridge; here we own
        // persistence + lifecycle. Recordings land in
        // <app-data>/Onda/Recordings unless configured otherwise.
        recRepo_ = std::make_unique<rec::RecordingRepository>(*db_);
        if (db_->isOpen()) recRepo_->init();
        recorder_ = std::make_unique<rec::RecordingEngine>(
            db_->isOpen() ? recRepo_.get() : nullptr);
        recorder_->setAppVersion(getApplicationVersion().toStdString());
        {
            const auto defDir =
                dbDir.getChildFile("Recordings").getFullPathName();
            const auto cfgDir = config_.get(
                juce::Identifier("recording.dir"), defDir);
            recorder_->setOutputDir(cfgDir.toString().toStdString());
        }

        // Phase 13: device layer over JUCE's AudioDeviceManager —
        // enumerate/select/switch/recover on the control thread; the
        // RT callback is untouched.
        juceBackend_ = std::make_unique<audio::JuceAudioBackend>(
            engine_->deviceManager());
        adevice_ = std::make_unique<adevice::AudioDeviceEngine>(
            *juceBackend_);

        mainWindow_ = std::make_unique<MainWindow>(getApplicationName(),
                                                 *engine_, *analysis_,
                                                 *perf_, *lib_, config_,
                                                 sampler_.get(),
                                                 recorder_.get(),
                                                 adevice_.get());
    }

    void shutdown() override {
        mainWindow_ = nullptr; // UI first: it references the engine
        adevice_.reset();      // device engine only holds refs
        juceBackend_.reset();
        if (recorder_) {
            recorder_->shutdown();       // finalize any live session
            recorder_.reset();
            recRepo_.reset();
        }
        if (sampler_) {
            sampler_->stopAll();
            if (db_ && db_->isOpen()) sampler_->saveAll();
        }
        if (engine_) {
            engine_->shutdown();
            engine_->attachSampler(nullptr);   // RT engine gone
            engine_->attachRecorder(nullptr);
            engine_.reset();
        }
        sampler_.reset();
        sampleRepo_.reset();
        if (lib_) { lib_->stopWorker(); lib_->close(); lib_.reset(); }
        analysis_.reset();   // workers join before the DB closes
        db_.reset();
        config_.save();
        log::info("Onda shutdown");
    }

    void systemRequestedQuit() override { quit(); }

private:
    class MainWindow : public juce::DocumentWindow {
    public:
        MainWindow(const juce::String& name, audio::AudioEngine& engine,
                   AnalysisService& analysis, PerformanceEngine& perf,
                   library::MusicLibraryEngine& lib, Config& config,
                   sampler::SamplerEngine* sampler,
                   rec::RecordingEngine* recorder,
                   adevice::AudioDeviceEngine* adevice)
            : juce::DocumentWindow(name,
                                   juce::Colour(0xff0d0f12),
                                   juce::DocumentWindow::allButtons) {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(engine, analysis, perf,
                                              lib, config, sampler,
                                              recorder, adevice),
                            true);
            setResizable(true, true);
            setResizeLimits(960, 540, 10000, 10000);
            centreWithSize(1440, 860);
            setVisible(true);
        }

        void closeButtonPressed() override {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

    Config config_;
    std::unique_ptr<Database> db_;
    std::unique_ptr<AnalysisService> analysis_;
    std::unique_ptr<PerformanceStore> perfStore_;
    std::unique_ptr<PerformanceEngine> perf_;
    std::unique_ptr<library::MusicLibraryEngine> lib_;
    std::unique_ptr<sampler::SampleRepository> sampleRepo_;
    std::unique_ptr<sampler::SamplerEngine>  sampler_;
    std::unique_ptr<rec::RecordingRepository> recRepo_;
    std::unique_ptr<rec::RecordingEngine>  recorder_;
    std::unique_ptr<audio::JuceAudioBackend> juceBackend_;
    std::unique_ptr<adevice::AudioDeviceEngine> adevice_;
    std::unique_ptr<audio::AudioEngine> engine_;
    std::unique_ptr<MainWindow> mainWindow_;
};

} // namespace onda

START_JUCE_APPLICATION(onda::OndaApplication)
