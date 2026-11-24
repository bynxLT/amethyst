#pragma once

#include "Theme.h"

// Glass knob with a caption underneath and a value bubble while dragging.
class LabeledKnob : public juce::Component
{
public:
    LabeledKnob (const juce::String& captionText, double minV, double maxV, double defaultV,
                 const juce::String& suffix = {}, double interval = 0.0, double skewMidPoint = 0.0)
        : caption (captionText)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        slider.setRange (minV, maxV, interval);
        if (skewMidPoint > minV && skewMidPoint < maxV)
            slider.setSkewFactorFromMidPoint (skewMidPoint);
        slider.setDoubleClickReturnValue (true, defaultV);
        slider.setValue (defaultV, juce::dontSendNotification);
        slider.setTextValueSuffix (suffix);
        slider.setPopupDisplayEnabled (true, true, nullptr, 1200);
        slider.setTooltip (captionText + " (double-click to reset)");
        addAndMakeVisible (slider);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (isEnabled() ? Theme::textDim : Theme::textDim.withAlpha (0.3f));
        g.setFont (juce::FontOptions (10.5f, juce::Font::bold));
        g.drawText (caption.toUpperCase(), getLocalBounds().removeFromBottom (14), juce::Justification::centred, false);
    }

    void resized() override { slider.setBounds (getLocalBounds().withTrimmedBottom (14)); }

    juce::Slider slider;
    juce::String caption;
};

// Rounded glass button that draws a vector icon.
class IconButton : public juce::Button
{
public:
    enum class Icon { ReturnToStart, Stop, Play, Pause, Record, Metronome, Folder, Reveal, Trash };

    explicit IconButton (Icon i, juce::Colour onColour = Theme::accent) : juce::Button ({}), icon (i), glow (onColour) {}

    void setIcon (Icon i) { if (icon != i) { icon = i; repaint(); } }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.5f);
        const float rad = juce::jmin (10.0f, r.getHeight() * 0.35f);
        const bool on = getToggleState();

        if (on)
        {
            g.setColour (glow.withAlpha (0.30f));
            g.fillRoundedRectangle (r.expanded (2.0f), rad + 2.0f);
            g.setGradientFill (juce::ColourGradient (glow.brighter (0.2f), r.getX(), r.getY(), glow.darker (0.4f), r.getX(), r.getBottom(), false));
        }
        else
        {
            g.setColour (juce::Colours::white.withAlpha (down ? 0.03f : (over ? 0.14f : 0.07f)));
        }
        g.fillRoundedRectangle (r, rad);

        g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (on ? 0.28f : 0.13f), r.getX(), r.getY(),
                                                 juce::Colours::white.withAlpha (0.0f), r.getX(), r.getCentreY(), false));
        g.fillRoundedRectangle (r, rad);
        g.setColour (juce::Colours::white.withAlpha (over ? 0.32f : 0.16f));
        g.drawRoundedRectangle (r.reduced (0.5f), rad, 1.0f);

        const float s = juce::jmin (r.getWidth(), r.getHeight()) * 0.42f;
        const auto ic = juce::Rectangle<float> (s, s).withCentre (r.getCentre());
        juce::Colour iconColour = on ? juce::Colours::white : Theme::text;
        if (! on && icon == Icon::Record)
            iconColour = Theme::record;
        if (! isEnabled())
            iconColour = iconColour.withAlpha (0.3f);
        g.setColour (iconColour);
        drawIcon (g, ic);
    }

