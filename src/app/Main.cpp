#include <juce_gui_extra/juce_gui_extra.h>

#include "MainComponent.h"
#include "OndaLookAndFeel.h"
#include "../platform/audio/AudioEngine.h"
#include "../platform/config/Config.h"
#include "../platform/log/Log.h"

namespace onda {

class OndaApplication : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override    { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return false; }

    void initialise(const juce::String&) override {
        log::init();
        log::info("Onda " + getApplicationVersion() + " starting");

        juce::Desktop::getInstance().setDefaultLookAndFeel(&lookAndFeel_);

        config_.load();
        engine_ = std::make_unique<audio::AudioEngine>(config_);
        engine_->start();

        mainWindow_ = std::make_unique<MainWindow>(getApplicationName(), *engine_);
    }

    void shutdown() override {
        mainWindow_ = nullptr; // UI first: it references the engine
        if (engine_) {
            engine_->shutdown();
            engine_.reset();
        }
        config_.save();
        juce::Desktop::getInstance().setDefaultLookAndFeel(nullptr);
        log::info("Onda shutdown");
    }

    void systemRequestedQuit() override { quit(); }

private:
    class MainWindow : public juce::DocumentWindow {
    public:
        MainWindow(const juce::String& name, audio::AudioEngine& engine)
            : juce::DocumentWindow(name,
                                   OndaLookAndFeel::kBackground,
                                   juce::DocumentWindow::allButtons) {
            setUsingNativeTitleBar(true);
            setContentOwned(new MainComponent(engine), true);
            setResizable(true, true);
            setResizeLimits(960, 600, 10000, 10000);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
        }

        void closeButtonPressed() override {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
    };

    OndaLookAndFeel lookAndFeel_;
    Config config_;
    std::unique_ptr<audio::AudioEngine> engine_;
    std::unique_ptr<MainWindow> mainWindow_;
};

} // namespace onda

START_JUCE_APPLICATION(onda::OndaApplication)
