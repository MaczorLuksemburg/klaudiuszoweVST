#include "Components.h"

namespace msc::ui
{
namespace
{
    juce::RangedAudioParameter& getParam (APVTS& state, const juce::String& id)
    {
        auto* param = state.getParameter (id);
        jassert (param != nullptr);
        return *param;
    }

    void fillChoices (juce::ComboBox& box, juce::RangedAudioParameter& param)
    {
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&param))
            box.addItemList (choice->choices, 1);

        box.setScrollWheelEnabled (true);   // quick stepping through options without opening the menu
    }

    float defaultValueOf (juce::RangedAudioParameter& param)
    {
        return param.convertFrom0to1 (param.getDefaultValue());
    }

    constexpr float minFreq = 20.0f, maxFreq = 20000.0f;
}

const klaud::Palette& palette()
{
    static const klaud::Palette p = []
    {
        klaud::Palette pal;
        pal.background   = juce::Colour (0xff0a0f18);
        pal.panel        = juce::Colour (0xff101826);
        pal.panelOutline = juce::Colour (0xff1d2a3e);
        pal.track        = juce::Colour (0xff1c283a);
        pal.knobTop      = juce::Colour (0xff34445c);
        pal.knobBottom   = juce::Colour (0xff141c29);
        pal.text         = juce::Colour (0xffdae5f3);
        pal.textDim      = juce::Colour (0xff7087a4);
        pal.accent       = colours::input;
        return pal;
    }();

    return p;
}

void drawCaption (juce::Graphics& g, const juce::String& text, juce::Rectangle<int> area, juce::Justification justification)
{
    g.setColour (palette().textDim);
    g.setFont (klaud::font (10.5f, true));
    g.drawText (text, area, justification, false);
}

//==============================================================================
Knob::Knob (APVTS& state, const juce::String& paramId, const juce::String& titleText, juce::Colour accent, bool arcFromStart)
    : title (titleText)
{
    slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 90, 18);
    slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
    slider.setColour (juce::Slider::textBoxHighlightColourId, accent.withAlpha (0.4f));
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    slider.setMouseDragSensitivity (220);

    if (arcFromStart)
        slider.getProperties().set (klaud::arcFromStartProperty, true);

    addAndMakeVisible (slider);

    attachment = std::make_unique<APVTS::SliderAttachment> (state, paramId, slider);
    slider.setDoubleClickReturnValue (true, defaultValueOf (getParam (state, paramId)));
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
void PowerButton::paintButton (juce::Graphics& g, bool highlighted, bool)
{
    const auto area = getLocalBounds().toFloat().reduced (2.0f);
    const auto centre = area.getCentre();
    const float radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f;
    const bool on = getToggleState();

    g.setColour (on ? accent.withAlpha (0.16f) : palette().panel.brighter (highlighted ? 0.15f : 0.06f));
    g.fillEllipse (area);
    g.setColour (on ? accent.withAlpha (0.7f) : palette().panelOutline.brighter (highlighted ? 0.4f : 0.1f));
    g.drawEllipse (area.reduced (0.5f), 1.0f);

    const auto colour = on ? accent : palette().textDim.withAlpha (highlighted ? 1.0f : 0.7f);
    const float iconRadius = radius * 0.48f;

    juce::Path arc;
    arc.addCentredArc (centre.x, centre.y + 0.5f, iconRadius, iconRadius, 0.0f, 0.75f,
                       juce::MathConstants<float>::twoPi - 0.75f, true);
    g.setColour (colour);
    g.strokePath (arc, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.drawLine (centre.x, centre.y - iconRadius - 1.0f, centre.x, centre.y - 0.5f, 1.6f);
}

//==============================================================================
ModulePanel::ModulePanel (APVTS& state, const char* onParamId, const juce::String& titleText,
                          const juce::String& subtitleText, juce::Colour accentColour)
    : apvts (state), accent (accentColour), title (titleText), subtitle (subtitleText), power (accentColour)
{
    power.setTooltip ("Module on/off. Touching any control switches the module on.");
    addAndMakeVisible (power);
    powerAttachment = std::make_unique<APVTS::ButtonAttachment> (apvts, onParamId, power);

    onWatcher = std::make_unique<juce::ParameterAttachment> (getParam (apvts, onParamId), [this] (float value)
    {
        isOn = value > 0.5f;
        repaint();
    });
    onWatcher->sendInitialUpdate();

    addMouseListener (this, true);
}

ModulePanel::~ModulePanel()
{
    removeMouseListener (this);
}

juce::Rectangle<int> ModulePanel::getContentBounds() const
{
    return getLocalBounds().withTrimmedTop (headerHeight).reduced (12, 0).withTrimmedBottom (12);
}

void ModulePanel::resized()
{
    power.setBounds (10, 10, 24, 24);
    layoutContent (getContentBounds());
}

void ModulePanel::mouseDown (const juce::MouseEvent& e)
{
    if (! isOn && e.eventComponent != &power)
        onWatcher->setValueAsCompleteGesture (1.0f);
}

void ModulePanel::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails&)
{
    // Scrolling a knob or menu inside an off module changes it, so switch the module on too.
    if (! isOn && e.eventComponent != this && e.eventComponent != &power)
        onWatcher->setValueAsCompleteGesture (1.0f);
}

