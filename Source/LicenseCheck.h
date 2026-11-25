#pragma once

#include "Account.h"
#include "Widgets.h"

// "License check" animation shown over the Account window while signing in (or refreshing the plan).
// It follows the real progress of the account: a step only ticks once it has really happened, and each
// step stays on screen for a moment so it can be read. Success fills the shield with the plan's badge;
// a failure turns it red with the reason. Click to close once it's finished.
class LicenseCheck : public juce::Component, private juce::Timer
{
public:
    enum class Mode { SignIn, Refresh };
    std::function<void()> onFinished;

    LicenseCheck (Account& a, Mode m) : account (a), mode (m)
    {
        start = now();
        stepStart = start;
        startTimerHz (60);
    }

    // Snapshot for tests / previews: draws a fixed moment of the animation.
    struct Frame { int stepsDone = 0; bool success = false, failed = false; int tier = 0; double t = 0.0, sinceEnd = 0.0; juce::String error; };
    static void paintFrame (juce::Graphics& g, juce::Rectangle<float> area, const Frame& f);

    void paint (juce::Graphics& g) override
    {
        Frame f;
        f.stepsDone = shownSteps;
        f.success = finished && ! failed;
        f.failed = failed;
        f.tier = tier;
        f.t = now() - start;
        f.sinceEnd = finished ? now() - endTime : 0.0;
        f.error = error;
        paintFrame (g, getLocalBounds().toFloat(), f);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (finished) close();
    }

private:
    static double now() { return juce::Time::getMillisecondCounterHiRes() * 0.001; }
    static constexpr double kMinStep = 0.5;   // each step is shown at least this long

    // How many steps have really happened (0..4).
    int realSteps() const
    {
        const bool busy = account.isBusy(), signedIn = account.isSignedIn();
        if (mode == Mode::SignIn && ! signedIn)
            return now() - start > kMinStep ? 1 : 0;   // the request is out: "connecting" is done
        if (busy)
            return 2;                                   // signed in, plan check still running
        return 4;
    }

    void timerCallback() override
    {
        const double t = now();
        if (! finished)
        {
            // failure: signing in ended without a session
            if (mode == Mode::SignIn && ! account.isBusy() && ! account.isSignedIn() && t - start > 0.6)
            {
                failed = finished = true;
                error = account.getStatus().isNotEmpty() ? account.getStatus() : juce::String ("Couldn't sign in.");
                endTime = t;
            }
            else if (shownSteps < realSteps() && t - stepStart >= kMinStep)
            {
                ++shownSteps;
                stepStart = t;
                if (shownSteps >= 4)
                {
                    finished = true;
                    tier = account.getTier();
                    endTime = t;
                }
            }
        }
        else if (! failed && t - endTime > 2.2)
            close();
        repaint();
    }

    void close()
    {
        stopTimer();
        if (onFinished) juce::MessageManager::callAsync (onFinished);
    }

    Account& account;
    Mode mode;
    double start = 0.0, stepStart = 0.0, endTime = 0.0;
    int shownSteps = 0, tier = 0;
    bool finished = false, failed = false;
    juce::String error;
};

