#include "Components.h"

namespace dynmap::ui
{
namespace
{
    juce::RangedAudioParameter& getParam (APVTS& state, const juce::String& id)
    {
        auto* param = state.getParameter (id);
        jassert (param != nullptr);
        return *param;
    }

    float defaultValueOf (juce::RangedAudioParameter& param)
    {
        return param.convertFrom0to1 (param.getDefaultValue());
    }

    // Low to high: warm to cool, so the band display reads like a spectrum.
    const juce::Colour bandGradient[] { juce::Colour (0xffff6b6b), juce::Colour (0xffff9f43), juce::Colour (0xfffeca57),
                                        juce::Colour (0xffa3e048), juce::Colour (0xff2ed573), juce::Colour (0xff1dd1a1),
                                        juce::Colour (0xff48dbfb), juce::Colour (0xff54a0ff), juce::Colour (0xff7d7bff),
                                        juce::Colour (0xffb77bff), juce::Colour (0xffe77bff), juce::Colour (0xffff7aa2) };
}

const klaud::Palette& palette()
{
    static const klaud::Palette p = []
    {
        klaud::Palette pal;
        pal.background   = juce::Colour (0xff0d1015);
        pal.panel        = juce::Colour (0xff141920);
        pal.panelOutline = juce::Colour (0xff222933);
        pal.track        = juce::Colour (0xff242b35);
        pal.knobTop      = juce::Colour (0xff3a4350);
        pal.knobBottom   = juce::Colour (0xff171b22);
        pal.text         = juce::Colour (0xffdde3ea);
        pal.textDim      = juce::Colour (0xff7b8694);
        pal.accent       = juce::Colour (0xff3fd0b6);
        return pal;
    }();

    return p;
}

juce::Colour stageColour (const DynMapProcessor& p, int stage)
{
    if (stage == inputStage)  return colours::input;
    if (stage == masterStage) return colours::master;

    const auto layout = p.engine.getLayout();
    const int position = juce::jmax (0, layout.positionOf (stage - 1));

    if (layout.numBands <= 1)
        return bandGradient[5];

    // Spread the used bands over the gradient (up to violet, so the two ends don't look alike).
    const float t = (float) position / (float) (layout.numBands - 1) * juce::jmin (9.0f, (float) layout.numBands + 3.0f);
    const int i = juce::jlimit (0, (int) std::size (bandGradient) - 2, (int) t);
    return bandGradient[i].interpolatedWith (bandGradient[i + 1], t - (float) i);
}

juce::String stageLabel (const DynMapProcessor& p, int stage)
{
    if (stage == inputStage)  return "Input";
    if (stage == masterStage) return "Master";

    const auto layout = p.engine.getLayout();
    return "Band " + juce::String (juce::jmax (0, layout.positionOf (stage - 1)) + 1);
}

void drawCaption (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area, juce::Justification justification)
{
    g.setColour (palette().textDim);
    g.setFont (klaud::font (10.5f, true));
    g.drawText (text, area, justification, false);
}

void drawPanel (juce::Graphics& g, juce::Rectangle<int> area, const juce::String& title, juce::Colour titleColour)
{
    const auto& pal = palette();
    const auto r = area.toFloat();

    g.setColour (pal.panel.withAlpha (0.92f));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (pal.panelOutline);
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);

    if (title.isNotEmpty())
    {
        g.setColour (titleColour.isTransparent() ? pal.textDim : titleColour);
        g.setFont (klaud::font (11.0f, true));
        g.drawText (title.toUpperCase(), area.getX() + 12, area.getY() + 7, area.getWidth() - 24, 16,
                    juce::Justification::centredLeft, false);
    }
}

void setParameter (APVTS& state, const juce::String& id, float value)
{
    if (auto* param = state.getParameter (id))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (value));
        param->endChangeGesture();
    }
}

