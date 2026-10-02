#include "PluginEditor.h"

using namespace dynmap;
using namespace dynmap::ui;

namespace
{
    constexpr int margin = 12, gap = 10, headerHeight = 56;
    constexpr int knobW = 70, knobH = 78;

    const juce::Identifier uiWidthId { "uiWidth" };
    const juce::Identifier selectedStageId { "selectedStage" };
    const juce::Identifier detectorAdvancedId { "detectorAdvanced" };

    // Lays components out left to right in a row of equal cells.
    void row (juce::Rectangle<int> area, std::initializer_list<juce::Component*> items, int cellWidth)
    {
        const int total = cellWidth * (int) items.size();
        int x = area.getX() + (area.getWidth() - total) / 2;

        for (auto* c : items)
        {
            if (c != nullptr)
                c->setBounds (x, area.getY(), cellWidth, area.getHeight());
            x += cellWidth;
        }
    }
}

//==============================================================================
DynMapMainView::DynMapMainView (DynMapProcessor& p, klaud::LookAndFeel& lookAndFeel)
    : processor (p),
      lookAndFeelSetter (*this, lookAndFeel),
      presetBar (p.presets),
      inputTab (p, inputStage, [this] { return selectedStage; }, [this] (int s) { selectStage (s); }),
      masterTab (p, masterStage, [this] { return selectedStage; }, [this] (int s) { selectStage (s); }),
      sidechainTab (p),
      bandDisplay (p, [this] { return selectedStage; }, [this] (int s) { selectStage (s); }),
      levelEditor (p, CurveKind::level),
      transientEditor (p, CurveKind::transient),
      detectorStyles (p),
      meter (p),
      readout (p)
{
    auto& apvts = p.apvts;

    quality.attach (apvts, ids::quality);
    phase.attach (apvts, ids::phase);
    quality.box.setTooltip ("Oversampling for saturation, waveshaping and the clipper (adds a little latency)");
    phase.box.setTooltip ("Crossover type: minimum phase (no extra latency) or linear phase (no phase shift, ~100 ms latency)");

    amount.attach (apvts, ids::amount);
    time.attach (apvts, ids::time);
    globalMix.attach (apvts, ids::globalMix);
    inGain.attach (apvts, ids::inGain);
    outGain.attach (apvts, ids::outGain);
    clip.attach (apvts, ids::clip);
    limiter.attach (apvts, ids::limiter);
    autoGain.attach (apvts, ids::autoGain);
    delta.attach (apvts, ids::delta);
    ceiling.attach (apvts, ids::ceiling);
    limRelease.attach (apvts, ids::limRel);

    amount.slider.setTooltip ("Scales every curve: 0 % is neutral, 100 % as drawn, 200 % exaggerated, below 0 % compression turns into expansion");
    time.slider.setTooltip ("Scales every attack, hold, release and transient time");
    globalMix.slider.setTooltip ("Dry/wet of the processing in every stage (input, bands, master). The dry part still goes "
                                 "through the band split, so nothing cancels; the clipper and limiter stay on.");
    outGain.slider.setTooltip ("Output gain; drives the clipper and limiter when they are on");
    clip.box.setTooltip ("Clips peaks at the ceiling before the limiter (oversampled)");
    limiter.setTooltip ("True-peak limiter: the output never goes over the ceiling");
    autoGain.setTooltip ("Matches the output loudness to the input, for fair comparisons");
    delta.setTooltip ("Plays only what the plugin changes (output minus input)");

    mode.box.setTooltip ("Dynamics: the level curve follows the detector. Waveshaper: the level curve bends every sample (distortion).");
    bypass.setTooltip ("Bypass this stage");
    solo.setTooltip ("Solo this band");
    mute.setTooltip ("Mute this band");
    pre.slider.setTooltip ("Gain before the stage: moves the signal along the curves");
    post.slider.setTooltip ("Gain after the stage (makeup)");
    mix.slider.setTooltip ("Dry/wet of this stage");
    width.slider.setTooltip ("Stereo width of this band");
    satType.box.setTooltip ("Saturation after or before the dynamics (oversampled)");
    drive.slider.setTooltip ("Saturation drive; lowers the point where saturation starts (Crush: fewer bits)");
    attack.slider.setTooltip ("How fast the detector follows rising levels");
    hold.slider.setTooltip ("How long the detector holds a peak before releasing");
    release.slider.setTooltip ("How fast the detector follows falling levels");
    relShape.slider.setTooltip ("Release shape: 0 % steady (dB per second), 100 % fast at first then slowing down");
    rms.slider.setTooltip ("0 = peak detection; above 0 the detector averages (RMS) over this time");
    link.slider.setTooltip ("How much the two channels share one detector (100 % = same gain on both)");
    trTime.slider.setTooltip ("Time scale of the transient detector: short catches clicks, long catches whole hits");
    smooth.slider.setTooltip ("Smooths the gain changes; 0 allows instant jumps (clicks on steps are then part of the sound)");
    maxBoost.slider.setTooltip ("Most the curves may boost (keeps quiet noise from being lifted too far)");
    maxCut.slider.setTooltip ("Most the curves may cut");
    scFilter.slider.setTooltip ("High-pass on the detector only, so bass doesn't drive the curve");
    lookahead.box.setTooltip ("Delays the audio so the detector sees peaks coming (adds latency)");
    stereo.box.setTooltip ("Process left/right or mid/side");
    scSource.box.setTooltip ("What drives this stage's curves: its own signal, the same band of the sidechain input, "
                             "or the whole sidechain (e.g. a kick ducking only the bass band)");

    for (auto* c : std::initializer_list<juce::Component*> {
             &presetBar, &quality, &phase, &inputTab, &masterTab, &sidechainTab, &bandDisplay, &levelEditor, &transientEditor,
             &mode, &bypass, &solo, &mute, &pre, &post, &mix, &width, &satType, &satPos, &drive,
             &attack, &hold, &release, &relShape, &rms, &link, &trTime, &smooth, &maxBoost, &maxCut, &scFilter,
             &lookahead, &stereo, &scSource, &amount, &time, &globalMix, &inGain, &outGain, &clip, &limiter,
             &autoGain, &delta, &ceiling, &limRelease, &meter, &readout, &detectorStyles, &advanced })
        addAndMakeVisible (c);

    addAndMakeVisible (linearScale);
    linearScale.setClickingTogglesState (true);
    linearScale.setTooltip ("Draw this level curve on linear amplitude axes, like Image-Line Maximus (0 dBFS in the middle, "
                            "the slope at the bottom-left corner sets the gain for quiet signals). The curve is converted.");
    linearScale.onClick = [this] { levelEditor.setLinear (linearScale.getToggleState()); };

    advanced.setClickingTogglesState (true);
    advanced.setTooltip ("Show every detector control instead of the styles");
    advanced.setToggleState ((bool) p.apvts.state.getProperty (detectorAdvancedId, false), juce::dontSendNotification);
    advanced.onClick = [this]
    {
        processor.apvts.state.setProperty (detectorAdvancedId, advanced.getToggleState(), nullptr);
        updateDetectorView();
    };
    updateDetectorView();

    const int saved = (int) p.apvts.state.getProperty (selectedStageId, bandStage (0));
    selectStage (juce::jlimit (0, numStages - 1, saved));

    setSize (baseWidth, baseHeight);
    startTimerHz (10);
}