void ModulePanel::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    const auto area = getLocalBounds().toFloat();

    g.setColour (pal.panel);
    g.fillRoundedRectangle (area, 7.0f);
    g.setColour (pal.panelOutline);
    g.drawRoundedRectangle (area.reduced (0.5f), 7.0f, 1.0f);

    // Accent strip along the top edge.
    g.setColour (isOn ? accent : accent.withAlpha (0.22f));
    g.fillRoundedRectangle (area.getX() + 14.0f, 0.0f, area.getWidth() - 28.0f, 2.0f, 1.0f);

    g.setColour (isOn ? pal.text : pal.textDim);
    g.setFont (klaud::font (13.0f, true));
    g.drawText (title, 42, 7, getWidth() - 50, 16, juce::Justification::centredLeft, true);

    g.setColour (isOn ? accent.withAlpha (0.8f) : pal.textDim.withAlpha (0.6f));
    g.setFont (klaud::font (10.5f));
    g.drawText (subtitle, 42, 22, getWidth() - 50, 13, juce::Justification::centredLeft, true);

    paintContent (g);
}

void ModulePanel::paintOverChildren (juce::Graphics& g)
{
    if (isOn)
        return;

    g.setColour (palette().panel.withAlpha (0.62f));
    g.fillRect (getContentBounds().expanded (8, 4));
}

//==============================================================================
FilterDisplay::FilterDisplay (APVTS& state, const char* cutoffId, const char* shapeId, const char* slopeId,
                              SpectrumAnalyzer& spectrum, juce::Colour accentColour)
    : analyzer (spectrum),
      accent (accentColour),
      cutoffParam (getParam (state, cutoffId)),
      cutoff (state.getRawParameterValue (cutoffId)),
      shape (state.getRawParameterValue (shapeId)),
      slope (state.getRawParameterValue (slopeId)),
      cutoffGesture (cutoffParam, [] (float) {})
{
    setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    setTooltip ("Drag to move the cutoff. Double-click to reset.");
    analyzer.setEnabled (true);
    startTimerHz (30);
}

FilterDisplay::~FilterDisplay()
{
    analyzer.setEnabled (false);
}

float FilterDisplay::xForFreq (float freq) const
{
    return (float) getWidth() * std::log (freq / minFreq) / std::log (maxFreq / minFreq);
}

float FilterDisplay::freqForX (float x) const
{
    return minFreq * std::pow (maxFreq / minFreq, juce::jlimit (0.0f, 1.0f, x / (float) juce::jmax (1, getWidth())));
}

void FilterDisplay::timerCallback()
{
    bool changed = analyzer.process();

    const float c = cutoff->load(), s = shape->load(), sl = slope->load();

    if (c != lastCutoff || s != lastShape || sl != lastSlope)
    {
        lastCutoff = c; lastShape = s; lastSlope = sl;
        changed = true;
    }

    if (changed)
        repaint();
}