//==============================================================================
Knob::Knob (const juce::String& titleText, bool arcFromStart) : title (titleText)
{
    slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 16);
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    slider.setMouseDragSensitivity (220);
    slider.setScrollWheelEnabled (true);

    if (arcFromStart)
        slider.getProperties().set (klaud::arcFromStartProperty, true);

    setAccent (palette().accent);
    addAndMakeVisible (slider);
}

void Knob::attach (APVTS& state, const juce::String& paramId)
{
    attachment.reset();
    attachment = std::make_unique<APVTS::SliderAttachment> (state, paramId, slider);
    slider.setDoubleClickReturnValue (true, defaultValueOf (getParam (state, paramId)));
    repaint();
}

void Knob::setAccent (juce::Colour accent)
{
    slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
    slider.setColour (juce::Slider::textBoxHighlightColourId, accent.withAlpha (0.4f));
    slider.repaint();
}

void Knob::paint (juce::Graphics& g)
{
    if (title.isNotEmpty())
        drawCaption (g, title, getLocalBounds().removeFromTop (titleHeight));
}

void Knob::resized()
{
    slider.setBounds (getLocalBounds().withTrimmedTop (title.isNotEmpty() ? titleHeight : 0));
}

//==============================================================================
Choice::Choice (const juce::String& titleText) : title (titleText)
{
    box.setScrollWheelEnabled (true);
    addAndMakeVisible (box);
}

void Choice::attach (APVTS& state, const juce::String& paramId)
{
    attachment.reset();

    if (box.getNumItems() == 0)
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&getParam (state, paramId)))
            box.addItemList (choice->choices, 1);

    attachment = std::make_unique<APVTS::ComboBoxAttachment> (state, paramId, box);
}

void Choice::paint (juce::Graphics& g)
{
    if (title.isNotEmpty())
        drawCaption (g, title, getLocalBounds().removeFromTop (15), juce::Justification::centredLeft);
}

void Choice::resized()
{
    auto area = getLocalBounds();
    if (title.isNotEmpty())
        area.removeFromTop (16);
    box.setBounds (area.removeFromTop (24));
}

//==============================================================================
Toggle::Toggle (const juce::String& text) : juce::TextButton (text)
{
    setClickingTogglesState (true);
    setAccent (palette().accent);
}

void Toggle::attach (APVTS& state, const juce::String& paramId)
{
    attachment.reset();
    attachment = std::make_unique<APVTS::ButtonAttachment> (state, paramId, *this);
}

void Toggle::setAccent (juce::Colour accent)
{
    setColour (juce::TextButton::buttonOnColourId, accent);
    repaint();
}

//==============================================================================
PresetBar::PresetBar (PresetManager& manager) : presets (manager)
{
    previous.setTooltip ("Previous preset");
    next.setTooltip ("Next preset");
    save.setTooltip ("Save the current settings (including curves) as a user preset");

    previous.onClick = [this] { step (-1); };
    next.onClick     = [this] { step (1); };
    save.onClick     = [this] { showSaveDialog(); };

    list.setTextWhenNothingSelected ("Init");
    list.onChange = [this]
    {
        const int index = list.getSelectedId() - 1;
        if (index >= 0 && index != presets.getCurrentPresetIndex())
            presets.loadPreset (index);
        shownName = presets.getCurrentPresetName();
    };

    for (auto* c : std::initializer_list<juce::Component*> { &previous, &list, &next, &save })
        addAndMakeVisible (c);

    refresh();
    startTimerHz (5);
}

void PresetBar::resized()
{
    auto area = getLocalBounds();
    save.setBounds (area.removeFromRight (56));
    area.removeFromRight (8);
    previous.setBounds (area.removeFromLeft (26));
    area.removeFromLeft (4);
    next.setBounds (area.removeFromRight (26));
    area.removeFromRight (4);
    list.setBounds (area);
}

