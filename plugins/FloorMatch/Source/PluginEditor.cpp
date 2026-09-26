#include "PluginEditor.h"

namespace floormatch::ui
{
namespace
{
    constexpr float minFreq = 20.0f, maxFreq = 20000.0f;

    juce::RangedAudioParameter& getParam (APVTS& state, const juce::String& id)
    {
        auto* param = state.getParameter (id);
        jassert (param != nullptr);
        return *param;
    }

    void drawCaption (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area,
                      juce::Justification justification = juce::Justification::centred)
    {
        g.setColour (palette().textDim);
        g.setFont (klaud::font (10.5f, true));
        g.drawText (text, area, justification, false);
    }

    void drawPanel (juce::Graphics& g, juce::Rectangle<int> area)
    {
        const auto r = area.toFloat();
        g.setColour (palette().panel);
        g.fillRoundedRectangle (r, 7.0f);
        g.setColour (palette().panelOutline);
        g.drawRoundedRectangle (r.reduced (0.5f), 7.0f, 1.0f);
    }

    juce::String dbText (float db) { return db <= -149.0f ? juce::String ("--") : juce::String (db, 1); }
}

const klaud::Palette& palette()
{
    static const klaud::Palette p = []
    {
        klaud::Palette pal;
        pal.background   = juce::Colour (0xff0f1413);
        pal.panel        = juce::Colour (0xff151c1b);
        pal.panelOutline = juce::Colour (0xff24302e);
        pal.track        = juce::Colour (0xff253230);
        pal.knobTop      = juce::Colour (0xff394a47);
        pal.knobBottom   = juce::Colour (0xff171e1d);
        pal.text         = juce::Colour (0xffdbe6e2);
        pal.textDim      = juce::Colour (0xff7a8f8a);
        pal.accent       = colours::accent;
        return pal;
    }();

    return p;
}

//==============================================================================
Knob::Knob (APVTS& state, const juce::String& paramId, const juce::String& titleText, bool arcFromStart)
    : title (titleText)
{
    slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 18);
    slider.setMouseDragSensitivity (220);
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    if (arcFromStart)
        slider.getProperties().set (klaud::arcFromStartProperty, true);
    addAndMakeVisible (slider);

    auto& param = getParam (state, paramId);
    attachment = std::make_unique<APVTS::SliderAttachment> (state, paramId, slider);
    slider.setDoubleClickReturnValue (true, param.convertFrom0to1 (param.getDefaultValue()));
}

void Knob::paint (juce::Graphics& g)
{
    drawCaption (g, title, getLocalBounds().removeFromTop (16));
}

void Knob::resized()
{
    slider.setBounds (getLocalBounds().withTrimmedTop (16));
}

//==============================================================================
Choice::Choice (APVTS& state, const juce::String& paramId, const juce::String& titleText)
    : title (titleText)
{
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&getParam (state, paramId)))
        box.addItemList (choice->choices, 1);

    box.setScrollWheelEnabled (true);
    addAndMakeVisible (box);
    attachment = std::make_unique<APVTS::ComboBoxAttachment> (state, paramId, box);
}

void Choice::paint (juce::Graphics& g)
{
    drawCaption (g, title, getLocalBounds().removeFromTop (16), juce::Justification::centredLeft);
}

void Choice::resized()
{
    box.setBounds (getLocalBounds().withTrimmedTop (18).withHeight (26));
}

//==============================================================================
SpectrumView::SpectrumView (FloorMatchProcessor& p) : processor (p)
{
    setInterceptsMouseClicks (false, false);
}

void SpectrumView::update (const floormatch::dsp::Snapshot& snapshot, bool hasSnapshot)
{
    active = hasSnapshot;
    if (! hasSnapshot)
    {
        repaint();
        return;
    }

    // Ease the curves and the vertical range so the display doesn't flicker.
    auto ease = [] (auto& shownValues, const auto& newValues)
    {
        for (size_t i = 0; i < shownValues.size(); ++i)
            shownValues[i] = shownValues[i] < -140.0f ? newValues[i] : shownValues[i] + 0.35f * (newValues[i] - shownValues[i]);
    };

    if (! shown.valid)
        shown = snapshot;

    ease (shown.noise, snapshot.noise);
    ease (shown.target, snapshot.target);
    ease (shown.output, snapshot.output);
    shown.valid = true;

    float loudest = -150.0f;
    for (int i = 6; i < floormatch::dsp::numProfileBands - 3; ++i)
        loudest = std::max ({ loudest, shown.noise[(size_t) i], shown.target[(size_t) i] });

    const float desiredTop = juce::jlimit (-60.0f, 0.0f, std::ceil ((loudest + 8.0f) / 10.0f) * 10.0f);
    topDb += 0.1f * (desiredTop - topDb);
    repaint();
}