DynMapMainView::~DynMapMainView()
{
    setLookAndFeel (nullptr);
}

void DynMapMainView::selectStage (int stage)
{
    // A band that isn't in use can't be selected; fall back to the lowest band.
    if (isBandStage (stage) && processor.engine.getLayout().positionOf (stage - 1) < 0)
        stage = bandStage (0);

    selectedStage = stage;
    processor.apvts.state.setProperty (selectedStageId, stage, nullptr);
    updateStageControls();
    repaint();
}

void DynMapMainView::updateStageControls()
{
    auto& apvts = processor.apvts;
    const int stage = selectedStage;
    auto id = [stage] (const char* name) { return stageParamId (stage, name); };

    stageAccent = stageColour (processor, stage);
    stageTitle = stageLabel (processor, stage);

    levelEditor.setStage (stage, stageAccent);
    detectorStyles.setStage (stage, stageAccent);
    advanced.setColour (juce::TextButton::buttonOnColourId, stageAccent);
    linearScale.setColour (juce::TextButton::buttonOnColourId, stageAccent);
    linearScale.setToggleState (levelEditor.isLinear(), juce::dontSendNotification);
    transientEditor.setStage (stage, stageAccent);

    mode.attach (apvts, id (ids::mode));
    bypass.attach (apvts, id (ids::bypass));
    pre.attach (apvts, id (ids::pre));
    post.attach (apvts, id (ids::post));
    mix.attach (apvts, id (ids::mix));
    satType.attach (apvts, id (ids::satType));
    satPos.attach (apvts, id (ids::satPos));
    drive.attach (apvts, id (ids::drive));
    attack.attach (apvts, id (ids::attack));
    hold.attach (apvts, id (ids::hold));
    release.attach (apvts, id (ids::release));
    relShape.attach (apvts, id (ids::relShape));
    rms.attach (apvts, id (ids::rms));
    link.attach (apvts, id (ids::link));
    trTime.attach (apvts, id (ids::trTime));
    smooth.attach (apvts, id (ids::smooth));
    maxBoost.attach (apvts, id (ids::maxBoost));
    maxCut.attach (apvts, id (ids::maxCut));
    scFilter.attach (apvts, id (ids::scFilter));
    lookahead.attach (apvts, id (ids::lookahead));
    stereo.attach (apvts, id (ids::stereo));
    scSource.attach (apvts, id (ids::scSource));

    const bool band = isBandStage (stage);
    width.setVisible (band);
    solo.setVisible (band);
    mute.setVisible (band);

    if (band)
    {
        width.attach (apvts, id (ids::width));
        solo.attach (apvts, id (ids::solo));
        mute.attach (apvts, id (ids::mute));
    }

    for (auto* k : { &pre, &post, &mix, &width, &drive, &attack, &hold, &release, &relShape, &rms, &link,
                     &trTime, &smooth, &maxBoost, &maxCut, &scFilter })
        k->setAccent (stageAccent);

    for (auto* t : { &bypass, &solo, &mute })
        t->setAccent (stageAccent);
}

