#pragma once

#include "Theme.h"

// Slider with a right-click menu: Reset, Copy value, Paste value, Type in value...
class ValueSlider : public juce::Slider
{
public:
    using juce::Slider::Slider;

    // Set by plug-in editors: which parameter this knob controls, so its menu can offer automation.
    juce::AudioProcessor* automationProcessor = nullptr;
    int automationParamIndex = -1;

    // Installed by the main window. Actions: 1 create clip, 2 edit (show) clip, 3 init song with this position, 4 delete clip.
    struct AutomationActions
    {
        std::function<bool (juce::AudioProcessor&, int)> hasClip;
        std::function<void (juce::AudioProcessor&, int, int action)> perform;
    };
    static AutomationActions& automationActions() { static AutomationActions a; return a; }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
        {
            juce::Slider::mouseDown (e);
            return;
        }

        juce::PopupMenu m;
        m.addSectionHeader ((getName().isNotEmpty() ? getName() + ":  " : juce::String()) + getTextFromValue (getValue()));

        auto& acts = automationActions();
        if (automationProcessor != nullptr && automationParamIndex >= 0 && acts.perform != nullptr)
        {
            const bool has = acts.hasClip != nullptr && acts.hasClip (*automationProcessor, automationParamIndex);
            m.addSectionHeader ("Automation");
            m.addItem (10, "Create automation clip", ! has);
            m.addItem (11, "Edit automation clip", has);
            m.addItem (12, "Init song with this position", has);
            m.addItem (13, "Delete automation clip", has);
        }

        m.addSectionHeader ("Value");
        m.addItem (1, "Reset", isDoubleClickReturnEnabled());
        m.addItem (2, "Copy value");
        m.addItem (3, "Paste value", clipboard().has_value());
        m.addItem (4, "Type in value...");
        m.addItem (5, "Randomize");

        juce::Component::SafePointer<ValueSlider> safe (this);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [safe] (int r)
        {
            if (safe == nullptr)
                return;
            auto& s = *safe;
            if (r == 1)      s.setValue (s.getDoubleClickReturnValue(), juce::sendNotificationSync);
            else if (r == 2) clipboard() = s.getValue();
            else if (r == 3 && clipboard().has_value()) s.setValue (juce::jlimit (s.getMinimum(), s.getMaximum(), *clipboard()), juce::sendNotificationSync);
            else if (r == 4) s.showTypeIn();
            else if (r == 5) s.setValue (s.proportionOfLengthToValue (juce::Random::getSystemRandom().nextDouble()), juce::sendNotificationSync);
            else if (r >= 10 && r <= 13 && s.automationProcessor != nullptr && automationActions().perform != nullptr)
                automationActions().perform (*s.automationProcessor, s.automationParamIndex, r - 9);
        });
    }

    void mouseDrag (const juce::MouseEvent& e) override { if (! e.mods.isPopupMenu()) juce::Slider::mouseDrag (e); }
    void mouseUp (const juce::MouseEvent& e) override   { if (! e.mods.isPopupMenu()) juce::Slider::mouseUp (e); }

private:
    static std::optional<double>& clipboard() { static std::optional<double> v; return v; }

    void showTypeIn()
    {
        typeIn = std::make_unique<juce::AlertWindow> ("Type in value", getName(), juce::MessageBoxIconType::NoIcon);
        typeIn->addTextEditor ("v", getTextFromValue (getValue()));
        typeIn->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        typeIn->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<ValueSlider> safe (this);
        typeIn->enterModalState (true, juce::ModalCallbackFunction::create ([safe] (int r)
        {
            if (safe == nullptr || safe->typeIn == nullptr)
                return;
            if (r == 1)
            {
                const auto text = safe->typeIn->getTextEditorContents ("v");
                safe->setValue (juce::jlimit (safe->getMinimum(), safe->getMaximum(), safe->getValueFromText (text)), juce::sendNotificationSync);
            }
            safe->typeIn.reset();
        }), false);
    }

    std::unique_ptr<juce::AlertWindow> typeIn;
};

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

    ValueSlider slider;
    juce::String caption;
};

