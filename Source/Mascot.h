#pragma once

#include <JuceHeader.h>
#include "AmethystArt.h"
#include "Theme.h"

// The anime angel mascot: the alternative app icon / logo ("App icon & logo" in Settings > Appearance),
// the Nightcore Maker's icon, and the little Easter-egg peeks around the app (MascotPeek).
namespace Mascot
{
    // true = every logo mark (toolbar, splash, welcome screen, windows, the window icon) shows her face
    inline bool& iconOn() { static bool on = false; return on; }

    inline juce::Image face()  { return juce::ImageCache::getFromMemory (AmethystArt::mascot_face_png, AmethystArt::mascot_face_pngSize); }
    inline juce::Image icon()  { return juce::ImageCache::getFromMemory (AmethystArt::mascot_icon_png, AmethystArt::mascot_icon_pngSize); }

    // The full figure, scaled once to a working height (the original is 2000 px tall).
    inline juce::Image figure()
    {
        static juce::Image scaled;
        if (! scaled.isValid())
        {
            auto src = juce::ImageCache::getFromMemory (AmethystArt::mascot_png, AmethystArt::mascot_pngSize);
            if (src.isValid())
                scaled = src.rescaled (juce::roundToInt (src.getWidth() * 640.0 / src.getHeight()), 640, juce::Graphics::highResamplingQuality);
        }
        return scaled;
    }

    // Round badge: her face in a circle with a glowing amethyst ring.
    inline void drawBadge (juce::Graphics& g, juce::Rectangle<float> r)
    {
        const float s = juce::jmin (r.getWidth(), r.getHeight());
        auto b = juce::Rectangle<float> (s, s).withCentre (r.getCentre());
        const float ring = juce::jmax (1.5f, s * 0.06f);
        g.setColour (Theme::accent.withAlpha (0.25f));
        g.fillEllipse (b.expanded (ring * 0.8f));
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xfff4ecff), b.getX(), b.getY(), juce::Colour (0xffd9c8ff), b.getRight(), b.getBottom(), false));
        g.fillEllipse (b);
        {
            juce::Graphics::ScopedSaveState keep (g);
            juce::Path clip;
            clip.addEllipse (b);
            g.reduceClipRegion (clip);
            g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
            g.drawImage (face(), b, juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
        }
        g.setGradientFill (juce::ColourGradient (Theme::accent2, b.getX(), b.getY(), juce::Colour (0xff5a3fd8), b.getRight(), b.getBottom(), false));
        g.drawEllipse (b.reduced (ring * 0.5f), ring);
    }

    // App-icon tile version: halo and face filling a rounded tile.
    inline void drawIconTile (juce::Graphics& g, juce::Rectangle<float> r, float corner)
    {
        juce::Graphics::ScopedSaveState keep (g);
        juce::Path clip;
        clip.addRoundedRectangle (r, corner);
        g.reduceClipRegion (clip);
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffe9dcff), r.getX(), r.getY(), juce::Colour (0xffb79cff), r.getRight(), r.getBottom(), false));
        g.fillRect (r);
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.drawImage (icon(), r, juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
    }

    // The badge as an image (menu icons).
    inline juce::Image badgeImage (int size)
    {
        juce::Image img (juce::Image::ARGB, size, size, true);
        juce::Graphics g (img);
        drawBadge (g, juce::Rectangle<float> ((float) size, (float) size).reduced ((float) size * 0.08f));
        return img;
    }

    inline juce::StringArray lines (const juce::String& kind)
    {
        if (kind == "logo")    return { "Hi! You found me!", "Hehe, that tickles!", "Okay, okay, I'm here!", "Shh... I'm guarding your takes." };
        if (kind == "clip")    return { "Whoa, too loud! The master is clipping!", "Eek! Turn it down a little!" };
        if (kind == "export")  return { "Exported! It sounds amazing!", "Another one done! Play it for me?", "Ooh, that's a banger!" };
        if (kind == "late")    return { "Recording this late? Respect.", "Don't forget to sleep... after one more take!" };
        if (kind == "konami")  return { "Cheat code accepted! Infinite talent unlocked!", "Up, up, down, down... you're a legend!" };
        if (kind == "summon")  return { "You called? Let's make a hit!", "Angel reporting for duty!", "Need a hand? I've got a big one!" };
        if (kind == "nightcore") return { "Nightcore time! Faster and cuter!" };
        return { "Hi!" };
    }

    inline juce::String randomLine (const juce::String& kind)
    {
        const auto l = lines (kind);
        return l[juce::Random::getSystemRandom().nextInt (l.size())];
    }
}

