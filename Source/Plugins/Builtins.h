#pragma once

#include "AmethystPlugin.h"

namespace Builtins
{
    constexpr const char* formatName = "AMETHYST";

    juce::Array<juce::PluginDescription> getDescriptions();
    std::unique_ptr<juce::AudioPluginInstance> create (const juce::String& identifier);

    // Account tier an effect needs: 1 = Pro, 2 = Ultra Pro.
    enum Tier { tierFree = 0, tierPro = 1, tierUltra = 2 };
    inline int requiredTier (const juce::String& identifier) { return identifier == "amethyst.tune" ? tierUltra : tierPro; }
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
}