// Rounded glass button that draws a vector icon.
class IconButton : public juce::Button
{
public:
    enum class Icon { ReturnToStart, Stop, Play, Pause, Record, Metronome, Folder, Reveal, Trash, CountIn, Punch, Gear };

    void setBadge (const juce::String& b) { if (badge != b) { badge = b; repaint(); } }

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

        if (badge.isNotEmpty())
        {
            auto chip = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ r.getRight() - 6.0f, r.getY() + 6.0f });
            g.setColour (glow);
            g.fillEllipse (chip);
            g.setColour (juce::Colours::white);
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText (badge, chip.toNearestInt(), juce::Justification::centred);
        }
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
            case Icon::CountIn:   // four beats, the first one bigger
            {
                const float dot = r.getWidth() * 0.16f;
                for (int k = 0; k < 4; ++k)
                {
                    const float d = k == 0 ? dot * 1.5f : dot;
                    g.fillEllipse (juce::Rectangle<float> (d, d).withCentre ({ r.getX() + r.getWidth() * (0.12f + 0.25f * (float) k), r.getCentreY() }));
                }
                break;
            }
            case Icon::Punch:     // [ o ]  a region with a record dot inside
            {
                const float w = r.getWidth();
                p.startNewSubPath (r.getX() + w * 0.22f, r.getY());
                p.lineTo (r.getX(), r.getY()); p.lineTo (r.getX(), r.getBottom()); p.lineTo (r.getX() + w * 0.22f, r.getBottom());
                p.startNewSubPath (r.getRight() - w * 0.22f, r.getY());
                p.lineTo (r.getRight(), r.getY()); p.lineTo (r.getRight(), r.getBottom()); p.lineTo (r.getRight() - w * 0.22f, r.getBottom());
                g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::mitered, juce::PathStrokeType::square));
                g.fillEllipse (r.withSizeKeepingCentre (w * 0.38f, w * 0.38f));
                break;
            }
            case Icon::Gear:
            {
                const auto c = r.getCentre();
                const float ro = r.getWidth() * 0.5f, ri = r.getWidth() * 0.36f;
                for (int k = 0; k < 8; ++k)
                {
                    const float a0 = (float) k * juce::MathConstants<float>::twoPi / 8.0f;
                    p.addCentredArc (c.x, c.y, ro, ro, 0.0f, a0 - 0.22f, a0 + 0.22f, k == 0);
                    p.addCentredArc (c.x, c.y, ri, ri, 0.0f, a0 + 0.36f, a0 + 0.56f, false);
                }
                p.closeSubPath();
                g.fillPath (p);
                g.setColour (Theme::background);
                g.fillEllipse (r.withSizeKeepingCentre (r.getWidth() * 0.32f, r.getWidth() * 0.32f));
                break;
            }
            case Icon::Trash:
                g.fillRoundedRectangle (r.getX() + r.getWidth() * 0.15f, r.getY() + r.getHeight() * 0.25f, r.getWidth() * 0.7f, r.getHeight() * 0.75f, 2.0f);
                g.fillRoundedRectangle (r.getX(), r.getY() + r.getHeight() * 0.06f, r.getWidth(), r.getHeight() * 0.12f, 1.0f);
                break;
        }
    }

    Icon icon;
    juce::Colour glow;
    juce::String badge;
};

// Magnet (snap) button: horseshoe icon + current snap size; lit when snapping is on.
class MagnetButton : public juce::Button
{
public:
    MagnetButton() : juce::Button ("Snap") {}
    void setLabel (const juce::String& t) { label = t; repaint(); }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const float rad = juce::jmin (9.0f, r.getHeight() * 0.5f);
        const bool on = getToggleState();
        if (on)
        {
            g.setColour (Theme::accent.withAlpha (0.25f));
            g.fillRoundedRectangle (r.expanded (1.5f), rad + 1.5f);
            g.setGradientFill (juce::ColourGradient (Theme::accent.brighter (0.2f), r.getX(), r.getY(), Theme::accent.darker (0.4f), r.getX(), r.getBottom(), false));
        }
        else
            g.setColour (juce::Colours::white.withAlpha (down ? 0.03f : (over ? 0.13f : 0.07f)));
        g.fillRoundedRectangle (r, rad);
        g.setColour (juce::Colours::white.withAlpha (over ? 0.3f : 0.15f));
        g.drawRoundedRectangle (r.reduced (0.5f), rad, 1.0f);

