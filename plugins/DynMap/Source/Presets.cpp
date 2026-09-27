#include "Presets.h"

namespace dynmap
{
namespace
{
    const juce::Identifier presetNameId { "presetName" };
    const juce::StringArray uiProperties { "uiWidth", "selectedStage" };   // kept when presets load
    constexpr const char* fileExtension = ".dynmappreset";

    struct CurveDef
    {
        int stage;
        CurveKind kind;
        int presetIndex = 0;        // Curve::preset index, used when custom is empty
        juce::String custom {};     // "x,y[,segment,tension];..."
    };

    struct FactoryPreset
    {
        juce::String name;
        std::vector<std::pair<juce::String, float>> values;   // real units; bools 0/1, choices by index
        std::vector<CurveDef> curves;
    };

    using Values = std::vector<std::pair<juce::String, float>>;

    std::pair<juce::String, float> sp (int stage, const char* name, float value)
    {
        return { stageParamId (stage, name), value };
    }

    // Turns band slot `slot` (1-11) on with its crossover at `hz`.
    void addBand (Values& v, int slot, float hz, int slope = slope24)
    {
        v.push_back (sp (bandStage (slot), ids::bandOn, 1.0f));
        v.push_back (sp (bandStage (slot), ids::freq, hz));
        v.push_back (sp (bandStage (slot), ids::slope, (float) slope));
    }

    void setTimes (Values& v, int stage, float attack, float release)
    {
        v.push_back (sp (stage, ids::attack, attack));
        v.push_back (sp (stage, ids::release, release));
    }