float SpectrumView::xFor (float hz) const
{
    return (float) getWidth() * std::log (hz / minFreq) / std::log (maxFreq / minFreq);
}

float SpectrumView::yFor (float db) const
{
    return juce::jmap (db, topDb, topDb - rangeDb, 0.0f, (float) getHeight());
}

juce::Path SpectrumView::curve (const std::array<float, floormatch::dsp::numProfileBands>& levels, bool closed) const
{
    juce::Path path;
    const float bottom = (float) getHeight();

    for (int i = 0; i < floormatch::dsp::numProfileBands; ++i)
    {
        const float x = xFor (floormatch::dsp::profileBandFrequency (i));
        const float y = juce::jlimit (-2.0f, bottom + 2.0f, yFor (levels[(size_t) i]));

        if (i == 0)
        {
            if (closed)
            {
                path.startNewSubPath (x, bottom);
                path.lineTo (x, y);
            }
            else
            {
                path.startNewSubPath (x, y);
            }
        }
        else
        {
            path.lineTo (x, y);
        }
    }

    if (closed)
    {
        path.lineTo (xFor (floormatch::dsp::profileBandFrequency (floormatch::dsp::numProfileBands - 1)), bottom);
        path.closeSubPath();
    }

    return path;
}

void SpectrumView::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    const auto bounds = getLocalBounds().toFloat();

    // Grid.
    g.setFont (klaud::font (10.0f));
    for (float hz : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = xFor (hz);
        g.setColour (juce::Colours::white.withAlpha (0.045f));
        g.drawVerticalLine (juce::roundToInt (x), 0.0f, bounds.getBottom());
        g.setColour (pal.textDim.withAlpha (0.7f));
        g.drawText (hz >= 1000.0f ? juce::String (hz / 1000.0f, 0) + "k" : juce::String (hz, 0),
                    juce::Rectangle<float> (x + 3.0f, bounds.getBottom() - 15.0f, 40.0f, 13.0f), juce::Justification::centredLeft, false);
    }

    for (float db = std::floor (topDb / 10.0f) * 10.0f; db > topDb - rangeDb; db -= 10.0f)
    {
        const float y = yFor (db);
        g.setColour (juce::Colours::white.withAlpha (0.045f));
        g.drawHorizontalLine (juce::roundToInt (y), 0.0f, bounds.getRight());
        g.setColour (pal.textDim.withAlpha (0.7f));
        g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (4.0f, y + 1.0f, 36.0f, 12.0f),
                    juce::Justification::centredLeft, false);
    }

    // Curves.
    if (active && shown.valid)
    {
        g.setColour (colours::input.withAlpha (0.16f));
        g.fillPath (curve (shown.noise, true));
        g.setColour (colours::input.withAlpha (0.85f));
        g.strokePath (curve (shown.noise, false), juce::PathStrokeType (1.4f, juce::PathStrokeType::curved));

        g.setColour (colours::accent.withAlpha (0.14f));
        g.fillPath (curve (shown.output, true));
        g.setColour (colours::accent.withAlpha (0.9f));
        g.strokePath (curve (shown.output, false), juce::PathStrokeType (1.8f, juce::PathStrokeType::curved));

        juce::Path dashed;
        const float dashes[] { 5.0f, 4.0f };
        juce::PathStrokeType (1.2f).createDashedStroke (dashed, curve (shown.target, false), dashes, 2);
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        g.fillPath (dashed);
    }
    else
    {
        g.setColour (pal.textDim);
        g.setFont (klaud::font (13.0f));
        g.drawText ("Play audio to see the noise floor", bounds, juce::Justification::centred, false);
    }

    // Legend.
    auto legend = juce::Rectangle<float> (44.0f, 8.0f, 400.0f, 14.0f);
    auto item = [&] (juce::Colour colour, const juce::String& text, bool dashedLine)
    {
        g.setColour (colour);
        if (dashedLine)
        {
            for (float x = 0.0f; x < 14.0f; x += 5.0f)
                g.fillRect (legend.getX() + x, legend.getCentreY() - 0.5f, 3.0f, 1.2f);
        }
        else
        {
            g.fillRect (legend.getX(), legend.getCentreY() - 1.0f, 14.0f, 2.0f);
        }
        g.setColour (pal.textDim);
        g.setFont (klaud::font (11.0f));
        const float width = juce::GlyphArrangement::getStringWidth (klaud::font (11.0f), text) + 8.0f;
        g.drawText (text, legend.withX (legend.getX() + 19.0f).withWidth (width), juce::Justification::centredLeft, false);
        legend.setX (legend.getX() + 19.0f + width + 12.0f);
    };

    item (colours::input, "noise in", false);
    item (juce::Colours::white.withAlpha (0.75f), "target", true);
    item (colours::accent, "noise out", false);

    // Profile status.
    g.setFont (klaud::font (11.0f));
    g.setColour (processor.hasProfile() ? colours::accent.withAlpha (0.85f) : pal.textDim);
    g.drawText (processor.hasProfile() ? "colour: learned profile" : "colour: each take's own (no profile)",
                juce::Rectangle<float> (bounds.getRight() - 290.0f, 8.0f, 282.0f, 14.0f), juce::Justification::centredRight, false);
}