void DynMapMainView::updateDetectorView()
{
    // Styles by default; every knob (what the styles set, plus limits and filters) under Advanced.
    const bool showAll = advanced.getToggleState();
    detectorStyles.setVisible (! showAll);

    for (auto* c : std::initializer_list<juce::Component*> { &attack, &hold, &release, &relShape, &rms, &link, &trTime,
                                                             &smooth, &maxBoost, &maxCut, &scFilter, &lookahead })
        c->setVisible (showAll);

    resized();
}

void DynMapMainView::timerCallback()
{
    // Keep the selection valid and the colours in step with band changes.
    if (isBandStage (selectedStage) && processor.engine.getLayout().positionOf (selectedStage - 1) < 0)
    {
        selectStage (bandStage (0));
        return;
    }

    const auto colour = stageColour (processor, selectedStage);
    const auto title = stageLabel (processor, selectedStage);

    linearScale.setToggleState (levelEditor.isLinear(), juce::dontSendNotification);

    const bool listensToSidechain = processor.apvts.getRawParameterValue (stageParamId (selectedStage, ids::scSource))->load() > 0.5f;
    const bool missing = listensToSidechain && processor.engine.meters.sidechainDb.load() <= -90.0f;
    if (missing != sidechainMissing)
    {
        sidechainMissing = missing;
        repaint (detectorPanel);
    }

    if (colour != stageAccent || title != stageTitle)
        updateStageControls();

    repaint (levelPanel.getUnion (transientPanel).getUnion (stagePanel).getUnion (detectorPanel).withHeight (34));
}