void PresetBar::refresh()
{
    presets.rescanUserPresets();
    list.clear (juce::dontSendNotification);

    const auto names = presets.getPresetNames();
    const int numFactory = presets.getNumFactoryPresets();

    list.addSectionHeading ("Factory");
    for (int i = 0; i < numFactory; ++i)
        list.addItem (names[i], i + 1);

    if (names.size() > numFactory)
    {
        list.addSeparator();
        list.addSectionHeading ("User");
        for (int i = numFactory; i < names.size(); ++i)
            list.addItem (names[i], i + 1);
    }

    shownName = presets.getCurrentPresetName();
    const int index = presets.getCurrentPresetIndex();

    if (index >= 0)
        list.setSelectedId (index + 1, juce::dontSendNotification);
    else
        list.setText (shownName, juce::dontSendNotification);
}

void PresetBar::timerCallback()
{
    if (presets.getCurrentPresetName() != shownName)
        refresh();
}

void PresetBar::step (int delta)
{
    const int count = presets.getPresetNames().size();
    if (count == 0)
        return;

    const int current = juce::jmax (0, presets.getCurrentPresetIndex());
    presets.loadPreset ((current + delta + count) % count);
    refresh();
}

void PresetBar::showSaveDialog()
{
    saveDialog = std::make_unique<juce::AlertWindow> ("Save preset", "Name your preset:", juce::MessageBoxIconType::NoIcon, this);
    saveDialog->setLookAndFeel (&getLookAndFeel());

    const auto current = presets.getCurrentPresetName();
    saveDialog->addTextEditor ("name", current == "Init" ? juce::String() : current);
    saveDialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    saveDialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<PresetBar> safeThis (this);

    saveDialog->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis] (int result)
    {
        if (safeThis == nullptr || safeThis->saveDialog == nullptr)
            return;

        const auto name = safeThis->saveDialog->getTextEditorContents ("name");
        safeThis->saveDialog->setVisible (false);

        if (result == 1 && safeThis->presets.saveUserPreset (name))
            safeThis->refresh();
    }), false);
}

//==============================================================================
const std::vector<DetectorStyle>& detectorStyles()
{
    // att, hold, release, shape, rms, link, lookahead index, smooth, transient time, release mode, attack mode, release 2
    static const std::vector<DetectorStyle> styles {
        { "Clean",     "Even and general purpose: follows the music without drawing attention.",
          5.0f, 0.0f, 120.0f, 0.0f, 0.0f, 100.0f, 0, 0.5f, 40.0f },
        { "Punchy",    "Slower attack lets each hit through before the curve acts.",
          25.0f, 0.0f, 100.0f, 30.0f, 0.0f, 100.0f, 0, 0.5f, 30.0f },
        { "Glue",      "Bus glue: 10 ms attack, quick release that settles slowly (REL 2), a little averaging, 1 ms lookahead.",
          10.0f, 0.0f, 120.0f, 40.0f, 5.0f, 100.0f, 2, 1.0f, 50.0f, ids::relLawClassic, 0, 600.0f },
        { "Smooth",    "Gentle levelling for pads and long notes: eased attack, slow second release, 2 ms lookahead.",
          20.0f, 10.0f, 250.0f, 60.0f, 20.0f, 100.0f, 3, 1.5f, 60.0f, ids::relLawClassic, 2, 1000.0f },
        { "Fast",      "Quick attack and release with 0.5 ms lookahead: grabs every peak, can add grit.",
          0.5f, 0.0f, 40.0f, 0.0f, 0.0f, 100.0f, 1, 0.3f, 20.0f },
        { "Brickwall", "Lookahead and instant attack: nothing slips past the curve (2 ms latency); REL 2 calms long notes.",
          0.05f, 5.0f, 80.0f, 0.0f, 0.0f, 100.0f, 3, 0.2f, 30.0f, ids::relLawClassic, 0, 250.0f },
        { "Pump",      "Breathing release for sidechain ducking and EDM pumping; 1 ms lookahead so the duck lands with the kick.",
          2.0f, 0.0f, 220.0f, 70.0f, 0.0f, 100.0f, 2, 0.5f, 40.0f },
        { "Waveform",  "Follows the waveform itself, so the curve becomes distortion (the Maximus trick).",
          0.01f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0, 0.0f, 5.0f },
        // Maximus's default master band, measured on the VST: ATT 2 ms is a 2 ms lookahead, REL 85.53 ms with
        // release curve 3 is our Accel 3 at the same time, and its 10 ms sustain is our built-in peak window.
        { "Maximus",   "Maximus's default timing: 2 ms lookahead, peak detection, REL 85.53 ms with its slow-start release curve.",
          0.2f, 0.0f, 85.53f, 0.0f, 0.0f, 100.0f, 3, 0.5f, 40.0f, ids::accelRelease (3) },
        { "Auto",      "Program-dependent: short peaks recover fast, long loud passages slowly, so it rarely pumps. A safe all-rounder.",
          3.0f, 0.0f, 100.0f, 0.0f, 0.0f, 100.0f, 2, 0.5f, 40.0f, ids::relLawAuto },
        { "Vocal",     "Vocal levelling: soft eased attack, short RMS, quick release that settles slowly, so words stay even.",
          15.0f, 0.0f, 150.0f, 40.0f, 10.0f, 100.0f, 0, 1.0f, 50.0f, ids::relLawClassic, 2, 700.0f },
        { "Master",    "Loud, calm masters: eased attack behind 2 ms lookahead, Maximus-style release with a slow second release.",
          1.0f, 0.0f, 120.0f, 0.0f, 0.0f, 100.0f, 3, 0.5f, 40.0f, ids::accelRelease (3), 2, 400.0f },
    };

    return styles;
}

