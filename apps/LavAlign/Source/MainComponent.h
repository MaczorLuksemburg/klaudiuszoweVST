#pragma once

#include "Preview.h"
#include "Session.h"

#include <KlaudChoiceButtons.h>
#include <KlaudLookAndFeel.h>

#include <juce_audio_utils/juce_audio_utils.h>

namespace lavalign
{
    class DelayGraph;
    class ChannelList;

    // The whole app window: takes on the left, the selected take's channels (roles and results), its delay
    // graph with a before/after preview, and the settings and export at the bottom.
    class MainComponent : public juce::Component,
                          public juce::FileDragAndDropTarget,
                          private juce::ListBoxModel,
                          private juce::Timer
    {
    public:
        MainComponent();
        ~MainComponent() override;

        void addFiles (const juce::Array<juce::File>& files);

        void paint (juce::Graphics&) override;
        void paintOverChildren (juce::Graphics&) override;
        void resized() override;
        bool keyPressed (const juce::KeyPress& key) override;

        bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
        void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
        void fileDragExit (const juce::StringArray&) override { dragOver = false; repaint(); }
        void filesDropped (const juce::StringArray& files, int, int) override;

        // Tests: analyse the selected take on the calling thread and wait for it.
        void analyseSelectedNow();
        void selectTake (int index);

        static juce::Colour lavColour (int index);

    private:
        friend class ChannelList;
        friend class DelayGraph;

        // ListBoxModel
        int getNumRows() override;
        void paintListBoxItem (int row, juce::Graphics&, int width, int height, bool selected) override;
        void selectedRowsChanged (int lastRowSelected) override;
        void deleteKeyPressed (int lastRowSelected) override;

        void timerCallback() override;

        Settings currentSettings() const;
        ExportOptions currentExportOptions() const;
        void settingsChanged();
        void rolesChanged();
        void scheduleAnalysis();
        void startAnalysis();
        void analysisFinished (int generation, juce::String error, std::vector<LavOutcome> outcomes, std::vector<float> boom);
        void buildPreview();
        void startExport();
        void exportStep();
        void updateButtons();
        void setStatus (const juce::String& text, bool warning = false);
        void saveSettings();
        void loadSettings();
        juce::String summaryFor (const Take& take) const;

        klaud::LookAndFeel lnf;
        Session session;
        Preview preview;

        int selectedTake = -1;

        // results per take signature + roles + settings (curves and qualities only)
        struct Summary { juce::String key; std::vector<LavOutcome> outcomes; juce::String error; };
        std::map<juce::String, Summary> summaries;
        juce::String summaryKey (const Take& take) const;

        // the selected take's audio (for the preview and the graph)
        std::vector<LavOutcome> outcomes;
        std::vector<float> boomTimeline;
        juce::String outcomesKey;
        std::vector<float> boomEnvelope;  // peak per 1/2000 of the take

        // background work
        class Worker;
        std::unique_ptr<Worker> worker;
        std::atomic<int> generation { 0 };
        bool analysing = false;
        int analysisPending = 0;  // timer ticks left before a scheduled analysis starts
        double progress = 0.0;
        juce::ProgressBar progressBar { progress };

        // export state
        bool exporting = false;
        int exportIndex = 0, exportFiles = 0;
        juce::StringArray exportErrors;

        // widgets
        juce::TextButton addButton { "Add files..." }, clearButton { "Clear" };
        juce::ListBox takeList { "takes", this };
        std::unique_ptr<ChannelList> channelList;
        std::unique_ptr<DelayGraph> graph;

        juce::TextButton playButton { "Play" };
        klaud::ChoiceButtons abButtons, listenButtons;
        juce::Label timeLabel;

        juce::Slider phaseSlider;
        juce::Label phaseLabel, rangeLabel, trackLabel, outputLabel, namesLabel;
        juce::ComboBox rangeBox;
        klaud::ChoiceButtons trackButtons, outputButtons, namesButtons;
        juce::TextButton folderButton, exportButton { "Align & export all" };
        juce::File exportFolder;  // empty: "Aligned" next to the sources
        juce::Label statusLabel;
        juce::TextButton openFolderButton { "Show files" };
        juce::File lastExportFolder;  // where the last export wrote ("Show files")
        bool dragOver = false;

        std::unique_ptr<juce::FileChooser> chooser;
        juce::ApplicationProperties properties;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
    };

    // Tests: renders the window with a synthetic take to PNG files in 'folder'.
    int renderSnapshots (const juce::File& folder);
}
