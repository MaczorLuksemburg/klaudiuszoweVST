#include "Presets.h"
#include "Parameters.h"

namespace msc
{
namespace
{
    const juce::Identifier presetNameId { "presetName" };
    const juce::Identifier uiWidthId    { "uiWidth" };
    constexpr const char* fileExtension = ".mscpreset";

    struct FactoryPreset
    {
        juce::String name;
        std::vector<std::pair<juce::String, float>> values;   // real units; bools 0/1, choices by index
    };

    const std::vector<FactoryPreset>& factoryPresets()
    {
        static const std::vector<FactoryPreset> presets {
            { "Init", {} },

            { "Mono Bass, Wide Top",
              { { ids::inOn, 1.0f }, { ids::inShape, 1.0f }, { ids::inCutoff, 180.0f }, { ids::inSlope, 1.0f },
                { ids::inDrySrc, (float) sourceMono },
                { ids::imOn, 1.0f }, { ids::imSide, 140.0f } } },

            { "Mono Maker",
              { { ids::imOn, 1.0f }, { ids::imSide, 0.0f } } },

            { "Haas Widener",
              { { ids::inOn, 1.0f }, { ids::inShape, 1.0f }, { ids::inCutoff, 250.0f }, { ids::inSlope, 1.0f },
                { ids::inDrySrc, (float) sourceMono },
                { ids::hsOn, 1.0f }, { ids::hsRight, 14.0f } } },

            { "Guitar Double",
              { { ids::inOn, 1.0f }, { ids::inShape, 1.0f }, { ids::inCutoff, 120.0f }, { ids::inSlope, 0.0f },
                { ids::inWetSrc, (float) sourceMono }, { ids::inDrySrc, (float) sourceMono },
                { ids::hsOn, 1.0f }, { ids::hsLeft, 0.0f }, { ids::hsRight, 22.0f },
                { ids::imOn, 1.0f }, { ids::imSide, 115.0f } } },

            { "Juno Chorus I",
              { { ids::chOn, 1.0f }, { ids::chMode, 0.0f }, { ids::chMix, 60.0f } } },

            { "Juno Chorus II, Wide",
              { { ids::inOn, 1.0f }, { ids::inShape, 1.0f }, { ids::inCutoff, 200.0f }, { ids::inSlope, 1.0f },
                { ids::inDrySrc, (float) sourceMono },
                { ids::chOn, 1.0f }, { ids::chMode, 1.0f }, { ids::chMix, 70.0f },
                { ids::imOn, 1.0f }, { ids::imSide, 120.0f } } },

            { "Juno Shimmer (I+II)",
              { { ids::chOn, 1.0f }, { ids::chMode, 2.0f }, { ids::chMix, 45.0f }, { ids::chTone, 12000.0f } } },

            { "Bass-Driven Pan",
              { { ids::dpOn, 1.0f }, { ids::dpAmount, 0.5f }, { ids::dpMax, 200.0f },
                { ids::dpShape, 0.0f }, { ids::dpCutoff, 250.0f }, { ids::dpSlope, 1.0f }, { ids::dpClip, (float) clipSoft } } },

            { "Transient Spread",
              { { ids::dpOn, 1.0f }, { ids::dpAmount, 0.35f }, { ids::dpMax, 200.0f },
                { ids::dpShape, 1.0f }, { ids::dpCutoff, 2000.0f }, { ids::dpSlope, 0.0f }, { ids::dpClip, (float) clipSoft } } },

            { "Mix Bus Width",
              { { ids::inOn, 1.0f }, { ids::inShape, 1.0f }, { ids::inCutoff, 120.0f }, { ids::inSlope, 2.0f },
                { ids::inDrySrc, (float) sourceMono },
                { ids::imOn, 1.0f }, { ids::imSide, 125.0f } } },
        };

        return presets;
    }
}

PresetManager::PresetManager (juce::AudioProcessorValueTreeState& state)
    : apvts (state)
{
    rescanUserPresets();
}

juce::File PresetManager::getUserPresetFolder()
{
    auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);

   #if JUCE_MAC
    base = base.getChildFile ("Application Support");
   #endif

    return base.getChildFile ("Maki plugins").getChildFile ("MSC").getChildFile ("Presets");
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

void PresetManager::loadPreset (int index)
{
    const int numFactory = getNumFactoryPresets();

    if (juce::isPositiveAndBelow (index, numFactory))
    {
        const auto& preset = factoryPresets()[(size_t) index];

        for (auto* p : apvts.processor.getParameters())
            if (auto* param = dynamic_cast<juce::RangedAudioParameter*> (p))
                param->setValueNotifyingHost (param->getDefaultValue());

        for (const auto& [id, value] : preset.values)
            if (auto* param = apvts.getParameter (id))
                param->setValueNotifyingHost (param->convertTo0to1 (value));

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

        if (apvts.state.hasProperty (uiWidthId))
            tree.setProperty (uiWidthId, apvts.state.getProperty (uiWidthId), nullptr);

        apvts.replaceState (tree);
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

    auto state = apvts.copyState();
    state.setProperty (presetNameId, cleanName, nullptr);
    state.removeProperty (uiWidthId, nullptr);

    const auto xml = state.createXml();

    if (xml == nullptr || ! xml->writeTo (folder.getChildFile (cleanName + fileExtension)))
        return false;

    rescanUserPresets();
    setCurrentPresetName (cleanName);
    return true;
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