DetectorStylePicker::DetectorStylePicker (DynMapProcessor& p) : processor (p)
{
    const auto& styles = detectorStyles();

    for (int i = 0; i < (int) styles.size(); ++i)
    {
        auto* button = buttons.add (new juce::TextButton (styles[(size_t) i].name));
        button->setTooltip (styles[(size_t) i].description);
        button->onClick = [this, i] { apply (i); };
        addAndMakeVisible (button);
    }

    startTimerHz (8);
}

void DetectorStylePicker::setStage (int newStage, juce::Colour accent)
{
    stage = newStage;

    for (auto* b : buttons)
        b->setColour (juce::TextButton::buttonOnColourId, accent);

    shown = -2;
    timerCallback();
}

int DetectorStylePicker::matchingStyle() const
{
    auto value = [this] (const char* name) { return processor.apvts.getRawParameterValue (stageParamId (stage, name))->load(); };
    auto near = [] (float a, float b) { return std::abs (a - b) <= 0.02f * juce::jmax (std::abs (a), std::abs (b)) + 1.0e-3f; };
    const auto& styles = detectorStyles();

    for (int i = 0; i < (int) styles.size(); ++i)
    {
        const auto& s = styles[(size_t) i];
        if (near (value (ids::attack), s.attack) && near (value (ids::hold), s.hold) && near (value (ids::release), s.release)
            && near (value (ids::relShape), s.relShape) && near (value (ids::rms), s.rms) && near (value (ids::link), s.link)
            && juce::roundToInt (value (ids::lookahead)) == s.lookahead && near (value (ids::smooth), s.smooth)
            && near (value (ids::trTime), s.trTime) && juce::roundToInt (value (ids::relLaw)) == s.relLaw
            && juce::roundToInt (value (ids::attLaw)) == s.attLaw && near (value (ids::release2), s.release2))
            return i;
    }

    return -1;
}

void DetectorStylePicker::apply (int index)
{
    const auto& s = detectorStyles()[(size_t) index];
    auto set = [this] (const char* name, float v) { setParameter (processor.apvts, stageParamId (stage, name), v); };

    set (ids::attack, s.attack);
    set (ids::hold, s.hold);
    set (ids::release, s.release);
    set (ids::relShape, s.relShape);
    set (ids::rms, s.rms);
    set (ids::link, s.link);
    set (ids::lookahead, (float) s.lookahead);
    set (ids::smooth, s.smooth);
    set (ids::trTime, s.trTime);
    set (ids::relLaw, (float) s.relLaw);
    set (ids::attLaw, (float) s.attLaw);
    set (ids::release2, s.release2);
    timerCallback();
}