private:
    void drawIcon (juce::Graphics& g, juce::Rectangle<float> r)
    {
        juce::Path p;
        switch (icon)
        {
            case Icon::Play:
                p.addTriangle (r.getX() + r.getWidth() * 0.12f, r.getY(), r.getX() + r.getWidth() * 0.12f, r.getBottom(), r.getRight(), r.getCentreY());
                g.fillPath (p);
                break;
            case Icon::Pause:
                g.fillRoundedRectangle (r.withWidth (r.getWidth() * 0.34f), 1.5f);
                g.fillRoundedRectangle (r.withTrimmedLeft (r.getWidth() * 0.66f), 1.5f);
                break;
            case Icon::Stop:
                g.fillRoundedRectangle (r.reduced (r.getWidth() * 0.06f), 2.5f);
                break;
            case Icon::Record:
                g.fillEllipse (r.reduced (r.getWidth() * 0.02f));
                break;
            case Icon::ReturnToStart:
                g.fillRoundedRectangle (r.withWidth (r.getWidth() * 0.16f), 1.0f);
                p.addTriangle (r.getRight(), r.getY(), r.getRight(), r.getBottom(), r.getX() + r.getWidth() * 0.22f, r.getCentreY());
                g.fillPath (p);
                break;
            case Icon::Metronome:
            {
                p.startNewSubPath (r.getX() + r.getWidth() * 0.18f, r.getBottom());
                p.lineTo (r.getX() + r.getWidth() * 0.36f, r.getY());
                p.lineTo (r.getX() + r.getWidth() * 0.64f, r.getY());
                p.lineTo (r.getRight() - r.getWidth() * 0.18f, r.getBottom());
                p.closeSubPath();
                g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.drawLine (r.getCentreX(), r.getBottom() - r.getHeight() * 0.15f, r.getRight(), r.getY() + r.getHeight() * 0.1f, 1.6f);
                break;
            }
            case Icon::Folder:
                p.addRoundedRectangle (r.getX(), r.getY() + r.getHeight() * 0.22f, r.getWidth(), r.getHeight() * 0.72f, 2.0f);
                p.addRoundedRectangle (r.getX(), r.getY() + r.getHeight() * 0.08f, r.getWidth() * 0.45f, r.getHeight() * 0.3f, 2.0f);
                g.fillPath (p);
                break;
            case Icon::Reveal:
                g.drawEllipse (r.reduced (r.getWidth() * 0.1f, r.getHeight() * 0.25f), 1.6f);
                g.fillEllipse (r.withSizeKeepingCentre (r.getWidth() * 0.28f, r.getWidth() * 0.28f));
                break;
            case Icon::Trash:
                g.fillRoundedRectangle (r.getX() + r.getWidth() * 0.15f, r.getY() + r.getHeight() * 0.25f, r.getWidth() * 0.7f, r.getHeight() * 0.75f, 2.0f);
                g.fillRoundedRectangle (r.getX(), r.getY() + r.getHeight() * 0.06f, r.getWidth(), r.getHeight() * 0.12f, 1.0f);
                break;
        }
    }

    Icon icon;
    juce::Colour glow;
};

// Brand mark: a glowing amethyst waveform gem.
inline void drawLogoMark (juce::Graphics& g, juce::Rectangle<float> r)
{
    const float s = juce::jmin (r.getWidth(), r.getHeight());
    const auto b = juce::Rectangle<float> (s, s).withCentre (r.getCentre());

    juce::Path gem;
    gem.startNewSubPath (b.getCentreX(), b.getY());
    gem.lineTo (b.getRight(), b.getY() + s * 0.36f);
    gem.lineTo (b.getCentreX(), b.getBottom());
    gem.lineTo (b.getX(), b.getY() + s * 0.36f);
    gem.closeSubPath();

    g.setColour (Theme::accent.withAlpha (0.25f));
    g.strokePath (gem, juce::PathStrokeType (5.0f));
    g.setGradientFill (juce::ColourGradient (Theme::accent2, b.getX(), b.getY(), juce::Colour (0xff5a3fd8), b.getRight(), b.getBottom(), false));
    g.fillPath (gem);
    g.setColour (juce::Colours::white.withAlpha (0.35f));
    g.strokePath (gem, juce::PathStrokeType (1.0f));

    juce::Path wave;
    const float y0 = b.getY() + s * 0.45f;
    wave.startNewSubPath (b.getX() + s * 0.22f, y0);
    const float heights[] = { 0.06f, 0.16f, 0.26f, 0.12f, 0.2f, 0.07f };
    for (int i = 0; i < 6; ++i)
    {
        const float x = b.getX() + s * (0.28f + i * 0.09f);
        wave.lineTo (x, y0 + ((i % 2 == 0) ? -1.0f : 1.0f) * s * heights[i]);
    }
    wave.lineTo (b.getRight() - s * 0.2f, y0);
    g.setColour (juce::Colours::white.withAlpha (0.95f));
    g.strokePath (wave, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}