void FilterDisplay::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    const auto area = getLocalBounds().toFloat();
    const float w = area.getWidth(), h = area.getHeight();

    juce::Path clipShape;
    clipShape.addRoundedRectangle (area, 5.0f);

    g.setColour (pal.background.withAlpha (0.85f));
    g.fillPath (clipShape);

    juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (clipShape);

    // Grid.
    g.setFont (klaud::font (9.5f));

    for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
    {
        const float x = xForFreq (f);
        const bool major = (f == 100.0f || f == 1000.0f || f == 10000.0f);
        g.setColour (juce::Colours::white.withAlpha (major ? 0.07f : 0.035f));
        g.drawVerticalLine (juce::roundToInt (x), 0.0f, h);

        if (major)
        {
            g.setColour (pal.textDim.withAlpha (0.55f));
            g.drawText (f >= 1000.0f ? juce::String (juce::roundToInt (f / 1000.0f)) + "k" : juce::String (juce::roundToInt (f)),
                        juce::Rectangle<float> (x + 3.0f, h - 13.0f, 30.0f, 11.0f), juce::Justification::centredLeft, false);
        }
    }

    // Spectrum (-90..0 dB, tilted).
    {
        juce::Path spectrum;
        spectrum.startNewSubPath (0.0f, h);

        for (float x = 0.0f; x <= w; x += 2.0f)
        {
            const float db = analyzer.getLevelAt (freqForX (x));
            const float y = juce::jmap (juce::jlimit (-90.0f, 0.0f, db), -90.0f, 0.0f, h, h * 0.08f);
            spectrum.lineTo (x, y);
        }

        spectrum.lineTo (w, h);
        spectrum.closeSubPath();

        g.setColour (pal.textDim.withAlpha (0.13f));
        g.fillPath (spectrum);
        g.setColour (pal.textDim.withAlpha (0.28f));
        g.strokePath (spectrum, juce::PathStrokeType (1.0f));
    }

    // Filter response (-48..+6 dB).
    const float fc = cutoff->load(), sh = shape->load();
    const int slopeIndex = (int) std::lround (slope->load());

    // Not clamped at the bottom: where the response falls below -48 dB the curve leaves the
    // display (and is clipped away) instead of lying flat along the floor like a shelf.
    auto yForDb = [h] (float db) { return juce::jmap (juce::jmin (6.0f, db), -48.0f, 6.0f, h - 1.0f, 4.0f); };

    juce::Path curve;

    for (float x = 0.0f; x <= w; x += 1.0f)
    {
        const float db = juce::Decibels::gainToDecibels (dsp::filterMagnitude (freqForX (x), fc, sh, slopeIndex), -60.0f);
        const float y = yForDb (db);

        if (x == 0.0f) curve.startNewSubPath (x, y);
        else           curve.lineTo (x, y);
    }

    juce::Path fill (curve);
    fill.lineTo (w, h + 100.0f);
    fill.lineTo (0.0f, h + 100.0f);
    fill.closeSubPath();

    g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.28f), 0.0f, 0.0f, accent.withAlpha (0.04f), 0.0f, h, false));
    g.fillPath (fill);
    g.setColour (accent);
    g.strokePath (curve, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    // Cutoff handle.
    const float cx = xForFreq (juce::jlimit (minFreq, maxFreq, fc));
    const float cy = juce::jmin (h - 7.0f, yForDb (juce::Decibels::gainToDecibels (dsp::filterMagnitude (fc, fc, sh, slopeIndex), -60.0f)));
    g.setColour (accent.withAlpha (0.25f));
    g.drawVerticalLine (juce::roundToInt (cx), 0.0f, h);
    g.setColour (pal.background);
    g.fillEllipse (cx - 5.0f, cy - 5.0f, 10.0f, 10.0f);
    g.setColour (accent);
    g.drawEllipse (cx - 5.0f, cy - 5.0f, 10.0f, 10.0f, 1.8f);

    // Readouts.
    const juce::StringArray slopeNames { "12 dB", "24 dB", "48 dB" };
    g.setFont (klaud::font (11.0f, true));
    g.setColour (pal.text.withAlpha (0.85f));
    g.drawText (cutoffParam.getCurrentValueAsText(), juce::Rectangle<float> (6.0f, 4.0f, w - 12.0f, 14.0f),
                juce::Justification::centredRight, false);
    g.setColour (pal.textDim);
    g.setFont (klaud::font (10.5f));
    g.drawText (slopeNames[juce::jlimit (0, 2, slopeIndex)], juce::Rectangle<float> (6.0f, 18.0f, w - 12.0f, 12.0f),
                juce::Justification::centredRight, false);
}

void FilterDisplay::mouseDown (const juce::MouseEvent& e)
{
    cutoffGesture.beginGesture();
    cutoffGesture.setValueAsPartOfGesture (freqForX ((float) e.x));
}

void FilterDisplay::mouseDrag (const juce::MouseEvent& e)
{
    cutoffGesture.setValueAsPartOfGesture (juce::jlimit (minFreq, 22000.0f, freqForX ((float) e.x)));
}

void FilterDisplay::mouseUp (const juce::MouseEvent&)
{
    cutoffGesture.endGesture();
}

