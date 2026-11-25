#pragma once

#include <JuceHeader.h>

// "Amethyst glass" palette: deep violet night, frosted translucent panels, lilac/pink glow.
namespace Theme
{
    // The palette is switchable (colour themes), so these are variables. Read them, don't write them:
    // use applyTheme().
    inline juce::Colour windowBg     { 0xff120a1f };   // opaque, for separate windows
    inline juce::Colour bgTop        { 0xff160b2b };
    inline juce::Colour bgBottom     { 0xff07040e };

    inline juce::Colour background   { 0xff0c0716 };
    inline juce::Colour panel        { 0x12ffffff };   // translucent glass
    inline juce::Colour panelLight   { 0x22ffffff };
    inline juce::Colour lane         { 0x40000000 };
    inline juce::Colour grid         { 0x14ffffff };

    inline juce::Colour clip         { 0xff8f5cff };
    inline juce::Colour wave         { 0xfff3eaff };
    inline juce::Colour record       { 0xffff4f7b };
    inline juce::Colour monitor      { 0xffffad5c };
    inline juce::Colour mute         { 0xff7a8cff };
    inline juce::Colour solo         { 0xffffd65c };
    inline juce::Colour playhead     { 0xffffffff };
    inline juce::Colour text         { 0xffefe8ff };
    inline juce::Colour textDim      { 0x99efe8ff };
    inline juce::Colour accent       { 0xffb47dff };
    inline juce::Colour accent2      { 0xffff70d9 };
    inline juce::Colour ledOn        { 0xffd2a8ff };

    // soft light blobs in the background
    inline juce::Colour glow1 { 0xff7b3fe4 }, glow2 { 0xffd23fe0 }, glow3 { 0xff4a3fe4 }, glow4 { 0xffff70d9 };

    struct ColourTheme
    {
        const char* name;
        int tier;   // 0 free, 1 Pro, 2 Ultra Pro
        juce::uint32 accent, accent2, clip, ledOn, bgTop, bgBottom, windowBg, background, glow1, glow2, glow3;
    };

    inline const ColourTheme kThemes[] = {
        { "Amethyst",    0, 0xffb47dff, 0xffff70d9, 0xff8f5cff, 0xffd2a8ff, 0xff160b2b, 0xff07040e, 0xff120a1f, 0xff0c0716, 0xff7b3fe4, 0xffd23fe0, 0xff4a3fe4 },
        { "Rose Quartz", 1, 0xffff8fb8, 0xffffb38a, 0xffff6f9f, 0xffffc2d6, 0xff2a0b1c, 0xff0e0408, 0xff1f0a14, 0xff160710, 0xffe43f8a, 0xffff8a5c, 0xffb83f7a },
        { "Sapphire",    1, 0xff6fa8ff, 0xff6ff0ff, 0xff4f7dff, 0xffa8c8ff, 0xff0b1530, 0xff04070e, 0xff0a1020, 0xff070c18, 0xff3f6ae4, 0xff3fd0e0, 0xff3f4ae4 },
        { "Emerald",     1, 0xff5fe0a0, 0xffb8ff6f, 0xff2fbf7f, 0xffa8f0cc, 0xff08241a, 0xff030a07, 0xff081a13, 0xff06120d, 0xff2fb87a, 0xff9fe03f, 0xff2f8f9f },
        { "Citrine",     2, 0xffffc65c, 0xffff7a5c, 0xffffad33, 0xffffe0a0, 0xff2a1a06, 0xff0e0802, 0xff1f1408, 0xff160e04, 0xffe49a3f, 0xffff5c3f, 0xffe4c03f },
        { "Obsidian",    2, 0xffd6d6ea, 0xffb47dff, 0xff9a9ab8, 0xffe6e6f5, 0xff121216, 0xff040406, 0xff101014, 0xff0a0a0d, 0xff5a5a70, 0xff7b3fe4, 0xff3a3a48 },
    };
    constexpr int kNumThemes = (int) (sizeof (kThemes) / sizeof (kThemes[0]));

    inline int& currentTheme() { static int i = 0; return i; }

    inline void applyTheme (int index)
    {
        index = juce::jlimit (0, kNumThemes - 1, index);
        const auto& th = kThemes[index];
        currentTheme() = index;
        accent = juce::Colour (th.accent);   accent2 = juce::Colour (th.accent2);
        clip = juce::Colour (th.clip);       ledOn = juce::Colour (th.ledOn);
        bgTop = juce::Colour (th.bgTop);     bgBottom = juce::Colour (th.bgBottom);
        windowBg = juce::Colour (th.windowBg); background = juce::Colour (th.background);
        glow1 = juce::Colour (th.glow1);     glow2 = juce::Colour (th.glow2);   glow3 = juce::Colour (th.glow3);
        glow4 = accent2;
    }
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
        blob (r.getX() + w * 0.12f, r.getY() + h * 0.05f, w * 0.45f, Theme::glow1.withAlpha (0.30f));
        blob (r.getX() + w * 0.88f, r.getY() + h * 0.30f, w * 0.38f, Theme::glow2.withAlpha (0.17f));
        blob (r.getX() + w * 0.45f, r.getY() + h * 1.05f, w * 0.55f, Theme::glow3.withAlpha (0.20f));
        blob (r.getX() + w * 0.62f, r.getY() + h * 0.02f, w * 0.20f, Theme::glow4.withAlpha (0.08f));
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