    const std::vector<FactoryPreset>& factoryPresets()
    {
        static const std::vector<FactoryPreset> presets = []
        {
            std::vector<FactoryPreset> list;
            constexpr auto level = CurveKind::level;
            constexpr auto transient = CurveKind::transient;
            const int b0 = bandStage (0), b1 = bandStage (1), b2 = bandStage (2), b3 = bandStage (3), b4 = bandStage (4);

            list.push_back ({ "Init", {}, {} });

            {
                Values v;
                addBand (v, 1, 88.0f);
                addBand (v, 2, 2500.0f);
                setTimes (v, b0, 47.8f, 282.0f);
                setTimes (v, b1, 22.4f, 282.0f);
                setTimes (v, b2, 13.5f, 132.0f);
                v.push_back ({ ids::globalMix, 70.0f });
                list.push_back ({ "OTT Style", v, { { b0, level, 5 }, { b1, level, 5 }, { b2, level, 5 } } });
            }

            {
                Values v;
                addBand (v, 1, 150.0f);
                addBand (v, 2, 2000.0f);
                addBand (v, 3, 8000.0f);
                for (int s : { b0, b1, b2, b3 })
                    setTimes (v, s, 20.0f, 150.0f);
                v.push_back ({ ids::outGain, 4.0f });
                v.push_back ({ ids::clip, (float) clipSoft });
                v.push_back ({ ids::limiter, 1.0f });
                v.push_back ({ ids::ceiling, -0.3f });
                v.push_back ({ ids::limRel, 60.0f });
                v.push_back ({ ids::quality, 2.0f });
                const juce::String gentle = "-72,-72;-18,-18;12,-4";
                list.push_back ({ "Loud Master", v, { { b0, level, 0, gentle }, { b1, level, 0, gentle },
                                                      { b2, level, 0, gentle }, { b3, level, 0, gentle } } });
            }

            list.push_back ({ "Drum Punch", { sp (inputStage, ids::trTime, 30.0f) }, { { inputStage, transient, 1 } } });

            list.push_back ({ "Drum Snap Extreme",
                              { sp (inputStage, ids::trTime, 20.0f), sp (inputStage, ids::satType, (float) satTape), sp (inputStage, ids::drive, 6.0f) },
                              { { inputStage, transient, 2 } } });

            list.push_back ({ "Transient Softener", { sp (inputStage, ids::trTime, 25.0f) }, { { inputStage, transient, 3 } } });
            list.push_back ({ "Tight Drums", { sp (inputStage, ids::trTime, 60.0f) }, { { inputStage, transient, 4 } } });
            list.push_back ({ "Room Bloom", { sp (inputStage, ids::trTime, 80.0f) }, { { inputStage, transient, 5 } } });

            {
                Values v;
                addBand (v, 1, 200.0f);
                addBand (v, 2, 3000.0f);
                for (int s : { b0, b1, b2 })
                {
                    setTimes (v, s, 30.0f, 400.0f);
                    v.push_back (sp (s, ids::maxBoost, 12.0f));
                }
                list.push_back ({ "Upward Glue", v, { { b0, level, 4 }, { b1, level, 4 }, { b2, level, 4 } } });
            }

            list.push_back ({ "Hard Gate",
                              { sp (inputStage, ids::attack, 0.1f), sp (inputStage, ids::hold, 20.0f), sp (inputStage, ids::release, 40.0f) },
                              { { inputStage, level, 8 } } });

            list.push_back ({ "Stair Steps",
                              { sp (inputStage, ids::attack, 0.5f), sp (inputStage, ids::release, 20.0f) },
                              { { inputStage, level, 10 } } });

            list.push_back ({ "Fold Waveshaper",
                              { sp (inputStage, ids::mode, (float) modeWaveshaper), sp (inputStage, ids::mix, 70.0f), { ids::quality, 3.0f } },
                              { { inputStage, level, 12 } } });

            list.push_back ({ "Soft Clip Waveshaper",
                              { sp (inputStage, ids::mode, (float) modeWaveshaper), { ids::quality, 2.0f } },
                              { { inputStage, level, 11 } } });

            list.push_back ({ "Inverted Dynamics",
                              { sp (inputStage, ids::rms, 20.0f), sp (inputStage, ids::attack, 5.0f), sp (inputStage, ids::release, 100.0f) },
                              { { inputStage, level, 9 } } });

            {
                Values v;
                addBand (v, 1, 120.0f, slope48);
                setTimes (v, b0, 10.0f, 120.0f);
                v.push_back (sp (b0, ids::lookahead, 4.0f));
                list.push_back ({ "Bass Control", v, { { b0, level, 2 } } });
            }

            {
                Values v;
                addBand (v, 1, 6000.0f);
                setTimes (v, b1, 0.5f, 60.0f);
                list.push_back ({ "Smooth De-esser", v, { { b1, level, 0, "-72,-72;-30,-30;12,-19.5" } } });
            }

            list.push_back ({ "Mid/Side Glue",
                              { sp (inputStage, ids::stereo, (float) stereoMS), sp (inputStage, ids::link, 0.0f),
                                sp (inputStage, ids::attack, 30.0f), sp (inputStage, ids::release, 200.0f) },
                              { { inputStage, level, 1 } } });

            list.push_back ({ "Sidechain Pump",
                              { sp (inputStage, ids::scSource, (float) scExternal), sp (inputStage, ids::attack, 1.0f),
                                sp (inputStage, ids::release, 180.0f), sp (inputStage, ids::relShape, 60.0f) },
                              { { inputStage, level, 0, "-72,-72;-30,-30;0,-24;12,-12" } } });

            {
                Values v;
                addBand (v, 1, 100.0f);
                addBand (v, 2, 400.0f);
                addBand (v, 3, 1600.0f);
                addBand (v, 4, 6400.0f);
                setTimes (v, b1, 0.5f, 15.0f);
                setTimes (v, b3, 1.0f, 30.0f);
                v.push_back (sp (b2, ids::mode, (float) modeWaveshaper));
                v.push_back (sp (b4, ids::rms, 15.0f));
                v.push_back ({ ids::quality, 2.0f });
                list.push_back ({ "Multiband Mangle", v,
                                  { { b0, level, 1 }, { b1, level, 10 }, { b2, level, 12 }, { b3, level, 10 }, { b4, level, 9 },
                                    { b4, transient, 2 } } });
            }

            return list;
        }();

        return presets;
    }
}

PresetManager::PresetManager (juce::AudioProcessorValueTreeState& state, CurveBank& bank)
    : apvts (state), curves (bank)
{
    rescanUserPresets();
}

juce::File PresetManager::getUserPresetFolder()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);

   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif

    return base.getChildFile ("Maki plugins").getChildFile ("DynMap").getChildFile ("Presets");
}

