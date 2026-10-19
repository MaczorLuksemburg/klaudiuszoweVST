#include "PluginEditor.h"

namespace
{
    constexpr int headerHeight = 56;
    constexpr int margin = 14;
    constexpr int gap = 6;

    const juce::Identifier uiWidthId { "uiWidth" };

    void setParam (juce::AudioProcessorValueTreeState& apvts, const char* id, float plainValue)
    {
        if (auto* p = apvts.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (plainValue));
            p->endChangeGesture();
        }
    }

    bool isOn (juce::AudioProcessorValueTreeState& apvts, const char* id)
    {
        return apvts.getRawParameterValue (id)->load() > 0.5f;
    }
}

//==============================================================================
WaveRefMainView::WaveRefMainView (WaveRefProcessor& p, klaud::LookAndFeel& lookAndFeel)
    : processor (p),
      lookAndFeelSetter (*this, lookAndFeel),
      settings (p.getUiState()),
      spectrum (p, settings),
      stereoPanel (p, settings),
      analyserBar (settings),
      waveform (p, settings),
      slotBar (p),
      controls (p, settings),
      presetBar (settings, [this] { analyserBar.refresh(); repaint(); })
{
    for (auto* c : std::initializer_list<juce::Component*> { &spectrum, &stereoPanel, &analyserBar, &waveform, &slotBar, &controls, &presetBar })
        addAndMakeVisible (c);

    setWantsKeyboardFocus (true);
    setSize (baseWidth, baseHeight);
}

WaveRefMainView::~WaveRefMainView()
{
    setLookAndFeel (nullptr);
}

void WaveRefMainView::paint (juce::Graphics& g)
{
    const auto& pal = waveref::ui::palette();

    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1a1d28), 0.0f, 0.0f,
                                             pal.background, 0.0f, (float) getHeight(), false));
    g.fillAll();

    g.setColour (pal.text);
    g.setFont (klaud::font (30.0f, true));
    g.drawText ("WaveRef", 22, 10, 150, 36, juce::Justification::centredLeft, false);

    g.setColour (pal.accent.withAlpha (0.85f));
    g.setFont (klaud::font (12.5f));
    g.drawText ("reference A/B and spectrum", 158, 23, 260, 18, juce::Justification::centredLeft, false);

    g.setColour (pal.textDim);
    g.setFont (klaud::font (11.0f));
    g.drawText (juce::String (JucePlugin_Manufacturer), getWidth() - 140, 20, 118, 18, juce::Justification::centredRight, false);

    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.fillRect (0, headerHeight - 1, getWidth(), 1);
}

void WaveRefMainView::resized()
{
    auto area = getLocalBounds().withTrimmedTop (headerHeight + 8).reduced (margin, 0).withTrimmedBottom (margin);
    presetBar.setBounds (440, 14, 470, 28);

    controls.setBounds (area.removeFromBottom (100));
    area.removeFromBottom (gap);
    slotBar.setBounds (area.removeFromBottom (30));
    area.removeFromBottom (gap);
    waveform.setBounds (area.removeFromBottom (176));
    area.removeFromBottom (gap);
    analyserBar.setBounds (area.removeFromBottom (32));
    area.removeFromBottom (2);

    stereoPanel.setBounds (area.removeFromRight (246));
    area.removeFromRight (gap);
    spectrum.setBounds (area);
}

void WaveRefMainView::refresh()
{
    spectrum.refresh();
    stereoPanel.refresh();
    waveform.refresh();
    controls.refresh();
    analyserBar.refresh();
    repaint();
}

bool WaveRefMainView::keyPressed (const juce::KeyPress& key)
{
    auto& apvts = processor.apvts;
    const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());

    if (c == 'a' || c == 'x')      { setParam (apvts, waveref::ids::ab, 0.0f); return true; }
    if (c == 'b' || c == 'y')      { setParam (apvts, waveref::ids::ab, 1.0f); return true; }
    if (c == 'm')                  { setParam (apvts, waveref::ids::mono, isOn (apvts, waveref::ids::mono) ? 0.0f : 1.0f); return true; }

    if (key == juce::KeyPress::spaceKey)   { waveform.togglePlay(); return true; }

    // Q W E R: jump to cue 1-4 (or set it when empty); with Shift, set it at the playhead.
    for (int i = 0; i < 4; ++i)
    {
        if (c == (juce::juce_wchar) "qwer"[i])
        {
            if (key.getModifiers().isShiftDown()) waveform.setCueHere (i);
            else                                  waveform.cueClicked (i, false);
            return true;
        }
    }

    if (key == juce::KeyPress::tabKey)
    {
        setParam (apvts, waveref::ids::ab, isOn (apvts, waveref::ids::ab) ? 0.0f : 1.0f);
        return true;
    }

    if (c >= '1' && c <= '8')
    {
        setParam (apvts, waveref::ids::slot, (float) (c - '1'));
        return true;
    }

    return false;
}

//==============================================================================
WaveRefEditor::WaveRefEditor (WaveRefProcessor& p)
    : AudioProcessorEditor (&p), processor (p), view (p, lookAndFeel)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (view);

    constexpr int w = WaveRefMainView::baseWidth, h = WaveRefMainView::baseHeight;
    const int savedWidth = juce::jlimit (w * 7 / 10, w * 2, (int) p.apvts.state.getProperty (uiWidthId, w));
    setSize (savedWidth, juce::roundToInt (savedWidth * (double) h / (double) w));

    setResizable (true, true);
    getConstrainer()->setFixedAspectRatio ((double) w / (double) h);
    setResizeLimits (w * 7 / 10, h * 7 / 10, w * 2, h * 2);
}

WaveRefEditor::~WaveRefEditor()
{
    setLookAndFeel (nullptr);
}

void WaveRefEditor::resized()
{
    const float scale = (float) getWidth() / (float) WaveRefMainView::baseWidth;
    view.setTransform (juce::AffineTransform::scale (scale));
    view.setBounds (0, 0, WaveRefMainView::baseWidth, WaveRefMainView::baseHeight);

    processor.apvts.state.setProperty (uiWidthId, getWidth(), nullptr);
}