void FilterDisplay::mouseDoubleClick (const juce::MouseEvent&)
{
    cutoffGesture.setValueAsCompleteGesture (defaultValueOf (cutoffParam));
}

//==============================================================================
FilterSection::FilterSection (APVTS& state, const char* shapeId, const char* cutoffId, const char* slopeId,
                              SpectrumAnalyzer& analyzer, juce::Colour accent)
    : display (state, cutoffId, shapeId, slopeId, analyzer, accent)
{
    shape.setSliderStyle (juce::Slider::LinearVertical);
    shape.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    shape.setColour (juce::Slider::trackColourId, accent);
    shape.getProperties().set (klaud::noFillProperty, true);
    shape.setTooltip ("Filter shape: low-pass, band-pass, high-pass");

    cutoff.setSliderStyle (juce::Slider::LinearHorizontal);
    cutoff.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    cutoff.setColour (juce::Slider::trackColourId, accent);
    cutoff.setTooltip ("Cutoff frequency");

    fillChoices (slope, getParam (state, slopeId));

    addAndMakeVisible (shape);
    addAndMakeVisible (cutoff);
    addAndMakeVisible (slope);
    addAndMakeVisible (display);

    shapeAttachment  = std::make_unique<APVTS::SliderAttachment> (state, shapeId, shape);
    cutoffAttachment = std::make_unique<APVTS::SliderAttachment> (state, cutoffId, cutoff);
    slopeAttachment  = std::make_unique<APVTS::ComboBoxAttachment> (state, slopeId, slope);

    shape.setDoubleClickReturnValue (true, defaultValueOf (getParam (state, shapeId)));
    cutoff.setDoubleClickReturnValue (true, defaultValueOf (getParam (state, cutoffId)));
}

void FilterSection::resized()
{
    auto area = getLocalBounds();
    auto bottom = area.removeFromBottom (24);
    area.removeFromBottom (8);

    auto column = area.removeFromLeft (44);
    area.removeFromLeft (6);

    shape.setBounds (column.withTrimmedLeft (20));
    display.setBounds (area);

    slope.setBounds (bottom.removeFromLeft (94));
    bottom.removeFromLeft (10);
    cutoff.setBounds (bottom);
}

void FilterSection::paint (juce::Graphics& g)
{
    // Labels at the thumb positions of the shape slider (its travel is inset by the thumb radius).
    const auto sliderBounds = shape.getBounds();
    const int top = sliderBounds.getY() + 7, bottom = sliderBounds.getBottom() - 7;
    const juce::String labels[] { "HP", "BP", "LP" };
    const int positions[] { top, (top + bottom) / 2, bottom };

    for (int i = 0; i < 3; ++i)
    {
        drawCaption (g, labels[i], { 0, positions[i] - 7, 20, 14 }, juce::Justification::centredLeft);
        g.setColour (palette().textDim.withAlpha (0.4f));
        g.drawHorizontalLine (positions[i], (float) sliderBounds.getX() + 2.0f, (float) sliderBounds.getX() + 6.0f);
    }
}

//==============================================================================
InputModule::InputModule (APVTS& state, SpectrumAnalyzer& analyzer)
    : ModulePanel (state, ids::inOn, "INPUT", "band split & source", colours::input),
      filter (state, ids::inShape, ids::inCutoff, ids::inSlope, analyzer, colours::input)
{
    fillChoices (wetSource, getParam (state, ids::inWetSrc));
    fillChoices (drySource, getParam (state, ids::inDrySrc));
    wetSource.setTooltip ("How the filtered band is fed into the stereo modules");
    drySource.setTooltip ("How the rest of the signal (bypassing the modules) is passed through");

    addAndMakeVisible (wetSource);
    addAndMakeVisible (drySource);
    addAndMakeVisible (filter);

    wetAttachment = std::make_unique<APVTS::ComboBoxAttachment> (state, ids::inWetSrc, wetSource);
    dryAttachment = std::make_unique<APVTS::ComboBoxAttachment> (state, ids::inDrySrc, drySource);
}

void InputModule::layoutContent (juce::Rectangle<int> area)
{
    auto top = area.removeFromTop (40);
    area.removeFromTop (12);

    auto left = top.removeFromLeft ((top.getWidth() - 10) / 2);
    top.removeFromLeft (10);

    wetCaption = left.removeFromTop (16);
    wetSource.setBounds (left.withHeight (24));
    dryCaption = top.removeFromTop (16);
    drySource.setBounds (top.withHeight (24));

    filter.setBounds (area);
}