inline void LicenseCheck::paintFrame (juce::Graphics& g, juce::Rectangle<float> area, const Frame& f)
{
    auto clamp01 = [] (double v) { return (float) juce::jlimit (0.0, 1.0, v); };
    auto ease = [] (float v) { return 1.0f - std::pow (1.0f - v, 3.0f); };

    const float fadeIn = ease (clamp01 (f.t / 0.25));
    const float fadeOut = f.success ? clamp01 ((f.sinceEnd - 1.8) / 0.4) : 0.0f;
    const float alpha = fadeIn * (1.0f - fadeOut);

    // dim the account window behind
    g.setColour (Theme::windowBg.withAlpha (0.94f * alpha));
    g.fillRect (area);

    const auto accent = f.failed ? Theme::record : Theme::accent;
    const auto accent2 = f.failed ? Theme::record.darker (0.3f) : Theme::accent2;
    const float size = juce::jmin (area.getWidth() * 0.32f, area.getHeight() * 0.3f);
    // a failure shakes the shield for a moment
    const float shake = f.failed ? 8.0f * (float) std::sin (f.sinceEnd * 45.0) * juce::jmax (0.0f, 1.0f - (float) f.sinceEnd * 2.5f) : 0.0f;
    const auto c = juce::Point<float> (area.getCentreX() + shake, area.getY() + area.getHeight() * 0.3f);

    // shield path
    auto shieldPath = [] (juce::Rectangle<float> r)
    {
        juce::Path p;
        p.startNewSubPath (r.getCentreX(), r.getY());
        p.cubicTo (r.getCentreX() + r.getWidth() * 0.18f, r.getY() + r.getHeight() * 0.1f, r.getRight() - r.getWidth() * 0.05f, r.getY() + r.getHeight() * 0.12f,
                   r.getRight(), r.getY() + r.getHeight() * 0.14f);
        p.cubicTo (r.getRight(), r.getY() + r.getHeight() * 0.6f, r.getCentreX() + r.getWidth() * 0.25f, r.getBottom() - r.getHeight() * 0.1f, r.getCentreX(), r.getBottom());
        p.cubicTo (r.getCentreX() - r.getWidth() * 0.25f, r.getBottom() - r.getHeight() * 0.1f, r.getX(), r.getY() + r.getHeight() * 0.6f,
                   r.getX(), r.getY() + r.getHeight() * 0.14f);
        p.cubicTo (r.getX() + r.getWidth() * 0.05f, r.getY() + r.getHeight() * 0.12f, r.getCentreX() - r.getWidth() * 0.18f, r.getY() + r.getHeight() * 0.1f,
                   r.getCentreX(), r.getY());
        p.closeSubPath();
        return p;
    };
    const auto shieldRect = juce::Rectangle<float> (size * 0.78f, size).withCentre (c);
    const auto shield = shieldPath (shieldRect);

    // spinning arcs around the shield while checking
    if (! f.success && ! f.failed)
    {
        for (int k = 0; k < 2; ++k)
        {
            const float rr = size * (0.72f + 0.1f * k);
            const double a0 = f.t * (k == 0 ? 2.6 : -1.8) + k;
            juce::Path arc;
            arc.addCentredArc (c.x, c.y, rr, rr, 0.0f, (float) a0, (float) a0 + juce::MathConstants<float>::pi * (k == 0 ? 0.8f : 0.5f), true);
            g.setColour ((k == 0 ? accent : accent2).withAlpha (0.8f * alpha));
            g.strokePath (arc, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    }

    // glow
    const float glowAmt = f.success ? ease (clamp01 (f.sinceEnd / 0.4)) : 0.35f + 0.15f * (float) std::sin (f.t * 4.0);
    g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.45f * glowAmt * alpha), c.x, c.y, accent.withAlpha (0.0f), c.x + size, c.y, true));
    g.fillEllipse (juce::Rectangle<float> (size * 2.0f, size * 2.0f).withCentre (c));

    // shield body: glass while checking, filled with the plan colours when done
    {
        const float fill = f.success || f.failed ? ease (clamp01 (f.sinceEnd / 0.45)) : 0.0f;
        g.setColour (juce::Colours::black.withAlpha (0.35f * alpha));
        g.fillPath (shield);
        if (fill > 0.0f)
        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (shield);
            const float h = shieldRect.getHeight() * fill;
            g.setGradientFill (juce::ColourGradient (accent.withAlpha (alpha), shieldRect.getX(), shieldRect.getBottom(),
                                                     accent2.withAlpha (alpha), shieldRect.getRight(), shieldRect.getY(), false));
            g.fillRect (shieldRect.withTop (shieldRect.getBottom() - h));
        }
        g.setColour (accent.withAlpha (0.95f * alpha));
        g.strokePath (shield, juce::PathStrokeType (2.2f));
    }

    // scan line sweeping the shield while checking
    if (! f.success && ! f.failed)
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (shield);
        const float phase = (float) std::fmod (f.t * 0.9, 1.0);
        const float y = shieldRect.getY() + shieldRect.getHeight() * phase;
        g.setGradientFill (juce::ColourGradient (accent2.withAlpha (0.0f), 0.0f, y - 18.0f, accent2.withAlpha (0.0f), 0.0f, y + 2.0f, false));
        juce::ColourGradient band (accent2.withAlpha (0.0f), 0.0f, y - 22.0f, accent2.withAlpha (0.55f * alpha), 0.0f, y, false);
        g.setGradientFill (band);
        g.fillRect (shieldRect.withTop (y - 22.0f).withHeight (22.0f));
        g.setColour (juce::Colours::white.withAlpha (0.8f * alpha));
        g.drawHorizontalLine ((int) y, shieldRect.getX(), shieldRect.getRight());
        // little "data" dots
        for (int i = 0; i < 6; ++i)
        {
            const float px = shieldRect.getX() + shieldRect.getWidth() * (0.15f + 0.14f * i);
            const float py = shieldRect.getY() + shieldRect.getHeight() * (0.3f + 0.08f * (float) std::sin (f.t * 3.0 + i));
            g.setColour (accent.withAlpha (0.5f * alpha));
            g.fillEllipse (px - 2.0f, py - 2.0f, 4.0f, 4.0f);
        }
    }
    else
    {
        // result glyph: a check (with gem / crown for paid plans) or a cross
        const float pop = ease (clamp01 ((f.sinceEnd - 0.2) / 0.3));
        g.setColour (juce::Colours::white.withAlpha (pop * alpha));
        juce::Path mark;
        const auto m = shieldRect.reduced (shieldRect.getWidth() * 0.27f, shieldRect.getHeight() * 0.3f).translated (0.0f, -shieldRect.getHeight() * 0.03f);
        if (f.failed)
        {
            mark.startNewSubPath (m.getTopLeft()); mark.lineTo (m.getBottomRight());
            mark.startNewSubPath (m.getTopRight()); mark.lineTo (m.getBottomLeft());
        }
        else
        {
            mark.startNewSubPath (m.getX(), m.getCentreY());
            mark.lineTo (m.getX() + m.getWidth() * 0.38f, m.getBottom());
            mark.lineTo (m.getRight(), m.getY());
        }
        g.strokePath (mark, juce::PathStrokeType (size * 0.07f * pop, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        // burst ring on success
        if (f.success)
        {
            const float b = clamp01 (f.sinceEnd / 0.7);
            const float rr = size * (0.6f + 0.9f * ease (b));
            g.setColour (accent2.withAlpha ((1.0f - b) * 0.8f * alpha));
            g.drawEllipse (juce::Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre (c), 2.0f);
        }
    }

    // title
    auto text = area.withTop (shieldRect.getBottom() + size * 0.22f).reduced (24.0f, 0.0f);
    g.setColour (Theme::text.withAlpha (alpha));
    g.setFont (juce::FontOptions (19.0f, juce::Font::bold));
    juce::String title = "Verifying your license";
    if (f.failed) title = "License check failed";
    else if (f.success) title = f.tier >= 2 ? "Ultra Pro unlocked" : f.tier == 1 ? "Pro unlocked" : "Signed in - Free plan";
    g.drawText (title, text.removeFromTop (28.0f), juce::Justification::centred);

    if (f.failed)
    {
        g.setColour (Theme::text.withAlpha (0.8f * alpha));
        g.setFont (juce::FontOptions (13.5f));
        g.drawFittedText (f.error, text.removeFromTop (60.0f).toNearestInt(), juce::Justification::centredTop, 3);
        g.setColour (Theme::textDim.withAlpha (alpha));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("Click to go back", text.removeFromTop (22.0f), juce::Justification::centred);
        return;
    }
    if (f.success)
    {
        g.setColour (Theme::textDim.withAlpha (alpha));
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (f.tier >= 1 ? "All your AMETHYST features are ready." : "Upgrade any time from this window.", text.removeFromTop (22.0f), juce::Justification::centred);
    }

    // checklist
    static const char* const steps[] = { "Connecting to AMETHYST servers", "Verifying your account", "Checking your license", "Unlocking your plan" };
    text.removeFromTop (10.0f);
    const float rowH = 24.0f;
    auto list = text.withSizeKeepingCentre (juce::jmin (300.0f, text.getWidth()), rowH * 4.0f).withY (text.getY());
    for (int i = 0; i < 4; ++i)
    {
        auto row = list.removeFromTop (rowH);
        auto icon = row.removeFromLeft (rowH).reduced (5.0f);
        const bool done = i < f.stepsDone, current = i == f.stepsDone && ! f.success;
        if (done)
        {
            g.setColour (Theme::accent.withAlpha (alpha));
            g.fillEllipse (icon);
            juce::Path tick;
            tick.startNewSubPath (icon.getX() + icon.getWidth() * 0.25f, icon.getCentreY());
            tick.lineTo (icon.getX() + icon.getWidth() * 0.45f, icon.getBottom() - icon.getHeight() * 0.27f);
            tick.lineTo (icon.getRight() - icon.getWidth() * 0.22f, icon.getY() + icon.getHeight() * 0.28f);
            g.setColour (juce::Colours::white.withAlpha (alpha));
            g.strokePath (tick, juce::PathStrokeType (1.8f));
        }
        else if (current)
        {
            juce::Path spin;
            const float a0 = (float) (f.t * 6.0);
            spin.addCentredArc (icon.getCentreX(), icon.getCentreY(), icon.getWidth() * 0.5f, icon.getHeight() * 0.5f, 0.0f, a0, a0 + 4.0f, true);
            g.setColour (Theme::accent2.withAlpha (alpha));
            g.strokePath (spin, juce::PathStrokeType (2.0f));
        }
        else
        {
            g.setColour (Theme::panelLight.withAlpha (alpha));
            g.drawEllipse (icon, 1.2f);
        }
        g.setColour ((done ? Theme::text : current ? Theme::text : Theme::textDim).withAlpha ((done || current ? 1.0f : 0.6f) * alpha));
        g.setFont (juce::FontOptions (13.5f, current ? juce::Font::bold : juce::Font::plain));
        g.drawText (steps[i], row.withTrimmedLeft (6.0f), juce::Justification::centredLeft);
    }
}
