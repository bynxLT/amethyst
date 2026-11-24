#pragma once

#include <JuceHeader.h>

// "Amethyst glass" palette: deep violet night, frosted translucent panels, lilac/pink glow.
namespace Theme
{
    const juce::Colour windowBg     { 0xff120a1f };   // opaque, for separate windows
    const juce::Colour bgTop        { 0xff160b2b };
    const juce::Colour bgBottom     { 0xff07040e };

    const juce::Colour background   { 0xff0c0716 };
    const juce::Colour panel        { 0x12ffffff };   // translucent glass
    const juce::Colour panelLight   { 0x22ffffff };
    const juce::Colour lane         { 0x40000000 };
    const juce::Colour grid         { 0x14ffffff };

    const juce::Colour clip         { 0xff8f5cff };
    const juce::Colour wave         { 0xfff3eaff };
    const juce::Colour record       { 0xffff4f7b };
    const juce::Colour monitor      { 0xffffad5c };
    const juce::Colour mute         { 0xff7a8cff };
    const juce::Colour solo         { 0xffffd65c };
    const juce::Colour playhead     { 0xffffffff };
    const juce::Colour text         { 0xffefe8ff };
    const juce::Colour textDim      { 0x99efe8ff };
    const juce::Colour accent       { 0xffb47dff };
    const juce::Colour accent2      { 0xffff70d9 };
    const juce::Colour ledOn        { 0xffd2a8ff };
}

namespace Glass
{
    // Deep purple gradient with soft light blobs. Draw once into a cached image.
    inline void background (juce::Graphics& g, juce::Rectangle<int> area)
    {
        const auto r = area.toFloat();
        g.setGradientFill (juce::ColourGradient (Theme::bgTop, r.getX(), r.getY(),
                                                 Theme::bgBottom, r.getRight(), r.getBottom(), false));
        g.fillRect (r);

        auto blob = [&g] (float cx, float cy, float radius, juce::Colour c)
        {
            juce::ColourGradient gr (c, cx, cy, c.withAlpha (0.0f), cx + radius, cy, true);
            g.setGradientFill (gr);
            g.fillEllipse (cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
        };

        const float w = r.getWidth(), h = r.getHeight();
        blob (r.getX() + w * 0.12f, r.getY() + h * 0.05f, w * 0.45f, juce::Colour (0xff7b3fe4).withAlpha (0.30f));
        blob (r.getX() + w * 0.88f, r.getY() + h * 0.30f, w * 0.38f, juce::Colour (0xffd23fe0).withAlpha (0.17f));
        blob (r.getX() + w * 0.45f, r.getY() + h * 1.05f, w * 0.55f, juce::Colour (0xff4a3fe4).withAlpha (0.20f));
        blob (r.getX() + w * 0.62f, r.getY() + h * 0.02f, w * 0.20f, juce::Colour (0xffff70d9).withAlpha (0.08f));
    }

    // Frosted glass card: translucent fill, top sheen, hairline border and a bright top edge.
    inline void panel (juce::Graphics& g, juce::Rectangle<float> r, float radius = 12.0f, float strength = 1.0f)
    {
        g.setColour (juce::Colours::white.withAlpha (0.045f * strength));
        g.fillRoundedRectangle (r, radius);

        juce::ColourGradient sheen (juce::Colours::white.withAlpha (0.085f * strength), r.getX(), r.getY(),
                                    juce::Colours::white.withAlpha (0.0f), r.getX(), r.getY() + juce::jmin (r.getHeight(), 46.0f), false);
        g.setGradientFill (sheen);
        g.fillRoundedRectangle (r, radius);

        g.setColour (juce::Colours::white.withAlpha (0.11f * strength));
        g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);

        g.setColour (juce::Colours::white.withAlpha (0.20f * strength));
        g.drawLine (r.getX() + radius, r.getY() + 1.0f, r.getRight() - radius, r.getY() + 1.0f, 1.0f);
    }

    // Recessed well (for displays, lanes, meters).
    inline void well (juce::Graphics& g, juce::Rectangle<float> r, float radius = 8.0f)
    {
        g.setColour (juce::Colours::black.withAlpha (0.38f));
        g.fillRoundedRectangle (r, radius);
        juce::ColourGradient shade (juce::Colours::black.withAlpha (0.35f), r.getX(), r.getY(),
                                    juce::Colours::transparentBlack, r.getX(), r.getY() + 10.0f, false);
        g.setGradientFill (shade);
        g.fillRoundedRectangle (r, radius);
        g.setColour (juce::Colours::white.withAlpha (0.07f));
        g.drawRoundedRectangle (r.reduced (0.5f), radius, 1.0f);
    }

    inline void title (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& text)
    {
        g.setColour (Theme::accent.withAlpha (0.9f));
        g.fillRoundedRectangle (juce::Rectangle<float> ((float) r.getX(), (float) r.getCentreY() - 6.0f, 3.0f, 12.0f), 1.5f);
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (13.5f, juce::Font::bold));
        g.drawText (text, r.withTrimmedLeft (10), juce::Justification::centredLeft, true);
    }

    inline juce::ColourGradient accentGradient (juce::Rectangle<float> r, bool vertical)
    {
        return vertical ? juce::ColourGradient (Theme::accent2, r.getX(), r.getY(), Theme::accent, r.getX(), r.getBottom(), false)
                        : juce::ColourGradient (Theme::accent, r.getX(), r.getY(), Theme::accent2, r.getRight(), r.getY(), false);
    }
}

// Vertical peak meter in a glass well.
inline void drawMeter (juce::Graphics& g, juce::Rectangle<float> r, float level)
{
    Glass::well (g, r, juce::jmin (3.0f, r.getWidth() * 0.5f));
    const float db = juce::Decibels::gainToDecibels (level, -60.0f);
    const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
    if (frac <= 0.0f)
        return;

    auto fill = r.reduced (1.0f).withTop (r.getBottom() - 1.0f - (r.getHeight() - 2.0f) * frac);
    juce::ColourGradient grad (juce::Colour (0xff7b6bff), r.getBottomLeft(), juce::Colour (0xffff4f7b), r.getTopLeft(), false);
    grad.addColour (0.75, Theme::accent2);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (fill, juce::jmin (2.0f, fill.getWidth() * 0.5f));
}

// Horizontal version.
inline void drawMeterH (juce::Graphics& g, juce::Rectangle<float> r, float level)
{
    Glass::well (g, r, juce::jmin (3.0f, r.getHeight() * 0.5f));
    const float db = juce::Decibels::gainToDecibels (level, -60.0f);
    const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
    if (frac <= 0.0f)
        return;

    auto fill = r.reduced (1.0f).withWidth ((r.getWidth() - 2.0f) * frac);
    juce::ColourGradient grad (juce::Colour (0xff7b6bff), r.getTopLeft(), juce::Colour (0xffff4f7b), r.getTopRight(), false);
    grad.addColour (0.75, Theme::accent2);
    g.setGradientFill (grad);
    g.fillRoundedRectangle (fill, juce::jmin (2.0f, fill.getHeight() * 0.5f));
}