void PresetManager::rescanUserPresets()
{
    userPresets = getUserPresetFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + fileExtension);
    userPresets.sort();
}

int PresetManager::getNumFactoryPresets() const
{
    return (int) factoryPresets().size();
}

juce::StringArray PresetManager::getPresetNames() const
{
    juce::StringArray names;

    for (const auto& preset : factoryPresets())
        names.add (preset.name);

    for (const auto& file : userPresets)
        names.add (file.getFileNameWithoutExtension());

    return names;
}

void PresetManager::resetToDefaults()
{
    for (auto* p : apvts.processor.getParameters())
        if (auto* param = dynamic_cast<juce::RangedAudioParameter*> (p))
            param->setValueNotifyingHost (param->getDefaultValue());

    curves.resetAll();
}

void PresetManager::loadPreset (int index)
{
    const int numFactory = getNumFactoryPresets();

    if (juce::isPositiveAndBelow (index, numFactory))
    {
        const auto& preset = factoryPresets()[(size_t) index];
        resetToDefaults();

        for (const auto& [id, value] : preset.values)
            if (auto* param = apvts.getParameter (id))
                param->setValueNotifyingHost (param->convertTo0to1 (value));

        for (const auto& def : preset.curves)
            curves.set (def.stage, def.kind, def.custom.isNotEmpty() ? Curve::fromString (def.kind, def.custom)
                                                                    : Curve::preset (def.kind, def.presetIndex));

        setCurrentPresetName (preset.name);
        return;
    }

    const int userIndex = index - numFactory;

    if (! juce::isPositiveAndBelow (userIndex, userPresets.size()))
        return;

    const auto file = userPresets[userIndex];

    if (auto xml = juce::XmlDocument::parse (file))
    {
        auto tree = juce::ValueTree::fromXml (*xml);

        if (! tree.hasType (apvts.state.getType()))
            return;

        tree.setProperty (presetNameId, file.getFileNameWithoutExtension(), nullptr);

        for (const auto& property : uiProperties)
            if (apvts.state.hasProperty (property))
                tree.setProperty (property, apvts.state.getProperty (property), nullptr);

        replaceState (tree);
    }
}

bool PresetManager::saveUserPreset (const juce::String& name)
{
    const auto cleanName = juce::File::createLegalFileName (name.trim());

    if (cleanName.isEmpty())
        return false;

    const auto folder = getUserPresetFolder();

    if (! folder.createDirectory())
        return false;

    auto state = copyState();
    state.setProperty (presetNameId, cleanName, nullptr);

    for (const auto& property : uiProperties)
        state.removeProperty (property, nullptr);

    const auto xml = state.createXml();

    if (xml == nullptr || ! xml->writeTo (folder.getChildFile (cleanName + fileExtension)))
        return false;

    rescanUserPresets();
    setCurrentPresetName (cleanName);
    return true;
}

juce::ValueTree PresetManager::copyState() const
{
    auto state = apvts.copyState();
    curves.writeTo (state);
    return state;
}

void PresetManager::replaceState (const juce::ValueTree& tree)
{
    apvts.replaceState (tree);
    curves.readFrom (tree);
}

juce::String PresetManager::getCurrentPresetName() const
{
    return apvts.state.getProperty (presetNameId, "Init").toString();
}

int PresetManager::getCurrentPresetIndex() const
{
    return getPresetNames().indexOf (getCurrentPresetName());
}

void PresetManager::setCurrentPresetName (const juce::String& name)
{
    apvts.state.setProperty (presetNameId, name, nullptr);
}
}