void InputModule::paintContent (juce::Graphics& g)
{
    drawCaption (g, "PROCESSED", wetCaption, juce::Justification::centredLeft);
    drawCaption (g, "UNPROCESSED", dryCaption, juce::Justification::centredLeft);
}

//==============================================================================
ModScopeDisplay::ModScopeDisplay (APVTS& state, ModScope& modScope, juce::Colour accentColour)
    : scope (modScope), accent (accentColour), clipMode (state.getRawParameterValue (ids::dpClip))
{
    setTooltip ("Pan movement before (dim) and after (bright) the mod clipper. Up = right, down = left.");
    scope.setEnabled (true);
    startTimerHz (30);
}

ModScopeDisplay::~ModScopeDisplay()
{
    scope.setEnabled (false);
}

void ModScopeDisplay::timerCallback()
{
    const float mode = clipMode->load();

    if (scope.process() || mode != lastClipMode)
    {
        lastClipMode = mode;
        repaint();
    }
}

void ModScopeDisplay::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    auto area = getLocalBounds().toFloat();

    constexpr float range = 1.6f;   // +-1 is a hard pan; the rest shows how far the modulation overshoots
    const int mode = (int) std::lround (clipMode->load());

    auto panel = [&] (juce::Rectangle<float> r)
    {
        g.setColour (pal.background.withAlpha (0.85f));
        g.fillRoundedRectangle (r, 5.0f);
    };

    // ---- transfer curve (square, right) ----
    const auto transfer = area.removeFromRight (area.getHeight());
    area.removeFromRight (8.0f);
    const auto wave = area;

    panel (transfer);
    {
        juce::Graphics::ScopedSaveState saved (g);
        g.reduceClipRegion (transfer.toNearestInt());

        const auto t = transfer.reduced (6.0f);
        auto toPoint = [&] (float in, float out)
        {
            return juce::Point<float> (juce::jmap (in, -range, range, t.getX(), t.getRight()),
                                       juce::jmap (out, -range, range, t.getBottom(), t.getY()));
        };

        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawLine ({ toPoint (-range, 0.0f), toPoint (range, 0.0f) });
        g.drawLine ({ toPoint (0.0f, -range), toPoint (0.0f, range) });

        const float dashes[] { 3.0f, 3.0f };
        g.setColour (pal.textDim.withAlpha (0.35f));
        g.drawDashedLine ({ toPoint (-range, 1.0f), toPoint (range, 1.0f) }, dashes, 2);
        g.drawDashedLine ({ toPoint (-range, -1.0f), toPoint (range, -1.0f) }, dashes, 2);
        g.drawLine ({ toPoint (-range, -range), toPoint (range, range) }, 0.8f);

        juce::Path curve;
        for (int i = 0; i <= 64; ++i)
        {
            const float in = juce::jmap ((float) i, 0.0f, 64.0f, -range, range);
            const auto point = toPoint (in, dsp::clip (in, mode));
            if (i == 0) curve.startNewSubPath (point);
            else        curve.lineTo (point);
        }

        g.setColour (accent);
        g.strokePath (curve, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // Where the latest peak sits on the curve.
        const auto& latest = scope.getColumn (ModScope::numColumns - 1);
        const float peak = std::abs (latest.preMax) > std::abs (latest.preMin) ? latest.preMax : latest.preMin;
        const auto dot = toPoint (juce::jlimit (-range, range, peak), juce::jlimit (-range, range, dsp::clip (peak, mode)));
        g.setColour (pal.background);
        g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (dot));
        g.setColour (accent);
        g.drawEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (dot), 1.6f);
    }

    // ---- scrolling waveform (left) ----
    panel (wave);
    juce::Graphics::ScopedSaveState saved (g);
    g.reduceClipRegion (wave.toNearestInt());

    const float cy = wave.getCentreY();
    const float halfHeight = wave.getHeight() * 0.5f - 4.0f;
    auto yFor = [&] (float v) { return cy - juce::jlimit (-range, range, v) / range * halfHeight; };

    g.setColour (juce::Colours::white.withAlpha (0.06f));
    g.drawHorizontalLine (juce::roundToInt (cy), wave.getX(), wave.getRight());

    const float dashes[] { 3.0f, 3.0f };
    g.setColour (pal.textDim.withAlpha (0.35f));
    g.drawDashedLine ({ wave.getX(), yFor (1.0f), wave.getRight(), yFor (1.0f) }, dashes, 2);
    g.drawDashedLine ({ wave.getX(), yFor (-1.0f), wave.getRight(), yFor (-1.0f) }, dashes, 2);

    const int columns = juce::jmin (ModScope::numColumns, (int) wave.getWidth());

    for (int i = 0; i < columns; ++i)
    {
        const auto& column = scope.getColumn (ModScope::numColumns - columns + i);
        const float x = wave.getX() + (float) i + 0.5f;

        g.setColour (pal.textDim.withAlpha (0.35f));
        g.drawLine (x, yFor (column.preMax), x, yFor (column.preMin) + 0.5f, 1.0f);
        g.setColour (accent.withAlpha (0.9f));
        g.drawLine (x, yFor (column.postMax), x, yFor (column.postMin) + 0.5f, 1.0f);
    }

    g.setFont (klaud::font (9.5f, true));
    g.setColour (pal.textDim.withAlpha (0.7f));
    g.drawText ("R", juce::Rectangle<float> (wave.getX() + 5.0f, yFor (1.0f) - 12.0f, 12.0f, 11.0f), juce::Justification::centredLeft, false);
    g.drawText ("L", juce::Rectangle<float> (wave.getX() + 5.0f, yFor (-1.0f) + 1.0f, 12.0f, 11.0f), juce::Justification::centredLeft, false);
}

