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

    juce::String section;
    for (int i = 0; i < numFactory; ++i)
    {
        if (const auto category = presets.getCategory (i); category != section)
        {
            if (section.isNotEmpty())
                list.addSeparator();
            list.addSectionHeading (category);
            section = category;
        }
        list.addItem (names[i], i + 1);
    }

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
        { "Waveform",  "Follows the waveform itself, so the curve becomes distortion.",
          0.01f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0, 0.0f, 5.0f },
        // Maximus's default master band, measured on the VST: ATT 2 ms is a 2 ms lookahead, REL 85.53 ms with
        // release curve 3 is our Accel 3 at the same time, and its 10 ms sustain is our built-in peak window.
        { "Maximus",   "Loudness-maximizer timing: 2 ms lookahead, peak detection, an 86 ms release that starts slowly.",
          0.2f, 0.0f, 85.53f, 0.0f, 0.0f, 100.0f, 3, 0.5f, 40.0f, ids::accelRelease (3) },
        { "Auto",      "Program-dependent: short peaks recover fast, long loud passages slowly, so it rarely pumps. A safe all-rounder.",
          3.0f, 0.0f, 100.0f, 0.0f, 0.0f, 100.0f, 2, 0.5f, 40.0f, ids::relLawAuto },
        { "Vocal",     "Vocal levelling: soft eased attack, short RMS, quick release that settles slowly, so words stay even.",
          15.0f, 0.0f, 150.0f, 40.0f, 10.0f, 100.0f, 0, 1.0f, 50.0f, ids::relLawClassic, 2, 700.0f },
        { "Master",    "Loud, calm masters: eased attack behind 2 ms lookahead, slow-start release with a slow second release.",
          1.0f, 0.0f, 120.0f, 0.0f, 0.0f, 100.0f, 3, 0.5f, 40.0f, ids::accelRelease (3), 2, 400.0f },
    };

    return styles;
}

//==============================================================================
DetectorStyleBank::DetectorStyleBank (DynMapProcessor& p) : processor (p)
{
    load();
    startTimer (1000);
}

juce::File DetectorStyleBank::getFile()
{
    if (fileOverride() != juce::File())
        return fileOverride();
    return PresetManager::getUserPresetFolder().getParentDirectory().getChildFile ("DetectorStyles.xml");
}

void DetectorStyleBank::load()
{
    const auto file = getFile();
    loadedTime = file.getLastModificationTime();
    custom = {};

    if (auto xml = juce::XmlDocument::parse (file))
        for (auto* e : xml->getChildWithTagNameIterator ("STYLE"))
        {
            const int slot = e->getIntAttribute ("slot", -1);
            if (! juce::isPositiveAndBelow (slot, numCustom))
                continue;

            DetectorStyle s;
            s.name = e->getStringAttribute ("name", "Custom " + juce::String (slot + 1));
            s.description = {};
            s.attack = (float) e->getDoubleAttribute ("attack", 5.0);
            s.hold = (float) e->getDoubleAttribute ("hold");
            s.release = (float) e->getDoubleAttribute ("release", 120.0);
            s.relShape = (float) e->getDoubleAttribute ("relShape");
            s.rms = (float) e->getDoubleAttribute ("rms");
            s.link = (float) e->getDoubleAttribute ("link", 100.0);
            s.lookahead = e->getIntAttribute ("lookahead");
            s.smooth = (float) e->getDoubleAttribute ("smooth", 0.5);
            s.trTime = (float) e->getDoubleAttribute ("trTime", 40.0);
            s.relLaw = e->getIntAttribute ("relLaw");
            s.attLaw = e->getIntAttribute ("attLaw");
            s.release2 = (float) e->getDoubleAttribute ("release2");
            custom[(size_t) slot] = s;
        }

    ++version;
}

void DetectorStyleBank::timerCallback()
{
    // Another instance (or this one) saved a slot: pick it up.
    if (getFile().getLastModificationTime() != loadedTime)
        load();
}

const DetectorStyle* DetectorStyleBank::get (int index) const
{
    const auto& builtIn = detectorStyles();
    if (juce::isPositiveAndBelow (index, (int) builtIn.size()))
        return &builtIn[(size_t) index];

    const int slot = slotOf (index);
    return juce::isPositiveAndBelow (slot, numCustom) && custom[(size_t) slot].has_value() ? &*custom[(size_t) slot] : nullptr;
}

