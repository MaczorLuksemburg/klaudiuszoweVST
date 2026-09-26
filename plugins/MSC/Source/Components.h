#pragma once

#include "PluginProcessor.h"
#include <KlaudLookAndFeel.h>

namespace msc::ui
{
    using APVTS = juce::AudioProcessorValueTreeState;

    namespace colours
    {
        const juce::Colour input  { 0xff4da6ff };   // blue
        const juce::Colour dynPan { 0xff35d0c0 };   // teal
        const juce::Colour haas   { 0xff8c7bff };   // violet
        const juce::Colour chorus { 0xffe07be0 };   // orchid
        const juce::Colour image  { 0xffffb454 };   // amber
    }

    const klaud::Palette& palette();

    void drawCaption (juce::Graphics&, const juce::String& text, juce::Rectangle<int> area,
                      juce::Justification justification = juce::Justification::centred);

    //==============================================================================
    // Rotary knob with a caption above and the value below.
    class Knob : public juce::Component
    {
    public:
        Knob (APVTS&, const juce::String& paramId, const juce::String& title, juce::Colour accent,
              bool arcFromStart = false);

        void paint (juce::Graphics&) override;
        void resized() override;

        juce::Slider slider;

    private:
        static constexpr int titleHeight = 16;

        juce::String title;
        std::unique_ptr<APVTS::SliderAttachment> attachment;
    };

    //==============================================================================
    class PowerButton : public juce::ToggleButton
    {
    public:
        explicit PowerButton (juce::Colour accentColour) : accent (accentColour) {}
        void paintButton (juce::Graphics&, bool highlighted, bool down) override;

    private:
        juce::Colour accent;
    };

    //==============================================================================
    // A module panel: power button + title, greyed out while off. Touching any
    // control inside a module that's off switches it on.
    class ModulePanel : public juce::Component
    {
    public:
        ModulePanel (APVTS&, const char* onParamId, const juce::String& title, const juce::String& subtitle, juce::Colour accent);
        ~ModulePanel() override;

        void paint (juce::Graphics&) override;
        void paintOverChildren (juce::Graphics&) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    protected:
        virtual void layoutContent (juce::Rectangle<int> area) = 0;
        virtual void paintContent (juce::Graphics&) {}

        juce::Rectangle<int> getContentBounds() const;

        APVTS& apvts;
        const juce::Colour accent;

    private:
        static constexpr int headerHeight = 40;

        juce::String title, subtitle;
        PowerButton power;
        std::unique_ptr<APVTS::ButtonAttachment> powerAttachment;
        std::unique_ptr<juce::ParameterAttachment> onWatcher;
        bool isOn = false;
    };

    //==============================================================================
    // Filter curve over a live spectrum. Drag horizontally to move the cutoff.
    class FilterDisplay : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
    {
    public:
        FilterDisplay (APVTS&, const char* cutoffId, const char* shapeId, const char* slopeId,
                       SpectrumAnalyzer&, juce::Colour accent);
        ~FilterDisplay() override;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

    private:
        void timerCallback() override;
        float xForFreq (float freq) const;
        float freqForX (float x) const;

        SpectrumAnalyzer& analyzer;
        juce::Colour accent;
        juce::RangedAudioParameter& cutoffParam;
        std::atomic<float>* cutoff;
        std::atomic<float>* shape;
        std::atomic<float>* slope;
        juce::ParameterAttachment cutoffGesture;
        float lastCutoff = -1.0f, lastShape = -1.0f, lastSlope = -1.0f;
    };

    //==============================================================================
    // FreeClip-style view of the dynamic-pan modulation: scrolling waveform before (dim)
    // and after (accent) the mod clipper, plus the clipper's transfer curve.
    class ModScopeDisplay : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
    {
    public:
        ModScopeDisplay (APVTS&, ModScope&, juce::Colour accent);
        ~ModScopeDisplay() override;

        void paint (juce::Graphics&) override;

    private:
        void timerCallback() override;