//==============================================================================
DynamicPanModule::DynamicPanModule (APVTS& state, SpectrumAnalyzer& analyzer, ModScope& modScope)
    : ModulePanel (state, ids::dpOn, "DYNAMIC PAN", "the signal pans itself", colours::dynPan),
      amount (state, ids::dpAmount, "AMOUNT", colours::dynPan),
      scope (state, modScope, colours::dynPan),
      filter (state, ids::dpShape, ids::dpCutoff, ids::dpSlope, analyzer, colours::dynPan)
{
    amount.slider.textFromValueFunction = [this] (double v) { return juce::String (juce::roundToInt (v * maxPercent())) + " %"; };
    amount.slider.valueFromTextFunction = [this] (const juce::String& text)
    {
        return juce::jlimit (0.0, 1.0, text.retainCharacters ("0123456789.").getDoubleValue() / (double) maxPercent());
    };
    addAndMakeVisible (amount);

    setUpNumberBox (maxBox, "Maximum of the Amount knob. Drag or double-click to type.");
    maxBox.setMouseDragSensitivity (200);   // wide 10-1000 % range: twice the speed of the other boxes
    sliderAttachments.push_back (std::make_unique<APVTS::SliderAttachment> (state, ids::dpMax, maxBox));

    setUpNumberBox (thresholdBox, "Compressor threshold. Everything below it is lifted by the ratio.");
    sliderAttachments.push_back (std::make_unique<APVTS::SliderAttachment> (state, ids::dpThresh, thresholdBox));

    setUpCombo (source, ids::dpSource, "Which channel drives the panning: the sum of both, left or right");
    setUpCombo (comp, ids::dpComp, "Compresses the modulator (10 ms attack, 150 ms release, auto makeup) so quiet parts pan as hard as loud ones");
    setUpCombo (clip, ids::dpClip, "Limits the pan movement to hard left/right. Shapes the modulation, not the audio.");
    setUpCombo (makeup, ids::dpMakeup, "How much of the compressor's auto makeup gain to apply. 100 % keeps full scale at full scale and lifts everything below it the most.");

    addAndMakeVisible (scope);
    addAndMakeVisible (filter);

    maxWatcher = std::make_unique<juce::ParameterAttachment> (getParam (state, ids::dpMax),
                                                              [this] (float) { amount.slider.updateText(); });
    maxWatcher->sendInitialUpdate();
}

void DynamicPanModule::setUpNumberBox (juce::Slider& box, const juce::String& tooltip)
{
    box.setSliderStyle (juce::Slider::LinearBarVertical);   // drag up/down like the knobs
    box.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 60, 24);
    box.setColour (juce::Slider::trackColourId, colours::dynPan);
    box.setSliderSnapsToMousePosition (false);
    box.setMouseDragSensitivity (400);
    box.setTooltip (tooltip);
    addAndMakeVisible (box);
}

void DynamicPanModule::setUpCombo (juce::ComboBox& box, const char* paramId, const juce::String& tooltip)
{
    fillChoices (box, getParam (apvts, paramId));
    box.setTooltip (tooltip);
    addAndMakeVisible (box);
    comboAttachments.push_back (std::make_unique<APVTS::ComboBoxAttachment> (apvts, paramId, box));
}

