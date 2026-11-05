#include "MainComponent.h"
#include "Platform.h"

// LavAlign: standalone app (no DAW needed) that phase-aligns lavalier mics to the boom.
class LavAlignApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override    { return "LavAlign"; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override          { return true; }

    void initialise (const juce::String& commandLine) override
    {
        lavalign::requestFullSpeed();
        window = std::make_unique<MainWindow>();
        openFromCommandLine (commandLine);
    }

    void shutdown() override { window.reset(); }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String& commandLine) override { openFromCommandLine (commandLine); }

private:
    void openFromCommandLine (const juce::String& commandLine)
    {
        juce::Array<juce::File> files;
        for (auto& arg : juce::StringArray::fromTokens (commandLine, true))
        {
            const auto path = arg.unquoted();
            if (juce::File::isAbsolutePath (path) && juce::File (path).exists()) files.add (juce::File (path));
        }
        if (! files.isEmpty() && window != nullptr) window->content->addFiles (files);
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow() : juce::DocumentWindow ("LavAlign", juce::Colour (0xff14171c), juce::DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            content = new lavalign::MainComponent();
            setContentOwned (content, true);
            setResizable (true, true);
            setResizeLimits (980, 620, 4000, 3000);
            centreWithSize (getWidth(), getHeight());
            setVisible (true);
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

        lavalign::MainComponent* content = nullptr;
    };

    std::unique_ptr<MainWindow> window;
};

START_JUCE_APPLICATION (LavAlignApplication)