        // horseshoe magnet
        auto icon = r.removeFromLeft (r.getHeight()).reduced (r.getHeight() * 0.22f);
        juce::Path arc;
        arc.addCentredArc (icon.getCentreX(), icon.getCentreY() - icon.getHeight() * 0.05f, icon.getWidth() * 0.38f, icon.getHeight() * 0.38f,
                           0.0f, juce::MathConstants<float>::halfPi, juce::MathConstants<float>::pi * 1.5f, true);
        g.setColour (on ? juce::Colours::white : Theme::text);
        g.strokePath (arc, juce::PathStrokeType (icon.getWidth() * 0.22f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
        g.setColour (on ? Theme::record.brighter (0.4f) : Theme::record);
        const float tipW = icon.getWidth() * 0.22f, tipY = icon.getCentreY() - icon.getHeight() * 0.05f;
        g.fillRect (juce::Rectangle<float> (icon.getCentreX() - icon.getWidth() * 0.38f - tipW * 0.5f, tipY - tipW * 1.4f, tipW, tipW * 1.1f));
        g.fillRect (juce::Rectangle<float> (icon.getCentreX() + icon.getWidth() * 0.38f - tipW * 0.5f, tipY - tipW * 1.4f, tipW, tipW * 1.1f));

        g.setColour (on ? juce::Colours::white : Theme::textDim);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (label, r.toNearestInt().withTrimmedRight (4), juce::Justification::centredLeft, true);
    }

private:
    juce::String label { "Beat" };
};

// Brand mark: a glowing amethyst waveform gem.
inline void drawLogoMark (juce::Graphics& g, juce::Rectangle<float> r)
{
    const float s = juce::jmin (r.getWidth(), r.getHeight());
    const float k = juce::jmax (1.0f, s / 36.0f);   // stroke scale: 1 at toolbar size
    const auto b = juce::Rectangle<float> (s, s).withCentre (r.getCentre());

    juce::Path gem;
    gem.startNewSubPath (b.getCentreX(), b.getY());
    gem.lineTo (b.getRight(), b.getY() + s * 0.36f);
    gem.lineTo (b.getCentreX(), b.getBottom());
    gem.lineTo (b.getX(), b.getY() + s * 0.36f);
    gem.closeSubPath();

    g.setColour (Theme::accent.withAlpha (0.25f));
    g.strokePath (gem, juce::PathStrokeType (5.0f * k));
    g.setGradientFill (juce::ColourGradient (Theme::accent2, b.getX(), b.getY(), juce::Colour (0xff5a3fd8), b.getRight(), b.getBottom(), false));
    g.fillPath (gem);
    g.setColour (juce::Colours::white.withAlpha (0.35f));
    g.strokePath (gem, juce::PathStrokeType (1.0f * k));

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
    g.strokePath (wave, juce::PathStrokeType (1.6f * k, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

// App icon: the gem on a rounded dark-purple glass tile.
inline juce::Image renderAppIcon (int size)
{
    juce::Image img (juce::Image::ARGB, size, size, true);
    juce::Graphics g (img);
    const auto r = juce::Rectangle<float> ((float) size, (float) size).reduced ((float) size * 0.04f);
    const float corner = (float) size * 0.22f;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff2a1550), r.getX(), r.getY(), juce::Colour (0xff0b0616), r.getRight(), r.getBottom(), false));
    g.fillRoundedRectangle (r, corner);
    juce::ColourGradient glow (juce::Colour (0xff9b5cff).withAlpha (0.45f), r.getCentreX(), r.getCentreY(),
                               juce::Colour (0xff9b5cff).withAlpha (0.0f), r.getRight(), r.getBottom(), true);
    g.setGradientFill (glow);
    g.fillRoundedRectangle (r, corner);
    g.setColour (juce::Colours::white.withAlpha (0.18f));
    g.drawRoundedRectangle (r.reduced ((float) size * 0.01f), corner, juce::jmax (1.0f, (float) size * 0.012f));
    drawLogoMark (g, r.reduced ((float) size * 0.17f));
    return img;
}