void DetectorStylePicker::timerCallback()
{
    const int match = matchingStyle();
    repaint();   // the summary line follows the knobs

    if (match == shown)
        return;

    shown = match;
    for (int i = 0; i < buttons.size(); ++i)
        buttons[i]->setToggleState (i == match, juce::dontSendNotification);
    repaint();
}

void DetectorStylePicker::resized()
{
    auto area = getLocalBounds();
    const int columnWidth = (area.getWidth() - 2 * 8) / columns;

    for (int i = 0; i < buttons.size(); ++i)
        buttons[i]->setBounds (area.getX() + (i % columns) * (columnWidth + 8), area.getY() + (i / columns) * (buttonHeight + 10), columnWidth, buttonHeight);
}

void DetectorStylePicker::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    const int rows = (buttons.size() + columns - 1) / columns;
    auto area = getLocalBounds().withTrimmedTop (rows * buttonHeight + (rows - 1) * 10 + 16).reduced (4, 0);

    g.setFont (klaud::font (12.5f));
    g.setColour (shown >= 0 ? pal.text : pal.textDim);
    g.drawFittedText (shown >= 0 ? juce::String (detectorStyles()[(size_t) shown].description)
                                 : juce::String ("Custom settings. Open Advanced to see or change them."),
                      area.removeFromTop (36), juce::Justification::topLeft, 2);

    // What the detector is actually set to.
    auto value = [this] (const char* name) { return processor.apvts.getRawParameterValue (stageParamId (stage, name))->load(); };
    auto ms = [] (float v) { return v < 1.0f ? juce::String (v, 2) + " ms" : v < 10.0f ? juce::String (v, 1) + " ms" : juce::String (juce::roundToInt (v)) + " ms"; };

    const int attMode = juce::roundToInt (value (ids::attLaw)), relMode = juce::roundToInt (value (ids::relLaw));
    juce::String summary = "attack " + ms (value (ids::attack)) + (attMode > 0 ? " (ease " + juce::String (attMode) + ")" : juce::String())
                         + ",  release " + ms (value (ids::release));
    if (relMode == ids::relLawAuto)
        summary << " (auto)";
    else if (relMode >= ids::relLawFirstAccel)
        summary << " (accel " << (relMode - ids::relLawFirstAccel + 1) << ")";
    if (value (ids::release2) > 0.0f)
        summary << " + " << ms (value (ids::release2));
    summary << ",  " << (value (ids::rms) > 0.0f ? "RMS " + ms (value (ids::rms)) : juce::String ("peak"));
    const int la = juce::roundToInt (value (ids::lookahead));
    if (la > 0)
        summary << ",  lookahead " << juce::String (lookaheadMs[(size_t) juce::jlimit (0, numLookaheads - 1, la)], 1) << " ms";

    g.setFont (klaud::font (11.0f));
    g.setColour (pal.textDim);
    g.drawFittedText (summary, area.removeFromTop (30), juce::Justification::topLeft, 2);
}

//==============================================================================
OutputMeter::OutputMeter (DynMapProcessor& p) : processor (p)
{
    setTooltip ("Peak levels: input (left pair) and output (right pair)");
    startTimerHz (30);
}

void OutputMeter::timerCallback()
{
    auto& m = processor.engine.meters;

    auto fall = [] (float& shown, float peak)
    {
        const float db = juce::Decibels::gainToDecibels (peak, -100.0f);
        shown = db > shown ? db : juce::jmax (-100.0f, shown - 1.2f);
    };

    for (int ch = 0; ch < 2; ++ch)
    {
        fall (inDb[(size_t) ch], m.inPeak[(size_t) ch].exchange (0.0f));
        fall (outDb[(size_t) ch], m.outPeak[(size_t) ch].exchange (0.0f));
    }

    repaint();
}