bool DetectorStyleBank::isEmpty (int index) const { return get (index) == nullptr; }

juce::String DetectorStyleBank::nameOf (int index) const
{
    if (const auto* s = get (index))
        return s->name;
    return "Custom " + juce::String (slotOf (index) + 1);
}

juce::String DetectorStyleBank::descriptionOf (int index) const
{
    if (! isCustom (index))
        return get (index)->description;

    const juce::String slotName = "Custom " + juce::String (slotOf (index) + 1);
    return isEmpty (index) ? "Empty slot. Open Advanced and choose \"Save to " + slotName + "\" in the menu next to the title."
                           : "Your own style (" + slotName + "), saved on this computer and shared by every DynMap.";
}

DetectorStyle DetectorStyleBank::read (int stage) const
{
    auto value = [this, stage] (const char* name) { return processor.apvts.getRawParameterValue (stageParamId (stage, name))->load(); };
    DetectorStyle s;
    s.attack = value (ids::attack);
    s.hold = value (ids::hold);
    s.release = value (ids::release);
    s.relShape = value (ids::relShape);
    s.rms = value (ids::rms);
    s.link = value (ids::link);
    s.lookahead = juce::roundToInt (value (ids::lookahead));
    s.smooth = value (ids::smooth);
    s.trTime = value (ids::trTime);
    s.relLaw = juce::roundToInt (value (ids::relLaw));
    s.attLaw = juce::roundToInt (value (ids::attLaw));
    s.release2 = value (ids::release2);
    return s;
}

int DetectorStyleBank::matching (int stage) const
{
    const auto now = read (stage);
    auto near = [] (float a, float b) { return std::abs (a - b) <= 0.02f * juce::jmax (std::abs (a), std::abs (b)) + 1.0e-3f; };

    for (int i = 0; i < size(); ++i)
        if (const auto* s = get (i))
            if (near (now.attack, s->attack) && near (now.hold, s->hold) && near (now.release, s->release)
                && near (now.relShape, s->relShape) && near (now.rms, s->rms) && near (now.link, s->link)
                && now.lookahead == s->lookahead && near (now.smooth, s->smooth) && near (now.trTime, s->trTime)
                && now.relLaw == s->relLaw && now.attLaw == s->attLaw && near (now.release2, s->release2))
                return i;

    return -1;
}

void DetectorStyleBank::apply (int stage, int index)
{
    const auto* s = get (index);
    if (s == nullptr)
        return;

    auto set = [this, stage] (const char* name, float v) { setParameter (processor.apvts, stageParamId (stage, name), v); };
    set (ids::attack, s->attack);
    set (ids::hold, s->hold);
    set (ids::release, s->release);
    set (ids::relShape, s->relShape);
    set (ids::rms, s->rms);
    set (ids::link, s->link);
    set (ids::lookahead, (float) s->lookahead);
    set (ids::smooth, s->smooth);
    set (ids::trTime, s->trTime);
    set (ids::relLaw, (float) s->relLaw);
    set (ids::attLaw, (float) s->attLaw);
    set (ids::release2, s->release2);
}

void DetectorStyleBank::save (int slot, int stage, const juce::String& name)
{
    if (! juce::isPositiveAndBelow (slot, numCustom))
        return;

    load();   // keep what other instances saved in the meantime
    auto s = read (stage);
    s.name = name.trim().isNotEmpty() ? name.trim().substring (0, 24) : "Custom " + juce::String (slot + 1);
    custom[(size_t) slot] = s;

    juce::XmlElement xml ("DETECTORSTYLES");
    for (int i = 0; i < numCustom; ++i)
        if (const auto& c = custom[(size_t) i])
        {
            auto* e = xml.createNewChildElement ("STYLE");
            e->setAttribute ("slot", i);
            e->setAttribute ("name", c->name);
            e->setAttribute ("attack", c->attack);
            e->setAttribute ("hold", c->hold);
            e->setAttribute ("release", c->release);
            e->setAttribute ("relShape", c->relShape);
            e->setAttribute ("rms", c->rms);
            e->setAttribute ("link", c->link);
            e->setAttribute ("lookahead", c->lookahead);
            e->setAttribute ("smooth", c->smooth);
            e->setAttribute ("trTime", c->trTime);
            e->setAttribute ("relLaw", c->relLaw);
            e->setAttribute ("attLaw", c->attLaw);
            e->setAttribute ("release2", c->release2);
        }

    const auto file = getFile();
    file.getParentDirectory().createDirectory();
    xml.writeTo (file);
    load();
}

