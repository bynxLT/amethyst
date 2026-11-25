#pragma once

#include "Widgets.h"

// Startup animation (about 2.4 s, click to skip): the gem pops in with a glow ring, a light sweep passes
// over it, the wordmark fades in letter by letter with the "2026" pill, a loading line fills while
// sparkles orbit, then everything fades into the app.
class StartupSplash : public juce::Component, private juce::Timer
{
public:
    static constexpr double kDuration = 2.4;
    std::function<void()> onDone;

    StartupSplash()
    {
        setOpaque (false);
        startTime = juce::Time::getMillisecondCounterHiRes();
        startTimerHz (60);
    }

    void paint (juce::Graphics& g) override { paintFrame (g, getLocalBounds().toFloat(), elapsed()); }

    void mouseDown (const juce::MouseEvent&) override
    {
        // skip: jump to the fade-out
        const double t = elapsed();
        if (t < kDuration - 0.45)
            startTime -= (kDuration - 0.45 - t) * 1000.0;
    }

    // Draws the animation at time t (seconds). Static so tests can render frames.
    static void paintFrame (juce::Graphics& g, juce::Rectangle<float> area, double t)
    {
        auto clamp01 = [] (double v) { return (float) juce::jlimit (0.0, 1.0, v); };
        auto ease = [] (float v) { return 1.0f - std::pow (1.0f - v, 3.0f); };
        auto easeBack = [] (float v) { const float c1 = 1.70158f, c3 = c1 + 1.0f; return 1.0f + c3 * std::pow (v - 1.0f, 3.0f) + c1 * std::pow (v - 1.0f, 2.0f); };

        const float fadeOut = clamp01 ((t - (kDuration - 0.55)) / 0.55);
        const float alpha = 1.0f - ease (fadeOut);
        if (alpha <= 0.001f)
            return;

        // background: deep violet with a soft bloom behind the logo
        const auto centre = area.getCentre().translated (0.0f, -area.getHeight() * 0.06f);
        g.setGradientFill (juce::ColourGradient (Theme::bgTop.brighter (0.15f).withAlpha (alpha), centre.x, centre.y,
                                                 Theme::bgBottom.withAlpha (alpha), area.getX(), area.getBottom(), true));
        g.fillRect (area);
        const float bloom = ease (clamp01 (t / 0.9));
        const float bloomR = juce::jmin (area.getWidth(), area.getHeight()) * (0.25f + 0.2f * bloom);
        g.setGradientFill (juce::ColourGradient (Theme::glow1.withAlpha (0.45f * bloom * alpha), centre.x, centre.y,
                                                 Theme::glow1.withAlpha (0.0f), centre.x + bloomR, centre.y, true));
        g.fillEllipse (juce::Rectangle<float> (bloomR * 2.0f, bloomR * 2.0f).withCentre (centre));

        // gem: scales in with a little overshoot, then breathes
        const float size = juce::jmin (area.getWidth(), area.getHeight()) * 0.2f * (1.0f + 0.04f * fadeOut);
        const float pop = easeBack (clamp01 (t / 0.55));
        const float breathe = 1.0f + 0.015f * (float) std::sin (t * 5.0);
        auto gem = juce::Rectangle<float> (size, size).withCentre (centre).withSizeKeepingCentre (size * pop * breathe, size * pop * breathe);

        // expanding glow ring
        const float ring = clamp01 ((t - 0.15) / 0.8);
        if (ring > 0.0f && ring < 1.0f)
        {
            const float rr = size * (0.55f + 1.1f * ease (ring));
            g.setColour (Theme::accent2.withAlpha (0.6f * (1.0f - ring) * alpha));
            g.drawEllipse (juce::Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre (centre), 2.5f * (1.0f - ring) + 0.5f);
        }

        {
            juce::Graphics::ScopedSaveState s (g);
            g.setOpacity (alpha * clamp01 (t / 0.25));
            drawLogoMark (g, gem);
        }

        // light sweep across the gem
        const float sweep = clamp01 ((t - 0.45) / 0.55);
        if (sweep > 0.0f && sweep < 1.0f)
        {
            juce::Graphics::ScopedSaveState s (g);
            juce::Path clip;
            clip.addEllipse (gem.reduced (gem.getWidth() * 0.08f));
            g.reduceClipRegion (clip);
            const float x = gem.getX() - gem.getWidth() * 0.6f + gem.getWidth() * 2.2f * ease (sweep);
            juce::ColourGradient shine (juce::Colours::white.withAlpha (0.0f), x - gem.getWidth() * 0.25f, gem.getY(),
                                        juce::Colours::white.withAlpha (0.0f), x + gem.getWidth() * 0.25f, gem.getY(), false);
            shine.addColour (0.5, juce::Colours::white.withAlpha (0.55f * alpha));
            g.setGradientFill (shine);
            g.fillRect (gem.expanded (gem.getWidth()));
        }

        // sparkles orbiting the gem
        for (int i = 0; i < 9; ++i)
        {
            const float appear = clamp01 ((t - 0.35 - i * 0.07) / 0.4);
            if (appear <= 0.0f) continue;
            const double ang = t * (0.9 + 0.13 * i) + i * 0.7;
            const float orbit = size * (0.85f + 0.12f * (float) (i % 3));
            const auto p = centre + juce::Point<float> ((float) std::cos (ang) * orbit, (float) std::sin (ang) * orbit * 0.55f);
            const float twinkle = 0.5f + 0.5f * (float) std::sin (t * 9.0 + i * 1.7);
            const float r = (1.2f + 1.6f * twinkle) * appear;
            g.setColour ((i % 2 == 0 ? Theme::accent : Theme::accent2).withAlpha (0.85f * appear * alpha));
            g.fillEllipse (p.x - r, p.y - r, r * 2.0f, r * 2.0f);
        }

        // wordmark, letter by letter
        const juce::String word ("AMETHYST");
        const float fontH = juce::jmax (22.0f, size * 0.36f);
        juce::Font font (juce::FontOptions (fontH, juce::Font::bold));
        g.setFont (font);
        const float spacing = fontH * 0.18f;
        float total = 0.0f;
        for (int i = 0; i < word.length(); ++i)
            total += juce::GlyphArrangement::getStringWidth (font, word.substring (i, i + 1)) + (i + 1 < word.length() ? spacing : 0.0f);
        float x = centre.x - total * 0.5f;
        const float baseY = gem.getBottom() + size * 0.42f;
        for (int i = 0; i < word.length(); ++i)
        {
            const auto letter = word.substring (i, i + 1);
            const float w = juce::GlyphArrangement::getStringWidth (font, letter);
            const float a = ease (clamp01 ((t - 0.6 - i * 0.05) / 0.35));
            g.setColour (Theme::text.withAlpha (a * alpha));
            g.drawText (letter, juce::Rectangle<float> (x, baseY - fontH + (1.0f - a) * fontH * 0.4f, w + 2.0f, fontH * 1.2f), juce::Justification::centredLeft, false);
            x += w + spacing;
        }

        // "2026" pill rising under the wordmark
        const float pill = ease (clamp01 ((t - 1.0) / 0.4));
        if (pill > 0.0f)
        {
            g.setFont (juce::FontOptions (fontH * 0.42f, juce::Font::bold));
            auto r = juce::Rectangle<float> (fontH * 1.9f, fontH * 0.62f).withCentre ({ centre.x, baseY + fontH * 0.75f + (1.0f - pill) * 12.0f });
            g.setGradientFill (juce::ColourGradient (Theme::accent.withAlpha (pill * alpha), r.getX(), r.getY(),
                                                     Theme::accent2.withAlpha (pill * alpha), r.getRight(), r.getY(), false));
            g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
            g.setColour (juce::Colours::white.withAlpha (pill * alpha));
            g.drawText ("2026", r, juce::Justification::centred, false);
        }

        // loading line
        const float load = ease (clamp01 ((t - 0.9) / 1.0));
        if (load > 0.0f)
        {
            auto bar = juce::Rectangle<float> (size * 1.6f, 3.0f).withCentre ({ centre.x, baseY + fontH * 1.75f });
            g.setColour (Theme::panelLight.withAlpha (alpha));
            g.fillRoundedRectangle (bar, 1.5f);
            g.setGradientFill (juce::ColourGradient (Theme::accent.withAlpha (alpha), bar.getX(), bar.getY(),
                                                     Theme::accent2.withAlpha (alpha), bar.getRight(), bar.getY(), false));
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * load), 1.5f);
        }
    }

private:
    double elapsed() const { return (juce::Time::getMillisecondCounterHiRes() - startTime) * 0.001; }

    void timerCallback() override
    {
        if (auto* p = getParentComponent(); p != nullptr && p->getIndexOfChildComponent (this) != p->getNumChildComponents() - 1)
            toFront (false);   // stay above the welcome screen while it appears
        repaint();
        if (elapsed() >= kDuration)
        {
            stopTimer();
            if (onDone) juce::MessageManager::callAsync (onDone);
        }
    }

    double startTime = 0.0;
};