//==============================================================================
FloorMeter::FloorMeter (APVTS& state) : target (state.getRawParameterValue (ids::target))
{
    setInterceptsMouseClicks (false, false);
}

void FloorMeter::update (const floormatch::dsp::Snapshot& snapshot, bool hasSnapshot)
{
    active = hasSnapshot;
    if (hasSnapshot)
    {
        auto ease = [] (float& v, float to) { v = v < -140.0f ? to : v + 0.3f * (to - v); };
        ease (inDb, snapshot.noiseDb);
        ease (outDb, snapshot.outputDb);
    }
    repaint();
}

float FloorMeter::yFor (float db, juce::Rectangle<float> area) const
{
    return juce::jmap (juce::jlimit (bottomDb, topDb, db), topDb, bottomDb, area.getY(), area.getBottom());
}

void FloorMeter::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    auto area = getLocalBounds().toFloat();
    auto numbers = area.removeFromBottom (40.0f);
    auto scale = area.removeFromLeft (30.0f);
    area.removeFromTop (18.0f);
    scale.removeFromTop (18.0f);

    const float barWidth = 30.0f;
    const auto inBar = juce::Rectangle<float> (area.getX() + 14.0f, area.getY(), barWidth, area.getHeight());
    const auto outBar = inBar.translated (barWidth + 26.0f, 0.0f);

    // Scale.
    g.setFont (klaud::font (10.0f));
    for (float db = topDb; db >= bottomDb; db -= 10.0f)
    {
        const float y = yFor (db, inBar);
        g.setColour (pal.textDim.withAlpha (0.7f));
        g.drawText (juce::String (juce::roundToInt (db)), juce::Rectangle<float> (scale.getX(), y - 6.0f, 28.0f, 12.0f),
                    juce::Justification::centredRight, false);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawHorizontalLine (juce::roundToInt (y), inBar.getX(), outBar.getRight());
    }

    auto drawBar = [&] (juce::Rectangle<float> bar, float db, juce::Colour colour, const juce::String& label)
    {
        g.setColour (pal.background.withAlpha (0.8f));
        g.fillRoundedRectangle (bar, 3.0f);

        if (active && db > bottomDb)
        {
            auto fill = bar.withTop (yFor (db, bar));
            g.setColour (colour.withAlpha (0.75f));
            g.fillRoundedRectangle (fill, 3.0f);
        }

        drawCaption (g, label, bar.withY (bar.getY() - 17.0f).withHeight (14.0f).toNearestInt());
    };

    drawBar (inBar, inDb, colours::input, "IN");
    drawBar (outBar, outDb, colours::accent, "OUT");

    // Target line across both bars.
    const float ty = yFor (target->load(), inBar);
    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.fillRect (inBar.getX() - 4.0f, ty - 0.75f, outBar.getRight() - inBar.getX() + 8.0f, 1.5f);

    // Numbers.
    g.setFont (klaud::font (14.0f, true));
    g.setColour (colours::input);
    g.drawText (active ? dbText (inDb) : "--", juce::Rectangle<float> (inBar.getX() - 16.0f, numbers.getY() + 4.0f, barWidth + 32.0f, 18.0f),
                juce::Justification::centred, false);
    g.setColour (colours::accent);
    g.drawText (active ? dbText (outDb) : "--", juce::Rectangle<float> (outBar.getX() - 16.0f, numbers.getY() + 4.0f, barWidth + 32.0f, 18.0f),
                juce::Justification::centred, false);
    drawCaption (g, "dB(A)", juce::Rectangle<float> (inBar.getX(), numbers.getY() + 22.0f, outBar.getRight() - inBar.getX(), 14.0f).toNearestInt());
}
}