float DynamicPanModule::maxPercent() const
{
    return apvts.getRawParameterValue (ids::dpMax)->load();
}

void DynamicPanModule::layoutContent (juce::Rectangle<int> area)
{
    constexpr int row = 40, rowGap = 8, captionHeight = 16, columnGap = 8;

    // Two groups: the pan (next to the Amount knob) and the modulator compressor.
    auto top = area.removeFromTop (3 * row + 2 * rowGap);
    amount.setBounds (top.removeFromLeft (top.getHeight()));
    top.removeFromLeft (12);

    auto panColumn = top.removeFromLeft ((top.getWidth() - 2 * columnGap) / 2);
    top.removeFromLeft (columnGap);
    dividerX = top.getX();
    top.removeFromLeft (columnGap);
    auto compColumn = top;

    auto placeColumn = [&] (juce::Rectangle<int> column,
                            std::initializer_list<std::pair<juce::Rectangle<int>*, juce::Component*>> items)
    {
        for (auto [caption, component] : items)
        {
            auto cell = column.removeFromTop (row);
            column.removeFromTop (rowGap);
            *caption = cell.removeFromTop (captionHeight);
            component->setBounds (cell);
        }
    };

    // Reads like the chain: source -> amount range -> clip; Max sits beside Threshold.
    placeColumn (panColumn,  { { &sourceCaption, &source }, { &maxCaption, &maxBox }, { &clipCaption, &clip } });
    placeColumn (compColumn, { { &compCaption, &comp }, { &thresholdCaption, &thresholdBox }, { &makeupCaption, &makeup } });

    area.removeFromTop (10);
    scopeCaption = area.removeFromTop (captionHeight);
    area.removeFromTop (2);
    scope.setBounds (area.removeFromTop (84));

    area.removeFromTop (10);
    filterCaption = area.removeFromTop (captionHeight);
    area.removeFromTop (2);
    filter.setBounds (area);
}

void DynamicPanModule::paintContent (juce::Graphics& g)
{
    drawCaption (g, "MOD SOURCE", sourceCaption, juce::Justification::centredLeft);
    drawCaption (g, "COMP", compCaption, juce::Justification::centredLeft);
    drawCaption (g, "THRESHOLD", thresholdCaption, juce::Justification::centredLeft);
    drawCaption (g, "MAX", maxCaption, juce::Justification::centredLeft);
    drawCaption (g, "MOD CLIP", clipCaption, juce::Justification::centredLeft);
    drawCaption (g, "MAKEUP", makeupCaption, juce::Justification::centredLeft);

    g.setColour (palette().panelOutline.brighter (0.15f));
    g.drawVerticalLine (dividerX, (float) sourceCaption.getY() + 2.0f, (float) clip.getBottom() - 2.0f);
    drawCaption (g, "MODULATION", scopeCaption, juce::Justification::centredLeft);
    drawCaption (g, "MODULATOR FILTER", filterCaption, juce::Justification::centredLeft);
}

//==============================================================================
HaasModule::HaasModule (APVTS& state)
    : ModulePanel (state, ids::hsOn, "HAAS", "delay & polarity", colours::haas),
      left (state, ids::hsLeft, "LEFT", colours::haas),
      right (state, ids::hsRight, "RIGHT", colours::haas)
{
    addAndMakeVisible (left);
    addAndMakeVisible (right);

    const auto phi = juce::String (juce::CharPointer_UTF8 ("\xc3\x98"));

    for (auto* button : { &invertLeft, &invertRight })
    {
        button->setButtonText (phi);
        button->setClickingTogglesState (true);
        button->setColour (juce::TextButton::buttonOnColourId, colours::haas);
        button->setTooltip ("Flip polarity");
        addAndMakeVisible (button);
    }

    invertLeftAttachment  = std::make_unique<APVTS::ButtonAttachment> (state, ids::hsInvL, invertLeft);
    invertRightAttachment = std::make_unique<APVTS::ButtonAttachment> (state, ids::hsInvR, invertRight);
}