void OutputMeter::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    auto bars = getLocalBounds().withTrimmedBottom (14).withTrimmedLeft (18).reduced (0, 4);

    constexpr float minDb = -48.0f, maxDb = 6.0f;
    auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (minDb, maxDb, db), minDb, maxDb, (float) bars.getBottom(), (float) bars.getY()); };

    const int barWidth = 6, pairGap = 8;
    int x = bars.getX();

    auto drawBar = [&] (float db, juce::Colour colour)
    {
        const auto r = juce::Rectangle<float> ((float) x, (float) bars.getY(), (float) barWidth, (float) bars.getHeight());
        g.setColour (pal.track.withAlpha (0.8f));
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (db > 0.0f ? juce::Colour (0xffff5c5c) : colour);
        g.fillRoundedRectangle (r.withTop (yFor (db)), 2.0f);
        x += barWidth + 2;
    };

    drawBar (inDb[0], pal.textDim);
    drawBar (inDb[1], pal.textDim);
    x += pairGap - 2;
    const int outX = x;
    drawBar (outDb[0], pal.accent);
    drawBar (outDb[1], pal.accent);

    g.setFont (klaud::font (9.0f));
    for (float db : { 0.0f, -6.0f, -12.0f, -24.0f, -36.0f })
    {
        const float y = yFor (db);
        g.setColour (pal.textDim.withAlpha (0.7f));
        g.drawText (juce::String ((int) db), 0, (int) y - 6, 16, 12, juce::Justification::centredRight, false);
    }

    drawCaption (g, "IN", { bars.getX() - 4, bars.getBottom() + 2, barWidth * 2 + 10, 12 });
    drawCaption (g, "OUT", { outX - 6, bars.getBottom() + 2, barWidth * 2 + 14, 12 });
}

//==============================================================================
LoudnessReadout::LoudnessReadout (DynMapProcessor& p) : processor (p)
{
    setTooltip ("Output short-term loudness (LUFS), highest true peak (click to reset), limiter gain reduction and auto gain");
    startTimerHz (15);
}

void LoudnessReadout::timerCallback()
{
    auto& m = processor.engine.meters;
    lufs = m.outLoudness.getLufs();
    truePeakDb = juce::Decibels::gainToDecibels (m.truePeakMax.load(), -100.0f);

    // Deepest recent gain reduction, easing back towards 0.
    limiterDb = juce::jmin (m.limiterGainDb.exchange (0.0f), limiterDb * 0.85f);
    autoGainDb = m.autoGainDb.load();
    repaint();
}

void LoudnessReadout::mouseDown (const juce::MouseEvent&)
{
    processor.engine.meters.resetTruePeak.store (true);
}

void LoudnessReadout::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    const bool autoGainOn = processor.apvts.getRawParameterValue (ids::autoGain)->load() > 0.5f;

    struct Item { juce::String label, value; juce::Colour colour; };
    const Item items[] {
        { "LUFS-S", lufs > -99.0f ? juce::String (lufs, 1) : "-", pal.text },
        { "TRUE PEAK", truePeakDb > -99.0f ? juce::String (truePeakDb, 1) : "-", truePeakDb > 0.0f ? juce::Colour (0xffff5c5c) : pal.text },
        { "LIMITER", limiterDb < -0.05f ? juce::String (limiterDb, 1) : "0.0", limiterDb < -0.05f ? colours::master : pal.textDim },
        { "AUTO GAIN", autoGainOn ? formatDb (autoGainDb) : "off", autoGainOn ? pal.text : pal.textDim } };

    const auto area = getLocalBounds();
    const int cellWidth = area.getWidth() / 2, cellHeight = area.getHeight() / 2;

    for (int i = 0; i < 4; ++i)
    {
        auto cell = juce::Rectangle<int> (area.getX() + (i % 2) * cellWidth, area.getY() + (i / 2) * cellHeight, cellWidth, cellHeight).reduced (2, 1);
        drawCaption (g, items[i].label, cell.removeFromTop (12), juce::Justification::centredLeft);
        g.setColour (items[i].colour);
        g.setFont (klaud::font (13.0f, true));
        g.drawText (items[i].value, cell, juce::Justification::centredLeft, false);
    }
}
}
