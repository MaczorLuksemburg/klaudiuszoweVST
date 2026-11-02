#pragma once

#include "PluginProcessor.h"
#include <KlaudLookAndFeel.h>
#include <KlaudChoiceButtons.h>

namespace waveref::ui
{
    using APVTS = juce::AudioProcessorValueTreeState;
    using Channel = SpectrumAnalyser::Channel;

    const klaud::Palette& palette();

    void drawCaption (juce::Graphics&, const juce::String& text, juce::Rectangle<int> area,
                      juce::Justification justification = juce::Justification::centred);

    bool isAudioFile (const juce::File&);

    //==============================================================================
    // Editor settings: curve colours and visibility, FFT size, speed, smoothing, tilt, range...
    // They live in the project state, and the last used values are also kept in a small file in the user's
    // application data folder, which new instances start from.
    class UiSettings
    {
    public:
        explicit UiSettings (juce::ValueTree uiState);

        juce::Colour getColour (int channel) const;
        void setColour (int channel, juce::Colour);
        static juce::Colour getDefaultColour (int channel);
        void resetColours();

        bool isVisible (int channel) const;
        void setVisible (int channel, bool);

        int fftOrder() const    { return get ("fftOrder", 13); }
        float speed() const     { return get ("speed", 0.4f); }          // seconds
        int smoothing() const   { return get ("smooth", 6); }            // 1/N octave, 0 = none
        float tilt() const      { return get ("tilt", 4.5f); }           // dB per octave
        float span() const      { return get ("span", 90.0f); }          // dB shown
        float top() const       { return get ("top", 0.0f); }            // dB at the top edge: 0, 3, 6, 9 or 12
        bool fill() const       { return get ("fill", true); }
        bool diffView() const   { return get ("diffView", false); }      // mix minus reference on the big graph
        float diffSpan() const  { return get ("diffSpan", 12.0f); }      // +- dB shown in that view
        bool freeze() const     { return get ("freeze", false); }

        template <typename T>
        T get (const char* key, T fallback) const { return (T) tree.getProperty (key, fallback); }

        void set (const char* key, const juce::var& value);

        // Presets: everything above except freeze. Factory ones change only what they name; user ones are files
        // in the application data folder.
        static juce::StringArray getFactoryPresetNames();
        static juce::StringArray getUserPresetNames();
        juce::String getPresetName() const { return tree.getProperty ("preset", "Default").toString(); }
        bool loadPreset (const juce::String& name);
        juce::String saveUserPreset (const juce::String& name);   // returns the name it was saved under, or empty
        bool deleteUserPreset (const juce::String& name);

        // Where settings and presets are kept (tests point it at a temporary folder).
        static void setStorageFolder (const juce::File& folder);

    private:
        static juce::File getStorageFolder();
        static juce::File getPresetFolder();
        static juce::File getGlobalFile();
        std::unique_ptr<juce::XmlElement> toXml() const;
        void applyXml (const juce::XmlElement&);
        void loadGlobal();
        void saveGlobal() const;

        juce::ValueTree tree;
    };

    //==============================================================================
    // Four spectrum curves on top of each other (mix mid / side, ref mid / side) and the difference below.
    class SpectrumView : public juce::Component, private juce::Timer
    {
    public:
        SpectrumView (WaveRefProcessor&, UiSettings&);
        ~SpectrumView() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseExit (const juce::MouseEvent&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

        void refresh() { timerCallback(); }

    private:
        enum class BandDrag { none, create, left, right, move };

        void timerCallback() override;
        float xToFreq (float x, juce::Rectangle<float> plot) const;
        void setBand (float low, float high);
        void drawBand (juce::Graphics&, juce::Rectangle<float> plot) const;
        juce::Rectangle<float> getPlotArea() const;
        float freqToX (float freq, juce::Rectangle<float> plot) const;
        float dbToY (float db, juce::Rectangle<float> plot) const;
        void drawFrequencyGrid (juce::Graphics&, juce::Rectangle<float> plot) const;
        void drawGrid (juce::Graphics&, juce::Rectangle<float> plot) const;
        void drawDiffGrid (juce::Graphics&, juce::Rectangle<float> plot) const;
        void drawCurve (juce::Graphics&, int channel, juce::Rectangle<float> plot) const;
        void paintDiff (juce::Graphics&, juce::Rectangle<float> plot) const;

        WaveRefProcessor& processor;
        UiSettings& settings;

        std::array<std::vector<float>, SpectrumAnalyser::numChannels> curves;
        std::array<bool, SpectrumAnalyser::numChannels> present {};
        std::vector<float> diffMid, diffSide;
        bool diffPresent = false, diffSidePresent = false, blind = false;
        juce::Point<float> mouse { -1.0f, -1.0f };
        BandDrag bandDrag = BandDrag::none;
        float bandAnchor = 1000.0f, bandGrab = 0.0f;
    };