//==============================================================================
DetectorStylePicker::DetectorStylePicker (DynMapProcessor& p, DetectorStyleBank& b) : processor (p), bank (b)
{
    for (int i = 0; i < bank.size(); ++i)
    {
        auto* button = buttons.add (new juce::TextButton());
        button->onClick = [this, i]
        {
            if (bank.isEmpty (i))
            {
                emptyHint = i;   // explain how to fill it
                repaint();
                return;
            }

            emptyHint = -1;
            bank.apply (stage, i);
            timerCallback();
        };
        addAndMakeVisible (button);
    }

    refreshButtons();
    startTimerHz (8);
}

void DetectorStylePicker::refreshButtons()
{
    bankVersion = bank.getVersion();

    for (int i = 0; i < buttons.size(); ++i)
    {
        auto* b = buttons[i];
        b->setButtonText (bank.nameOf (i));
        b->setTooltip (bank.descriptionOf (i));
        b->setAlpha (bank.isEmpty (i) ? 0.45f : 1.0f);
    }
}

void DetectorStylePicker::setStage (int newStage, juce::Colour newAccent)
{
    stage = newStage;
    accent = newAccent;

    for (auto* b : buttons)
        b->setColour (juce::TextButton::buttonOnColourId, accent);

    shown = -2;
    emptyHint = -1;
    timerCallback();
}

void DetectorStylePicker::timerCallback()
{
    if (bank.getVersion() != bankVersion)
    {
        refreshButtons();
        shown = -2;
    }

    const int match = bank.matching (stage);
    repaint();   // the summary line follows the knobs

    if (match == shown)
        return;

    shown = match;
    if (match >= 0)
        emptyHint = -1;
    for (int i = 0; i < buttons.size(); ++i)
        buttons[i]->setToggleState (i == match, juce::dontSendNotification);
    repaint();
}

void DetectorStylePicker::resized()
{
    auto area = getLocalBounds();
    const int columnWidth = (area.getWidth() - 2 * 8) / columns;

    for (int i = 0; i < buttons.size(); ++i)
        buttons[i]->setBounds (area.getX() + (i % columns) * (columnWidth + 8), area.getY() + (i / columns) * (buttonHeight + rowGap),
                               columnWidth, buttonHeight);
}

void DetectorStylePicker::paint (juce::Graphics& g)
{
    const auto& pal = palette();
    const int rows = (buttons.size() + columns - 1) / columns;
    auto area = getLocalBounds().withTrimmedTop (rows * buttonHeight + (rows - 1) * rowGap + 10).reduced (4, 0);

    const int described = emptyHint >= 0 ? emptyHint : shown;
    g.setFont (klaud::font (12.0f));
    g.setColour (described >= 0 ? pal.text : pal.textDim);
    g.drawFittedText (described >= 0 ? bank.descriptionOf (described)
                                     : juce::String ("Custom settings. Open Advanced to see or change them."),
                      area.removeFromTop (32), juce::Justification::topLeft, 2);

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
    g.drawFittedText (summary, area.removeFromTop (28), juce::Justification::topLeft, 2);
}

