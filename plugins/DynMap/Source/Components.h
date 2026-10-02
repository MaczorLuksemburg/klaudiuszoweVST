#pragma once

#include "PluginProcessor.h"
#include <KlaudLookAndFeel.h>

namespace dynmap::ui
{
    using APVTS = juce::AudioProcessorValueTreeState;

    namespace colours
    {
        const juce::Colour input  { 0xff5aa9ff };
        const juce::Colour master { 0xfff2a93b };
        const juce::Colour boost  { 0xfff2a93b };
        const juce::Colour cut    { 0xff4fc3f7 };
        const juce::Colour sidechain { 0xffc58bff };
    }

    const klaud::Palette& palette();

    // Colour of a stage: input blue, master amber, bands along a gradient by position (low to high).
    juce::Colour stageColour (const DynMapProcessor&, int stage);
    juce::String stageLabel (const DynMapProcessor&, int stage);   // "Input", "Band 2" (by position), "Master"

    void drawCaption (juce::Graphics&, const juce::String& text, juce::Rectangle<int> area,
                      juce::Justification justification = juce::Justification::centred);

    // Panel background with a small uppercase title.
    void drawPanel (juce::Graphics&, juce::Rectangle<int> area, const juce::String& title = {}, juce::Colour titleColour = {});

    // Sets a parameter from the UI as one undo-able gesture.
    void setParameter (APVTS&, const juce::String& id, float value);

    //==============================================================================
    // Rotary knob with a caption above and the value below; can be re-attached to another parameter.
    class Knob : public juce::Component
    {
    public:
        Knob (const juce::String& title, bool arcFromStart = false);

        void attach (APVTS&, const juce::String& paramId);
        void setAccent (juce::Colour);

        void paint (juce::Graphics&) override;
        void resized() override;

        juce::Slider slider;

    private:
        static constexpr int titleHeight = 15;

        juce::String title;
        std::unique_ptr<APVTS::SliderAttachment> attachment;
    };

    // Combo box with a caption above; re-attachable.
    class Choice : public juce::Component
    {
    public:
        explicit Choice (const juce::String& title);

        void attach (APVTS&, const juce::String& paramId);

        void paint (juce::Graphics&) override;
        void resized() override;

        juce::ComboBox box;

    private:
        juce::String title;
        std::unique_ptr<APVTS::ComboBoxAttachment> attachment;
    };

    // Toggle text button; re-attachable.
    class Toggle : public juce::TextButton
    {
    public:
        explicit Toggle (const juce::String& text);
        void attach (APVTS&, const juce::String& paramId);
        void setAccent (juce::Colour);

    private:
        std::unique_ptr<APVTS::ButtonAttachment> attachment;
    };

    //==============================================================================
    class PresetBar : public juce::Component, private juce::Timer
    {
    public:
        explicit PresetBar (PresetManager&);

        void resized() override;
        void refresh();

    private:
        void timerCallback() override;
        void step (int delta);
        void showSaveDialog();

        PresetManager& presets;
        juce::TextButton previous { "<" }, next { ">" }, save { "Save" };
        juce::ComboBox list;
        juce::String shownName;
        std::unique_ptr<juce::AlertWindow> saveDialog;
    };

    //==============================================================================
    // One-click detector settings. Choosing a style sets the stage's detector parameters; the
    // highlighted style is whichever one the current settings match ("Custom" otherwise).
    struct DetectorStyle
    {
        const char* name;
        const char* description;
        float attack, hold, release, relShape, rms, link;
        int lookahead;
        float smooth, trTime;
    };

    const std::vector<DetectorStyle>& detectorStyles();

    class DetectorStylePicker : public juce::Component, private juce::Timer
    {
    public:
        explicit DetectorStylePicker (DynMapProcessor&);

        void setStage (int stage, juce::Colour accent);
        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void timerCallback() override;
        int matchingStyle() const;
        void apply (int style);

        DynMapProcessor& processor;
        static constexpr int buttonHeight = 38, columns = 3;

        int stage = inputStage;
        int shown = -2;
        juce::OwnedArray<juce::TextButton> buttons;
    };

    // Input/output peak meters.
    class OutputMeter : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
    {
    public:
        explicit OutputMeter (DynMapProcessor&);
        void paint (juce::Graphics&) override;

    private:
        void timerCallback() override;

        DynMapProcessor& processor;
        std::array<float, 2> inDb { -100.0f, -100.0f }, outDb { -100.0f, -100.0f };
    };

    // Short-term loudness, true-peak maximum (click to reset), limiter and auto-gain activity.
    class LoudnessReadout : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
    {
    public:
        explicit LoudnessReadout (DynMapProcessor&);
        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;

    private:
        void timerCallback() override;

        DynMapProcessor& processor;
        float lufs = -100.0f, truePeakDb = -100.0f, limiterDb = 0.0f, autoGainDb = 0.0f;
    };
}