//==============================================================================
using namespace floormatch;

namespace
{
    constexpr int headerHeight = 56;
    const juce::Identifier uiWidthId { "uiWidth" };

    const juce::Rectangle<int> spectrumPanel { 16, 68, 620, 290 };
    const juce::Rectangle<int> meterPanel    { 648, 68, 196, 290 };
    const juce::Rectangle<int> controlPanel  { 16, 370, 828, 116 };
}

FloorMatchMainView::FloorMatchMainView (FloorMatchProcessor& p, klaud::LookAndFeel& lookAndFeel)
    : processor (p),
      lookAndFeelSetter (*this, lookAndFeel),
      spectrum (p),
      meter (p.apvts),
      target (p.apvts, ids::target, "TARGET", true),
      maxReduction (p.apvts, ids::maxReduction, "MAX REDUCTION", true),
      match (p.apvts, ids::match, "COLOUR MATCH", true),
      lookahead (p.apvts, ids::lookahead, "LOOKAHEAD"),
      listen (p.apvts, ids::listen, "LISTEN")
{
    for (auto* c : std::initializer_list<juce::Component*> { &spectrum, &meter, &target, &maxReduction, &match, &lookahead, &listen,
                                                             &learnButton, &quietestButton, &resetQuietestButton, &fillButton,
                                                             &clearProfileButton })
        addAndMakeVisible (c);

    target.slider.setTooltip ("Noise floor every take is brought to, A-weighted. Learn or Use quietest set it for you.");
    maxReduction.slider.setTooltip ("The most a take's noise is turned down. Keeps very noisy takes from sounding processed.");
    match.slider.setTooltip ("0 %: only the level is matched and each take keeps its own noise colour. "
                             "100 %: the noise is also shaped like the learned profile.");
    lookahead.box.setTooltip ("How far ahead the plugin looks. 1 s follows cuts between takes exactly, at 1 s of latency "
                              "(compensated by the DAW). Off: low latency, but a cut to louder noise is followed 0.4 s late.");
    listen.box.setTooltip ("Removed: hear only what is taken out. It should be noise, never words.");

    learnButton.setTooltip ("Play the take whose background you want (usually the quietest), click Learn, and click again "
                            "to store it. Sets the target level and the noise colour.");
    learnButton.onClick = [this]
    {
        processor.setLearning (! processor.isLearning());
        updateButtons();
    };

    quietestButton.setTooltip ("Play the whole track once; the plugin remembers the quietest steady background. "
                               "Click to use it as the target level and colour.");
    quietestButton.onClick = [this]
    {
        processor.useQuietest();
        updateButtons();
    };

    resetQuietestButton.setTooltip ("Forget the quietest background heard so far.");
    resetQuietestButton.onClick = [this] { processor.resetQuietest(); };

    clearProfileButton.setTooltip ("Forget the learned noise colour: only levels are matched.");
    clearProfileButton.onClick = [this] { processor.clearProfile(); };

    fillButton.setClickingTogglesState (true);
    fillButton.setTooltip ("Takes quieter than the target get room tone shaped like the target added, "
                           "so every take reaches the same background.");
    fillAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (p.apvts, ids::fill, fillButton);

    setSize (baseWidth, baseHeight);
    updateButtons();
    startTimerHz (30);
}

FloorMatchMainView::~FloorMatchMainView()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void FloorMatchMainView::timerCallback()
{
    refresh();
}

void FloorMatchMainView::refresh()
{
    floormatch::dsp::Snapshot snapshot;
    const bool hasSnapshot = processor.getSnapshot (snapshot);
    spectrum.update (snapshot, hasSnapshot);
    meter.update (snapshot, hasSnapshot);

    if (++blink % 6 == 0)
        updateButtons();
}

void FloorMatchMainView::updateButtons()
{
    const bool learning = processor.isLearning();
    learnButton.setButtonText (learning ? "Learning... click to store" : "Learn");
    learnButton.setToggleState (learning && (blink / 12) % 2 == 0, juce::dontSendNotification);
    learnButton.setColour (juce::TextButton::buttonOnColourId, ui::colours::learn);

    float quietest = 0.0f;
    const bool hasQuietest = processor.getQuietest (quietest);
    quietestButton.setEnabled (hasQuietest);
    quietestButton.setButtonText (hasQuietest ? "Use quietest  " + juce::String (quietest, 1) + " dB" : "Quietest: listening...");
    clearProfileButton.setEnabled (processor.hasProfile());
}