        ModScope& scope;
        juce::Colour accent;
        std::atomic<float>* clipMode;
        float lastClipMode = -1.0f;
    };

    //==============================================================================
    // Shape slider (LP / BP / HP), slope menu, display and cutoff slider.
    class FilterSection : public juce::Component
    {
    public:
        FilterSection (APVTS&, const char* shapeId, const char* cutoffId, const char* slopeId,
                       SpectrumAnalyzer&, juce::Colour accent);

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        juce::Slider shape, cutoff;
        juce::ComboBox slope;
        FilterDisplay display;

        std::unique_ptr<APVTS::SliderAttachment> shapeAttachment, cutoffAttachment;
        std::unique_ptr<APVTS::ComboBoxAttachment> slopeAttachment;
    };

    //==============================================================================
    class InputModule : public ModulePanel
    {
    public:
        InputModule (APVTS&, SpectrumAnalyzer&);

    private:
        void layoutContent (juce::Rectangle<int>) override;
        void paintContent (juce::Graphics&) override;

        juce::ComboBox wetSource, drySource;
        std::unique_ptr<APVTS::ComboBoxAttachment> wetAttachment, dryAttachment;
        FilterSection filter;
        juce::Rectangle<int> wetCaption, dryCaption;
    };

    class DynamicPanModule : public ModulePanel
    {
    public:
        DynamicPanModule (APVTS&, SpectrumAnalyzer&, ModScope&);

    private:
        void layoutContent (juce::Rectangle<int>) override;
        void paintContent (juce::Graphics&) override;
        float maxPercent() const;
        void setUpNumberBox (juce::Slider&, const juce::String& tooltip);
        void setUpCombo (juce::ComboBox&, const char* paramId, const juce::String& tooltip);

        Knob amount;
        juce::Slider maxBox, thresholdBox;
        juce::ComboBox source, comp, clip, makeup;
        std::vector<std::unique_ptr<APVTS::SliderAttachment>> sliderAttachments;
        std::vector<std::unique_ptr<APVTS::ComboBoxAttachment>> comboAttachments;
        std::unique_ptr<juce::ParameterAttachment> maxWatcher;
        ModScopeDisplay scope;
        FilterSection filter;
        juce::Rectangle<int> sourceCaption, compCaption, thresholdCaption, maxCaption, clipCaption, makeupCaption,
                             scopeCaption, filterCaption;
        int dividerX = 0;
    };

    class HaasModule : public ModulePanel
    {
    public:
        explicit HaasModule (APVTS&);

    private:
        void layoutContent (juce::Rectangle<int>) override;
        void paintContent (juce::Graphics&) override;

        Knob left, right;
        juce::TextButton invertLeft, invertRight;
        std::unique_ptr<APVTS::ButtonAttachment> invertLeftAttachment, invertRightAttachment;
        juce::Rectangle<int> polarityCaption, hintArea;
    };

    class ChorusModule : public ModulePanel
    {
    public:
        explicit ChorusModule (APVTS&);

    private:
        void layoutContent (juce::Rectangle<int>) override;
        void paintContent (juce::Graphics&) override;

        std::array<juce::TextButton, 3> modes;
        std::unique_ptr<juce::ParameterAttachment> modeAttachment;
        Knob depth, width, tone, mix;
        juce::Rectangle<int> modeCaption;
    };

    class ImageModule : public ModulePanel
    {
    public:
        explicit ImageModule (APVTS&);

    private:
        void layoutContent (juce::Rectangle<int>) override;

        Knob balance, mid, side;
    };

    //==============================================================================
    class PresetBar : public juce::Component, private juce::Timer
    {
    public:
        explicit PresetBar (PresetManager&);

        void resized() override;

    private:
        void timerCallback() override;
        void refresh();
        void step (int delta);
        void showSaveDialog();

        PresetManager& presets;
        juce::TextButton previous { "<" }, next { ">" }, save { "SAVE" };
        juce::ComboBox list;
        juce::String shownName;
        std::unique_ptr<juce::AlertWindow> saveDialog;
    };
}
