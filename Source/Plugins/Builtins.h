#pragma once

#include "AmethystPlugin.h"

namespace Builtins
{
    constexpr const char* formatName = "AMETHYST";

    juce::Array<juce::PluginDescription> getDescriptions();
    std::unique_ptr<juce::AudioPluginInstance> create (const juce::String& identifier);

    // Account tier an effect needs: 1 = Pro, 2 = Ultra Pro.
    enum Tier { tierFree = 0, tierPro = 1, tierUltra = 2 };
    inline int requiredTier (const juce::String& identifier)
    {
        if (identifier == "amethyst.tune" || identifier == "amethyst.denoise" || identifier == "amethyst.harmony") return tierUltra;
        if (identifier == "amethyst.loudness") return tierFree;
        return tierPro;
    }
    inline juce::String tierName (int tier) { return tier >= tierUltra ? "AMETHYST Ultra Pro" : "AMETHYST Pro"; }
    std::unique_ptr<AmethystPlugin> makeTune();

    // factories (one per effect)
    std::unique_ptr<AmethystPlugin> makeEq();
    std::unique_ptr<AmethystPlugin> makeDynamics();
    std::unique_ptr<AmethystPlugin> makeReverb();
    std::unique_ptr<AmethystPlugin> makeEcho();
    std::unique_ptr<AmethystPlugin> makePrism();
    std::unique_ptr<AmethystPlugin> makeShine();
    std::unique_ptr<AmethystPlugin> makeDeEsser();
    std::unique_ptr<AmethystPlugin> makeWarmth();
    std::unique_ptr<AmethystPlugin> makeDoubler();
    std::unique_ptr<AmethystPlugin> makeShift();
    std::unique_ptr<AmethystPlugin> makeTapeStop();
    std::unique_ptr<AmethystPlugin> makePolish();
    juce::StringArray masterStyleNames();   // Polish styles, in order (index = "style" value)
    std::unique_ptr<AmethystPlugin> makeRider();
    std::unique_ptr<AmethystPlugin> makeDenoise();
    std::unique_ptr<AmethystPlugin> makeHarmony();
    std::unique_ptr<AmethystPlugin> makeImager();
    std::unique_ptr<AmethystPlugin> makeTransient();
    std::unique_ptr<AmethystPlugin> makeGate();
    std::unique_ptr<AmethystPlugin> makeAutoFilter();
    std::unique_ptr<AmethystPlugin> makeLoudness();
}