    //==============================================================================
    // Waveform of the selected reference with the loop region, playhead and zoom.
    // A small numbered button: click = jump to the cue (or set it if it is empty), right-click = menu.
    class CueButton : public juce::Component, public juce::SettableTooltipClient
    {
    public:
        explicit CueButton (int cueNumber) : number (cueNumber) {}

        void paint (juce::Graphics&) override;
        void mouseUp (const juce::MouseEvent&) override;

        std::function<void (bool popup)> onClick;
        bool isSet = false;

    private:
        int number;
    };

    class WaveformView : public juce::Component,
                         public juce::FileDragAndDropTarget,
                         private juce::Timer,
                         private juce::ChangeListener
    {
    public:
        WaveformView (WaveRefProcessor&, UiSettings&);
        ~WaveformView() override;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseMove (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

        bool isInterestedInFileDrag (const juce::StringArray& files) override;
        void filesDropped (const juce::StringArray& files, int x, int y) override;
        void fileDragEnter (const juce::StringArray&, int, int) override { dropHighlight = true; repaint(); }
        void fileDragExit (const juce::StringArray&) override { dropHighlight = false; repaint(); }

        void refresh() { timerCallback(); }

        // Keyboard and button entry points.
        void cueClicked (int index, bool popup);
        void setCueHere (int index);
        void togglePlay();

    private:
        enum class Drag { none, create, start, end, region, cue };

        void timerCallback() override;
        void changeListenerCallback (juce::ChangeBroadcaster*) override { needsRefresh = true; }

        int selectedSlot() const;
        juce::Rectangle<int> getWaveArea() const;
        double xToTime (float x) const;
        float timeToX (double seconds) const;
        void fitView();
        void zoomToLoop();
        void toggleLoop();
        void applyLoop (double start, double end);
        void playFrom (double seconds);
        int cueAt (juce::Point<float> position) const;
        void setBpm (double bpm);
        void tap();

        WaveRefProcessor& processor;
        UiSettings& settings;

        juce::TextButton loopButton { "LOOP" }, fitButton { "FIT" }, zoomButton { "ZOOM LOOP" }, playButton { "PLAY" };
        juce::Label bpmLabel;
        juce::TextButton tapButton { "TAP" }, halfButton { "/2" }, doubleButton { "x2" }, detectButton { "DETECT" },
                         barButton { "SET BAR START" }, gridButton { "GRID" };
        std::vector<double> taps;
        std::array<std::unique_ptr<CueButton>, 4> cueButtons;
        std::unique_ptr<APVTS::ButtonAttachment> playAttach;

        std::shared_ptr<const TrackData> track;
        int shownSlot = -1;
        double viewStart = 0.0, viewEnd = 1.0;
        Drag drag = Drag::none;
        int dragCue = -1;
        float cueGrab = 0.0f;
        double dragAnchor = 0.0, dragGrab = 0.0, dragStartLoop = 0.0, dragEndLoop = 0.0;
        int64_t lastPlayhead = -1;
        bool needsRefresh = true, dropHighlight = false;
    };

    //==============================================================================
    class SlotButton : public juce::Component, public juce::SettableTooltipClient, public juce::FileDragAndDropTarget
    {
    public:
        SlotButton (WaveRefProcessor&, int slotIndex);

        void paint (juce::Graphics&) override;
        void mouseUp (const juce::MouseEvent&) override;

        bool isInterestedInFileDrag (const juce::StringArray& files) override;
        void filesDropped (const juce::StringArray& files, int x, int y) override;

        void select();
        void chooseFile();

    private:
        WaveRefProcessor& processor;
        int index;
        std::unique_ptr<juce::FileChooser> chooser;
    };

    class SlotBar : public juce::Component, private juce::Timer, private juce::ChangeListener
    {
    public:
        explicit SlotBar (WaveRefProcessor&);
        ~SlotBar() override;

        void resized() override;

    private:
        void timerCallback() override { repaintAll(); }
        void changeListenerCallback (juce::ChangeBroadcaster*) override { repaintAll(); }
        void repaintAll();

        WaveRefProcessor& processor;
        std::array<std::unique_ptr<SlotButton>, numSlots> buttons;
    };

    //==============================================================================
    // Goniometer of mix and reference on top of each other, with correlation and width readouts.
    class StereoPanel : public juce::Component, private juce::Timer
    {
    public:
        StereoPanel (WaveRefProcessor&, UiSettings&);
        ~StereoPanel() override;

        void paint (juce::Graphics&) override;
        void refresh() { timerCallback(); }

    private:
        void timerCallback() override { repaint(); }
        void drawBar (juce::Graphics&, juce::Rectangle<float> area, const juce::String& name, bool reference);

        WaveRefProcessor& processor;
        UiSettings& settings;
        float scale = 1.0f;
    };

    //==============================================================================
    // Controls for the analyser: curve chips (click = show/hide, right-click = colour), FFT size, speed, ...
    class AnalyserBar : public juce::Component
    {
    public:
        explicit AnalyserBar (UiSettings&);

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseUp (const juce::MouseEvent&) override;
        void refresh();

    private:
        struct Chip { juce::Rectangle<int> bounds; int channel; };

        void showColourPicker (int channel);

        UiSettings& settings;
        std::array<Chip, SpectrumAnalyser::numChannels> chips;
        void rebuildRange (bool forDiff);

        juce::ComboBox fftBox, smoothBox, tiltBox, topBox, rangeBox;
        juce::Slider speedSlider;
        bool rangeShowsDiff = false;
        juce::ToggleButton fillButton { "Fill" }, diffButton { "Diff" }, freezeButton { "Freeze" };
    };

    //==============================================================================
    // Display presets: factory ones, and the user's own (SAVE keeps the current look under a name).
    class PresetBar : public juce::Component
    {
    public:
        PresetBar (UiSettings&, std::function<void()> onChanged);

        void resized() override;
        void refresh();

    private:
        void showSaveDialog();
        void showDeleteDialog();

        UiSettings& settings;
        std::function<void()> changed;
        juce::ComboBox box;
        juce::TextButton saveButton { "SAVE" }, deleteButton { "DELETE" };
        std::unique_ptr<juce::AlertWindow> dialog;
    };

    //==============================================================================
    class ABButton : public juce::Button
    {
    public:
        ABButton (UiSettings& s, int channelForColour, const juce::String& name)
            : juce::Button (name), settings (s), colourChannel (channelForColour) {}

        void paintButton (juce::Graphics&, bool highlighted, bool down) override;
        void setLabels (const juce::String& big, const juce::String& small, bool neutral);

    private:
        UiSettings& settings;
        int colourChannel;
        juce::String bigText, smallText;
        bool neutralColour = false;
    };

    class Knob : public juce::Component
    {
    public:
        Knob (APVTS&, const juce::String& paramId, const juce::String& title);
        void paint (juce::Graphics&) override;
        void resized() override;

        juce::Slider slider;

    private:
        juce::String title;
        std::unique_ptr<APVTS::SliderAttachment> attachment;
    };

    // Switch, loudness match, monitoring, sync and blind test.
    class ControlPanel : public juce::Component, private juce::Timer
    {
    public:
        ControlPanel (WaveRefProcessor&, UiSettings&);

        void paint (juce::Graphics&) override;
        void resized() override;

        void refresh() { timerCallback(); }

    private:
        void timerCallback() override;
        void setAb (bool ref);
        void showOptions();
        void guess (bool xIsRef);

        WaveRefProcessor& processor;
        UiSettings& settings;
        APVTS& apvts;

        ABButton mixButton, refButton;
        juce::ToggleButton matchButton { "MATCH" }, monoButton { "MONO" }, sideButton { "SIDE" }, deltaButton { "DELTA" },
                           bandButton { "BAND" }, blindButton { "BLIND" }, keyLockButton { "KEY LOCK" };
        klaud::ChoiceButtons matchMode, syncMode;
        juce::TextButton optionsButton { "..." }, guessX { "X is the ref" }, guessY { "Y is the ref" }, alignButton { "ALIGN" };
        Knob trimKnob, freqKnob, widthKnob, offsetKnob, beatKnob, nudgeKnob;

        std::unique_ptr<APVTS::ButtonAttachment> matchAttach, monoAttach, sideAttach, deltaAttach, bandAttach, blindAttach, keyLockAttach;

        juce::String matchText, resultText, syncText;
        bool wasBlind = false;
    };
}