//==============================================================================
DetectorStyleMenu::DetectorStyleMenu (DetectorStyleBank& b, std::function<int()> stageGetter)
    : bank (b), getStage (std::move (stageGetter))
{
    previous.setTooltip ("Previous style");
    next.setTooltip ("Next style");
    list.setTooltip ("Which style the detector knobs match. Pick one to apply it, or save the current settings "
                     "into a custom slot (shared by every DynMap on this computer).");
    previous.onClick = [this] { step (-1); };
    next.onClick = [this] { step (1); };
    list.onChange = [this]
    {
        const int id = list.getSelectedId();
        if (id > saveIdBase)
        {
            askName (id - saveIdBase - 1);
            shown = -2;   // show the match again until a name is chosen
        }
        else if (id > 0 && id - 1 != bank.matching (getStage()))
        {
            bank.apply (getStage(), id - 1);
        }
    };

    for (auto* c : std::initializer_list<juce::Component*> { &previous, &list, &next })
        addAndMakeVisible (c);

    refresh();
    startTimerHz (8);
}

void DetectorStyleMenu::refresh()
{
    rebuild();
    timerCallback();
}

void DetectorStyleMenu::resized()
{
    auto area = getLocalBounds();
    previous.setBounds (area.removeFromLeft (24));
    next.setBounds (area.removeFromRight (24));
    area.reduce (3, 0);
    list.setBounds (area);
}

void DetectorStyleMenu::rebuild()
{
    bankVersion = bank.getVersion();
    list.clear (juce::dontSendNotification);

    const int numBuiltIn = (int) detectorStyles().size();
    for (int i = 0; i < numBuiltIn; ++i)
        list.addItem (bank.nameOf (i), i + 1);

    list.addSeparator();
    for (int slot = 0; slot < DetectorStyleBank::numCustom; ++slot)
    {
        const int index = bank.indexOfSlot (slot);
        list.addItem (bank.isEmpty (index) ? "Custom " + juce::String (slot + 1) + " (empty)" : bank.nameOf (index), index + 1);
        list.setItemEnabled (index + 1, ! bank.isEmpty (index));
    }

    list.addSeparator();
    list.addSectionHeading ("Save current settings");
    for (int slot = 0; slot < DetectorStyleBank::numCustom; ++slot)
    {
        const int index = bank.indexOfSlot (slot);
        const juce::String slotName = "Custom " + juce::String (slot + 1);
        list.addItem (bank.isEmpty (index) ? "Save to " + slotName
                                           : "Save to \"" + bank.nameOf (index) + "\" (" + slotName + ")",
                      saveIdBase + slot + 1);
    }

    shown = -2;
}

void DetectorStyleMenu::timerCallback()
{
    if (bank.getVersion() != bankVersion)
        rebuild();

    const int match = bank.matching (getStage());
    if (match == shown && list.getSelectedId() < saveIdBase)
        return;

    shown = match;
    if (match >= 0)
        list.setSelectedId (match + 1, juce::dontSendNotification);
    else
        list.setText ("Custom", juce::dontSendNotification);
}

void DetectorStyleMenu::step (int delta)
{
    const int count = bank.size();
    int index = bank.matching (getStage());

    for (int tries = 0; tries < count; ++tries)
    {
        index = ((index < 0 ? (delta > 0 ? -1 : 0) : index) + delta + count) % count;
        if (! bank.isEmpty (index))
        {
            bank.apply (getStage(), index);
            return;
        }
    }
}

void DetectorStyleMenu::askName (int slot)
{
    const int index = bank.indexOfSlot (slot);
    const auto current = bank.isEmpty (index) ? "Custom " + juce::String (slot + 1) : bank.nameOf (index);

    nameDialog = std::make_unique<juce::AlertWindow> ("Save detector style", "Name for Custom " + juce::String (slot + 1) + ":",
                                                      juce::MessageBoxIconType::NoIcon, this);
    nameDialog->setLookAndFeel (&getLookAndFeel());
    nameDialog->addTextEditor ("name", current);
    nameDialog->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    nameDialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));

    juce::Component::SafePointer<DetectorStyleMenu> safeThis (this);
    nameDialog->enterModalState (true, juce::ModalCallbackFunction::create ([safeThis, slot] (int result)
    {
        if (safeThis == nullptr || safeThis->nameDialog == nullptr)
            return;

        // Close the window either way; only Save (or Return) stores the slot.
        const auto name = safeThis->nameDialog->getTextEditorContents ("name");
        safeThis->nameDialog->setVisible (false);

        if (result == 1)
            safeThis->bank.save (slot, safeThis->getStage(), name);

        safeThis->shown = -2;
        safeThis->timerCallback();
    }), false);
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