// She slides up from the bottom-right corner, says something in a bubble, and slides away again.
// Click her (or the bubble) to send her off early. The parent gives this component its bounds
// (a corner of the window); it lets clicks through everywhere except on her and the bubble.
class MascotPeek : public juce::Component, private juce::Timer
{
public:
    MascotPeek() { setInterceptsMouseClicks (true, false); setAlwaysOnTop (true); }

    void show (const juce::String& line)
    {
        text = line;
        phase = 0.0;
        holding = true;
        setVisible (true);
        toFront (false);
        startTimerHz (60);
    }

    bool isShowing() const { return isVisible(); }
    void jumpToUp() { phase = inTime + 0.5; repaint(); }   // tests: skip the slide-in

    void paint (juce::Graphics& g) override
    {
        const auto img = Mascot::figure();
        if (! img.isValid()) return;
        const auto fig = figureBounds();
        g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        g.setOpacity (1.0f);
        g.drawImage (img, fig, juce::RectanglePlacement::xRight | juce::RectanglePlacement::yTop);

        // speech bubble beside her head, fading in once she's up
        const float a = (float) juce::jlimit (0.0, 1.0, (slide() - 0.7) / 0.3);
        if (a <= 0.0f) return;
        auto bub = bubbleBounds();
        g.setColour (juce::Colours::black.withAlpha (0.3f * a));
        g.fillRoundedRectangle (bub.translated (0.0f, 2.0f), 14.0f);
        g.setColour (juce::Colour (0xfff7f2ff).withAlpha (a));
        g.fillRoundedRectangle (bub, 14.0f);
        juce::Path tail;
        tail.addTriangle (bub.getRight() - 4.0f, bub.getBottom() - 22.0f, bub.getRight() + 14.0f, bub.getBottom() - 4.0f, bub.getRight() - 18.0f, bub.getBottom() - 2.0f);
        g.fillPath (tail);
        g.setColour (Theme::accent.withAlpha (a));
        g.drawRoundedRectangle (bub.reduced (0.5f), 14.0f, 1.5f);
        g.setColour (juce::Colour (0xff2a1550).withAlpha (a));
        g.setFont (juce::FontOptions (14.5f, juce::Font::bold));
        g.drawFittedText (text, bub.reduced (14.0f, 8.0f).toNearestInt(), juce::Justification::centred, 3);
    }

    bool hitTest (int x, int y) override
    {
        const auto p = juce::Point<float> ((float) x, (float) y);
        return figureBounds().reduced (30.0f, 0.0f).contains (p) || (slide() > 0.7 && bubbleBounds().contains (p));
    }

    void mouseDown (const juce::MouseEvent&) override { holding = false; }

private:
    static constexpr double inTime = 0.4, holdTime = 4.2, outTime = 0.4;

    // 0 = hidden below the edge, 1 = fully up
    double slide() const
    {
        if (phase < inTime) { const double t = phase / inTime; return 1.0 - std::pow (1.0 - t, 3.0); }
        if (phase < inTime + holdTime) return 1.0;
        const double t = juce::jmin (1.0, (phase - inTime - holdTime) / outTime);
        return 1.0 - t * t;
    }

    juce::Rectangle<float> figureBounds() const
    {
        const float h = (float) getHeight() * 1.25f;            // taller than the area: only her top part peeks up
        const float w = h * 971.0f / 2000.0f;
        const float y = (float) getHeight() - (float) slide() * (float) getHeight() * 0.92f;
        return { (float) getWidth() - w - 8.0f, y, w, h };
    }

    juce::Rectangle<float> bubbleBounds() const
    {
        const auto fig = figureBounds();
        return { juce::jmax (4.0f, fig.getX() - 200.0f), fig.getY() + 40.0f, 210.0f, 70.0f };
    }

    void timerCallback() override
    {
        phase += 1.0 / 60.0;
        if (! holding && phase < inTime + holdTime && phase >= inTime)
            phase = inTime + holdTime;                     // clicked: go now
        if (phase >= inTime + holdTime + outTime)
        {
            stopTimer();
            setVisible (false);
            return;
        }
        repaint();
    }

    juce::String text;
    double phase = 0.0;
    bool holding = true;
};
