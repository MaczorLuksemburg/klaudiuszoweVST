#pragma once

#include "BandDisplay.h"
#include "HistoryView.h"
#include "CurveEditor.h"

// Everything is laid out at a fixed base size and scaled as a whole when the window resizes.
class DynMapMainView : public juce::Component, private juce::Timer
{
public:
    static constexpr int baseWidth = 1320;
    static constexpr int baseHeight = 800;

    DynMapMainView (DynMapProcessor&, klaud::LookAndFeel&);
    ~DynMapMainView() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    int getSelectedStage() const { return selectedStage; }
    void selectStage (int stage);

private:
    void timerCallback() override;
    void updateStageControls();
    void updateDetectorView();
    void updateHistoryView();
    void undoCurve (bool redo);
    bool keyPressed (const juce::KeyPress&) override;

    // Declared first so the look and feel is in place before any child component is built.
    struct LookAndFeelSetter
    {
        LookAndFeelSetter (juce::Component& c, juce::LookAndFeel& lf) { c.setLookAndFeel (&lf); }
    };

    DynMapProcessor& processor;
    LookAndFeelSetter lookAndFeelSetter;
    int selectedStage = dynmap::bandStage (0);
    juce::Colour stageAccent;
    juce::String stageTitle;
    bool sidechainMissing = false;   // the selected stage listens to a sidechain that isn't arriving

    dynmap::ui::PresetBar presetBar;
    dynmap::ui::Choice quality { "" }, phase { "" };

    dynmap::ui::StageTab inputTab, masterTab;
    dynmap::ui::SidechainTab sidechainTab;
    dynmap::ui::BandDisplay bandDisplay;
    dynmap::ui::HistoryView history;
    juce::TextButton historyToggle { "History" };
    juce::ShapeButton undoButton { "Undo", {}, {}, {} }, redoButton { "Redo", {}, {}, {} };

    // Selected stage.
    dynmap::ui::CurveEditor levelEditor, transientEditor;
    dynmap::ui::Choice mode { "" };
    dynmap::ui::Toggle bypass { "Bypass" }, solo { "S" }, mute { "M" };
    juce::TextButton linearScale { "Linear" };
    dynmap::ui::Knob pre { "PRE" }, post { "POST" }, mix { "MIX", true }, width { "WIDTH" };
    dynmap::ui::Choice satType { "SATURATION" }, satPos { "POSITION" };
    dynmap::ui::Knob drive { "DRIVE", true };
    dynmap::ui::Knob attack { "ATTACK", true }, hold { "HOLD", true }, release { "RELEASE", true }, relShape { "REL SHAPE", true };
    dynmap::ui::Knob release2 { "REL 2", true };
    dynmap::ui::Knob rms { "RMS", true }, link { "LINK", true }, trTime { "TRANS TIME", true }, smooth { "SMOOTH", true };
    dynmap::ui::Knob maxBoost { "MAX BOOST", true }, maxCut { "MAX CUT", true }, scFilter { "DET HP", true };
    dynmap::ui::Choice lookahead { "LOOKAHEAD" }, stereo { "STEREO" }, scSource { "DETECT FROM" }, relLaw { "REL MODE" };
    dynmap::ui::Choice attLaw { "ATT MODE" };
    dynmap::ui::DetectorStyleBank styleBank;
    dynmap::ui::DetectorStylePicker detectorStyles;
    dynmap::ui::DetectorStyleMenu styleMenu;
    juce::TextButton advanced { "Advanced" };

    // Global and output.
    dynmap::ui::Knob amount { "AMOUNT" }, time { "TIME" }, globalMix { "MIX", true }, inGain { "IN" }, outGain { "OUT" };
    dynmap::ui::Knob lowCut { "LOW CUT", true };
    dynmap::ui::Choice clip { "CLIPPER" };
    dynmap::ui::Toggle limiter { "Limiter" }, autoGain { "Auto gain" }, delta { "Delta" };
    dynmap::ui::Knob ceiling { "CEILING" }, limRelease { "RELEASE", true };
    dynmap::ui::OutputMeter meter;
    dynmap::ui::LoudnessReadout readout;

    juce::Rectangle<int> levelPanel, transientPanel, stagePanel, detectorPanel, globalPanel, outputPanel;
    juce::Rectangle<int> sidechainNote;   // where the "no sidechain signal" warning goes (set in resized)

    juce::TooltipWindow tooltips { this, 700 };
};

class DynMapEditor : public juce::AudioProcessorEditor
{
public:
    explicit DynMapEditor (DynMapProcessor&);
    ~DynMapEditor() override;

    void resized() override;

private:
    DynMapProcessor& processor;
    klaud::LookAndFeel lookAndFeel { dynmap::ui::palette() };
    DynMapMainView view;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynMapEditor)
};