void HaasModule::layoutContent (juce::Rectangle<int> area)
{
    auto knobs = area.removeFromTop (150);
    left.setBounds (knobs.removeFromLeft (knobs.getWidth() / 2).withTrimmedRight (2));
    right.setBounds (knobs.withTrimmedLeft (2));

    area.removeFromTop (18);
    polarityCaption = area.removeFromTop (16);

    auto buttons = area.removeFromTop (26);
    const int half = buttons.getWidth() / 2;
    invertLeft.setBounds (buttons.removeFromLeft (half).withSizeKeepingCentre (40, 26));
    invertRight.setBounds (buttons.withSizeKeepingCentre (40, 26));

    hintArea = area.removeFromBottom (48);
}

void HaasModule::paintContent (juce::Graphics& g)
{
    drawCaption (g, "POLARITY", polarityCaption);

    g.setColour (palette().textDim.withAlpha (0.75f));
    g.setFont (klaud::font (10.5f));
    g.drawFittedText ("1-30 ms: width\n30 ms and up: echo\nkeep one side at 0 ms", hintArea, juce::Justification::centred, 3);
}

//==============================================================================
ChorusModule::ChorusModule (APVTS& state)
    : ModulePanel (state, ids::chOn, "CHORUS", "Juno-style stereo", colours::chorus),
      depth (state, ids::chDepth, "DEPTH", colours::chorus, true),
      width (state, ids::chWidth, "WIDTH", colours::chorus, true),
      tone (state, ids::chTone, "TONE", colours::chorus, true),
      mix (state, ids::chMix, "MIX", colours::chorus, true)
{
    const juce::StringArray names { "I", "II", "I+II" };

    for (int i = 0; i < 3; ++i)
    {
        auto& button = modes[(size_t) i];
        button.setButtonText (names[i]);
        button.setColour (juce::TextButton::buttonOnColourId, colours::chorus);
        button.setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < 2 ? juce::Button::ConnectedOnRight : 0));
        button.onClick = [this, i] { modeAttachment->setValueAsCompleteGesture ((float) i); };
        addAndMakeVisible (button);
    }

    modeAttachment = std::make_unique<juce::ParameterAttachment> (getParam (state, ids::chMode), [this] (float value)
    {
        const int selected = (int) std::lround (value);
        for (int i = 0; i < 3; ++i)
            modes[(size_t) i].setToggleState (i == selected, juce::dontSendNotification);
    });
    modeAttachment->sendInitialUpdate();

    for (auto* knob : { &depth, &width, &tone, &mix })
        addAndMakeVisible (knob);
}

void ChorusModule::layoutContent (juce::Rectangle<int> area)
{
    modeCaption = area.removeFromTop (16);

    auto row = area.removeFromTop (26);
    const int buttonWidth = (row.getWidth() - 8) / 3;

    for (auto& button : modes)
    {
        button.setBounds (row.removeFromLeft (buttonWidth));
        row.removeFromLeft (4);
    }

    area.removeFromTop (14);
    auto top = area.removeFromTop ((area.getHeight() - 12) / 2);
    area.removeFromTop (12);
    depth.setBounds (top.removeFromLeft (top.getWidth() / 2));
    width.setBounds (top);
    tone.setBounds (area.removeFromLeft (area.getWidth() / 2));
    mix.setBounds (area);
}

void ChorusModule::paintContent (juce::Graphics& g)
{
    drawCaption (g, "MODE", modeCaption, juce::Justification::centredLeft);
}

//==============================================================================
ImageModule::ImageModule (APVTS& state)
    : ModulePanel (state, ids::imOn, "IMAGE", "balance & M/S", colours::image),
      balance (state, ids::imBalance, "BALANCE", colours::image),
      mid (state, ids::imMid, "MID", colours::image),
      side (state, ids::imSide, "SIDE", colours::image)
{
    for (auto* knob : { &balance, &mid, &side })
        addAndMakeVisible (knob);
}

void ImageModule::layoutContent (juce::Rectangle<int> area)
{
    constexpr int spacing = 10;
    const int h = (area.getHeight() - 2 * spacing) / 3;
    balance.setBounds (area.removeFromTop (h));
    area.removeFromTop (spacing);
    mid.setBounds (area.removeFromTop (h));
    area.removeFromTop (spacing);
    side.setBounds (area);
}

//==============================================================================
PresetBar::PresetBar (PresetManager& manager)
    : presets (manager)
{
    previous.setTooltip ("Previous preset");
    next.setTooltip ("Next preset");
    save.setTooltip ("Save the current settings as a user preset");

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
    save.setBounds (area.removeFromRight (64));
    area.removeFromRight (10);
    previous.setBounds (area.removeFromLeft (28));
    area.removeFromLeft (4);
    next.setBounds (area.removeFromRight (28));
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
}