void FloorMatchMainView::paint (juce::Graphics& g)
{
    const auto& pal = ui::palette();

    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff1a2322), 0.0f, 0.0f, pal.background, 0.0f, (float) getHeight(), false));
    g.fillAll();

    // Header.
    g.setColour (pal.text);
    g.setFont (klaud::font (28.0f, true));
    g.drawText ("FloorMatch", 22, 11, 170, 34, juce::Justification::centredLeft, false);

    g.setColour (ui::colours::accent.withAlpha (0.85f));
    g.setFont (klaud::font (12.5f));
    g.drawText ("dialogue noise floor matcher", 190, 23, 240, 18, juce::Justification::centredLeft, false);

    g.setColour (pal.textDim);
    g.setFont (klaud::font (11.0f));
    g.drawText (JucePlugin_Manufacturer, getWidth() - 160, 22, 138, 18, juce::Justification::centredRight, false);

    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.fillRect (0, headerHeight - 1, getWidth(), 1);

    for (auto panel : { spectrumPanel, meterPanel, controlPanel })
        ui::drawPanel (g, panel);

    ui::drawCaption (g, "NOISE FLOOR", meterPanel.withHeight (26).withTrimmedTop (8));
}

void FloorMatchMainView::resized()
{
    spectrum.setBounds (spectrumPanel.reduced (6));
    meter.setBounds (meterPanel.withTrimmedTop (30).reduced (14, 8));

    auto area = controlPanel.reduced (14, 8);
    target.setBounds (area.removeFromLeft (96));
    area.removeFromLeft (8);

    auto learnColumn = area.removeFromLeft (190).withTrimmedTop (12);
    learnButton.setBounds (learnColumn.removeFromTop (28));
    learnColumn.removeFromTop (8);
    auto quietestRow = learnColumn.removeFromTop (28);
    resetQuietestButton.setBounds (quietestRow.removeFromRight (52));
    quietestRow.removeFromRight (6);
    quietestButton.setBounds (quietestRow);
    learnColumn.removeFromTop (8);
    clearProfileButton.setBounds (learnColumn.removeFromTop (22).removeFromLeft (120));
    clearProfileButton.setButtonText ("Clear colour");

    area.removeFromLeft (18);
    maxReduction.setBounds (area.removeFromLeft (96));
    area.removeFromLeft (6);
    match.setBounds (area.removeFromLeft (96));
    area.removeFromLeft (18);

    auto right = area;
    auto fillColumn = right.removeFromLeft (130).withTrimmedTop (30);
    fillButton.setBounds (fillColumn.removeFromTop (30));
    right.removeFromLeft (14);
    auto choices = right;
    lookahead.setBounds (choices.removeFromTop (48));
    choices.removeFromTop (4);
    listen.setBounds (choices.removeFromTop (48));
}

//==============================================================================
FloorMatchEditor::FloorMatchEditor (FloorMatchProcessor& p)
    : AudioProcessorEditor (&p), processor (p), view (p, lookAndFeel)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (view);

    constexpr int w = FloorMatchMainView::baseWidth, h = FloorMatchMainView::baseHeight;
    const int savedWidth = juce::jlimit (w * 7 / 10, w * 2, (int) p.apvts.state.getProperty (uiWidthId, w));
    setSize (savedWidth, juce::roundToInt (savedWidth * (double) h / (double) w));

    setResizable (true, true);
    getConstrainer()->setFixedAspectRatio ((double) w / (double) h);
    setResizeLimits (w * 7 / 10, h * 7 / 10, w * 2, h * 2);
}

FloorMatchEditor::~FloorMatchEditor()
{
    setLookAndFeel (nullptr);
}

void FloorMatchEditor::refresh()
{
    view.refresh();
}

void FloorMatchEditor::resized()
{
    const float scale = (float) getWidth() / (float) FloorMatchMainView::baseWidth;
    view.setTransform (juce::AffineTransform::scale (scale));
    view.setBounds (0, 0, FloorMatchMainView::baseWidth, FloorMatchMainView::baseHeight);

    processor.apvts.state.setProperty (uiWidthId, getWidth(), nullptr);
}