void DynMapMainView::paint (juce::Graphics& g)
{
    const auto& pal = palette();

    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff181d25), 0.0f, 0.0f, pal.background, 0.0f, (float) getHeight(), false));
    g.fillAll();

    // Header.
    g.setColour (pal.text);
    g.setFont (klaud::font (28.0f, true));
    g.drawText ("DynMap", 22, 11, 130, 34, juce::Justification::centredLeft, false);

    g.setColour (pal.accent.withAlpha (0.85f));
    g.setFont (klaud::font (12.5f));
    g.drawText ("dynamic mapping", 142, 24, 160, 18, juce::Justification::centredLeft, false);

    g.setColour (pal.textDim);
    g.setFont (klaud::font (11.0f));
    g.drawText ("Maki plugins", getWidth() - 132, 21, 116, 18, juce::Justification::centredRight, false);

    drawCaption (g, "OVERSAMPLE", { quality.getX() - 90, quality.getY(), 84, quality.getHeight() }, juce::Justification::centredRight);
    drawCaption (g, "BANDS", { phase.getX() - 52, phase.getY(), 46, phase.getHeight() }, juce::Justification::centredRight);

    g.setColour (juce::Colours::white.withAlpha (0.05f));
    g.fillRect (0, headerHeight - 1, getWidth(), 1);

    // Panels for the selected stage.
    drawPanel (g, levelPanel, "Level map  -  " + stageTitle, stageAccent);
    drawPanel (g, transientPanel, "Transient map", stageAccent);
    drawPanel (g, stagePanel, "Gain and saturation", stageAccent);
    drawPanel (g, detectorPanel, "Detector", stageAccent);

    if (sidechainMissing)
    {
        g.setColour (colours::sidechain);
        g.setFont (klaud::font (11.5f, true));
        g.drawFittedText ("No sidechain signal: send the trigger to inputs 3/4 (until then this stage follows its own signal)",
                          detectorPanel.getX() + 14, detectorPanel.getBottom() - 96, detectorPanel.getWidth() - 28, 30,
                          juce::Justification::centredLeft, 2);
    }
    drawPanel (g, globalPanel, "Global");
    drawPanel (g, outputPanel, "Output");

    // Small signal-flow arrows between input, bands and master.
    g.setColour (pal.textDim.withAlpha (0.5f));
    for (auto* c : std::initializer_list<juce::Component*> { &inputTab, &bandDisplay })
    {
        const float x = (float) c->getRight() + 4.0f, y = (float) inputTab.getBounds().getCentreY();
        juce::Path chevron;
        chevron.startNewSubPath (x - 1.5f, y - 4.0f);
        chevron.lineTo (x + 2.0f, y);
        chevron.lineTo (x - 1.5f, y + 4.0f);
        g.strokePath (chevron, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

void DynMapMainView::resized()
{
    // Header.
    presetBar.setBounds (330, 15, 430, 26);
    quality.setBounds (862, 15, 70, 26);
    phase.setBounds (996, 15, 142, 26);

    // Input | bands | master.
    const int rowY = headerHeight + 8, rowH = 222;
    inputTab.setBounds (margin, rowY, 74, rowH - 16 - 62);
    sidechainTab.setBounds (margin, rowY + rowH - 16 - 56, 74, 56);
    masterTab.setBounds (baseWidth - margin - 74, rowY, 74, rowH - 16);
    bandDisplay.setBounds (margin + 74 + 12, rowY, baseWidth - 2 * (margin + 74 + 12), rowH);

    // Lower area.
    const int lowerY = rowY + rowH + 10, lowerH = baseHeight - lowerY - margin;
    int x = margin;
    levelPanel = { x, lowerY, 420, lowerH };           x += 420 + gap;
    transientPanel = { x, lowerY, 330, 236 };
    stagePanel = { x, lowerY + 236 + gap, 330, lowerH - 236 - gap };   x += 330 + gap;
    detectorPanel = { x, lowerY, 300, lowerH };        x += 300 + gap;
    globalPanel = { x, lowerY, baseWidth - margin - x, 232 };
    outputPanel = { x, lowerY + 232 + gap, baseWidth - margin - x, lowerH - 232 - gap };

    // Level map: header controls, then a square editor.
    {
        auto area = levelPanel.reduced (10, 6);
        auto header = area.removeFromTop (26);
        bypass.setBounds (header.removeFromRight (62).reduced (0, 2));
        header.removeFromRight (6);
        mode.setBounds (header.removeFromRight (112).withTrimmedTop (1));
        header.removeFromRight (6);
        linearScale.setBounds (header.removeFromRight (58).reduced (0, 2));
        area.removeFromTop (6);
        levelEditor.setBounds (area.withSizeKeepingCentre (area.getWidth(), juce::jmin (area.getHeight(), area.getWidth())));
    }

    // Transient map.
    {
        auto area = transientPanel.reduced (10, 6);
        area.removeFromTop (30);
        transientEditor.setBounds (area);
    }

    // Gain and saturation.
    {
        auto area = stagePanel.reduced (8, 6);
        auto header = area.removeFromTop (24);
        mute.setBounds (header.removeFromRight (26).reduced (0, 2));
        header.removeFromRight (4);
        solo.setBounds (header.removeFromRight (26).reduced (0, 2));
        area.removeFromTop (2);
        row (area.removeFromTop (knobH), { &pre, &post, &mix, &width }, knobW + 4);
        area.removeFromTop (6);
        auto bottom = area.removeFromTop (knobH);
        auto left = bottom.removeFromLeft (bottom.getWidth() - knobW - 20).reduced (14, 0);
        satType.setBounds (left.removeFromTop (40));
        left.removeFromTop (2);
        satPos.setBounds (left.removeFromTop (40));
        drive.setBounds (bottom.removeFromLeft (knobW + 10));
    }

    // Detector.
    {
        auto area = detectorPanel.reduced (8, 6);
        advanced.setBounds (area.getRight() - 78, area.getY() + 1, 78, 22);
        area.removeFromTop (34);

        if (! advanced.getToggleState())
        {
            // Styles, then the two routing choices that matter without the details.
            auto combos = area.removeFromBottom (42).reduced (6, 0);
            const int w = (combos.getWidth() - 6) / 2;
            stereo.setBounds (combos.removeFromLeft (w));
            combos.removeFromLeft (6);
            scSource.setBounds (combos);
            detectorStyles.setBounds (area.reduced (6, 4));
        }
        else
        {
            const int spacing = juce::jmin (18, (area.getHeight() - 3 * knobH - 42) / 4);
            row (area.removeFromTop (knobH), { &attack, &hold, &release, &relShape }, knobW);
            area.removeFromTop (spacing);
            row (area.removeFromTop (knobH), { &rms, &link, &trTime, &smooth }, knobW);
            area.removeFromTop (spacing);
            row (area.removeFromTop (knobH), { &maxBoost, &maxCut, &scFilter }, knobW);
            area.removeFromTop (spacing);
            auto combos = area.removeFromTop (42).reduced (6, 0);
            const int w = (combos.getWidth() - 12) / 3;
            lookahead.setBounds (combos.removeFromLeft (w));
            combos.removeFromLeft (6);
            stereo.setBounds (combos.removeFromLeft (w));
            combos.removeFromLeft (6);
            scSource.setBounds (combos);
        }
    }

    // Global.
    {
        auto area = globalPanel.reduced (8, 6);
        area.removeFromTop (26);
        auto top = area.removeFromTop (knobH + 6);
        amount.setBounds (top.removeFromLeft (84));
        row (top, { &time, &globalMix }, 60);
        area.removeFromTop (6);
        auto bottom = area.removeFromTop (knobH);
        row (bottom.removeFromLeft (124), { &inGain, &outGain }, 60);
        bottom.removeFromLeft (6);
        auto toggles = bottom.withSizeKeepingCentre (bottom.getWidth(), 56);
        autoGain.setBounds (toggles.removeFromTop (26));
        toggles.removeFromTop (4);
        delta.setBounds (toggles.removeFromTop (26));
    }

    // Output.
    {
        auto area = outputPanel.reduced (8, 6);
        area.removeFromTop (26);
        readout.setBounds (area.removeFromBottom (58).reduced (4, 0));
        meter.setBounds (area.removeFromRight (64));
        area.removeFromRight (6);
        auto top = area.removeFromTop (42);
        clip.setBounds (top.removeFromLeft (top.getWidth() - 70));
        top.removeFromLeft (6);
        limiter.setBounds (top.withTrimmedTop (16));
        area.removeFromTop (4);
        row (area.removeFromTop (knobH), { &ceiling, &limRelease }, 64);
    }
}

//==============================================================================
DynMapEditor::DynMapEditor (DynMapProcessor& p)
    : AudioProcessorEditor (&p), processor (p), view (p, lookAndFeel)
{
    setLookAndFeel (&lookAndFeel);
    addAndMakeVisible (view);

    constexpr int w = DynMapMainView::baseWidth, h = DynMapMainView::baseHeight;
    const int savedWidth = juce::jlimit (w * 6 / 10, w * 2, (int) p.apvts.state.getProperty (uiWidthId, w * 9 / 10));
    setSize (savedWidth, juce::roundToInt (savedWidth * (double) h / (double) w));

    setResizable (true, true);
    getConstrainer()->setFixedAspectRatio ((double) w / (double) h);
    setResizeLimits (w * 6 / 10, h * 6 / 10, w * 2, h * 2);
}

DynMapEditor::~DynMapEditor()
{
    setLookAndFeel (nullptr);
}

void DynMapEditor::resized()
{
    const float scale = (float) getWidth() / (float) DynMapMainView::baseWidth;
    view.setTransform (juce::AffineTransform::scale (scale));
    view.setBounds (0, 0, DynMapMainView::baseWidth, DynMapMainView::baseHeight);

    processor.apvts.state.setProperty (uiWidthId, getWidth(), nullptr);
}
