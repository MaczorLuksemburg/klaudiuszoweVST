#include "PluginEditor.h"

namespace
{
    constexpr int headerHeight = 62;
    constexpr int margin = 12;
    constexpr int gap = 10;

    // Module widths in chain order: input, dynamic pan, Haas, chorus, image.
    constexpr int moduleWidths[] { 300, 350, 180, 206, 140 };

    const juce::Identifier uiWidthId { "uiWidth" };
}

//==============================================================================
MscMainView::MscMainView (MscProcessor& p, klaud::LookAndFeel& lookAndFeel)
    : lookAndFeelSetter (*this, lookAndFeel),
      presetBar (p.presets),
      inputModule (p.apvts, p.inputAnalyzer),
      dynPanModule (p.apvts, p.dynPanAnalyzer, p.modScope),
      haasModule (p.apvts),
      chorusModule (p.apvts),
      imageModule (p.apvts)
{
    for (auto* c : std::initializer_list<juce::Component*> { &presetBar, &inputModule, &dynPanModule,
                                                             &haasModule, &chorusModule, &imageModule })
        addAndMakeVisible (c);

    setSize (baseWidth, baseHeight);
}

MscMainView::~MscMainView()
{
    setLookAndFeel (nullptr);
}

void MscMainView::paint (juce::Graphics& g)
{
    const auto& pal = msc::ui::palette();

    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff111b2b), 0.0f, 0.0f,
                                             pal.background, 0.0f, (float) getHeight(), false));
    g.fillAll();

    // Header.
    g.setColour (pal.text);
    g.setFont (klaud::font (30.0f, true));
    g.drawText ("MSC", 22, 12, 90, 36, juce::Justification::centredLeft, false);

    g.setColour (msc::ui::colours::input.withAlpha (0.85f));
    g.setFont (klaud::font (12.5f));
    g.drawText ("multistage stereo control", 96, 25, 220, 18, juce::Justification::centredLeft, false);

    g.setColour (pal.textDim);
    g.setFont (klaud::font (11.0f));
    g.drawText ("Maki plugins", getWidth() - 140, 22, 118, 18, juce::Justification::centredRight, false);

    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.fillRect (0, headerHeight - 1, getWidth(), 1);

    // Signal-flow chevrons between the modules.
    const float cy = (float) inputModule.getBounds().getCentreY();
    g.setColour (pal.textDim.withAlpha (0.45f));

    for (auto* module : std::initializer_list<juce::Component*> { &inputModule, &dynPanModule, &haasModule, &chorusModule })
    {
        const float cx = (float) module->getRight() + gap * 0.5f;
        juce::Path chevron;
        chevron.startNewSubPath (cx - 1.5f, cy - 4.0f);
        chevron.lineTo (cx + 2.0f, cy);
        chevron.lineTo (cx - 1.5f, cy + 4.0f);
        g.strokePath (chevron, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void MscMainView::resized()
{
    presetBar.setBounds (600, 17, 460, 28);

    auto area = getLocalBounds().withTrimmedTop (headerHeight + margin).reduced (margin, 0).withTrimmedBottom (margin);
    juce::Component* modules[] { &inputModule, &dynPanModule, &haasModule, &chorusModule, &imageModule };

    for (size_t i = 0; i < std::size (modules); ++i)
    {
        modules[i]->setBounds (area.removeFromLeft (moduleWidths[i]));
        area.removeFromLeft (gap);
    }
}

//==============================================================================
MscEditor::MscEditor (MscProcessor& p)
    : AudioProcessorEditor (&p), processor (p), view (p, lookAndFeel)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (view);

    constexpr int w = MscMainView::baseWidth, h = MscMainView::baseHeight;
    const int savedWidth = juce::jlimit (w * 7 / 10, w * 2, (int) p.apvts.state.getProperty (uiWidthId, w));
    setSize (savedWidth, juce::roundToInt (savedWidth * (double) h / (double) w));

    setResizable (true, true);
    getConstrainer()->setFixedAspectRatio ((double) w / (double) h);
    setResizeLimits (w * 7 / 10, h * 7 / 10, w * 2, h * 2);
}

MscEditor::~MscEditor()
{
    setLookAndFeel (nullptr);
}

void MscEditor::resized()
{
    const float scale = (float) getWidth() / (float) MscMainView::baseWidth;
    view.setTransform (juce::AffineTransform::scale (scale));
    view.setBounds (0, 0, MscMainView::baseWidth, MscMainView::baseHeight);

    processor.apvts.state.setProperty (uiWidthId, getWidth(), nullptr);
}
