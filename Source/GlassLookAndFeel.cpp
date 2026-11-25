#include "GlassLookAndFeel.h"

GlassLookAndFeel::GlassLookAndFeel()
    : juce::LookAndFeel_V4 (juce::LookAndFeel_V4::ColourScheme (
          0xff120a1f,   // windowBackground
          0xff221538,   // widgetBackground
          0xff1a1029,   // menuBackground
          0x33ffffff,   // outline
          0xffefe8ff,   // defaultText
          0xff8f5cff,   // defaultFill
          0xffffffff,   // highlightedText
          0xff6a3fd0,   // highlightedFill
          0xffefe8ff))  // menuText
{
   #if JUCE_WINDOWS
    // Windows 11's variable UI font reads cleaner at small sizes; fall back to Segoe UI.
    const auto names = juce::Font::findAllTypefaceNames();
    setDefaultSansSerifTypefaceName (names.contains ("Segoe UI Variable Text") ? "Segoe UI Variable Text"
                                     : names.contains ("Segoe UI Variable") ? "Segoe UI Variable" : "Segoe UI");
   #elif JUCE_MAC
    setDefaultSansSerifTypefaceName ("SF Pro Text");
   #endif
    refreshColours();
}

void GlassLookAndFeel::refreshColours()
{
    setColour (juce::ResizableWindow::backgroundColourId, Theme::windowBg);
    setColour (juce::DocumentWindow::backgroundColourId, Theme::windowBg);

    setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
    setColour (juce::TextButton::buttonOnColourId, Theme::accent);
    setColour (juce::TextButton::textColourOffId, Theme::text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::white);

    setColour (juce::Slider::rotarySliderFillColourId, Theme::accent);
    setColour (juce::Slider::thumbColourId, Theme::accent);
    setColour (juce::Slider::trackColourId, Theme::accent);
    setColour (juce::Slider::textBoxTextColourId, Theme::text);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, Theme::accent.withAlpha (0.4f));

    setColour (juce::Label::textColourId, Theme::text);
    setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::ListBox::outlineColourId, juce::Colours::transparentBlack);

    setColour (juce::ComboBox::textColourId, Theme::text);
    setColour (juce::ComboBox::arrowColourId, Theme::text);

    setColour (juce::TextEditor::textColourId, Theme::text);
    setColour (juce::TextEditor::highlightColourId, Theme::accent.withAlpha (0.4f));
    setColour (juce::TextEditor::highlightedTextColourId, juce::Colours::white);
    setColour (juce::CaretComponent::caretColourId, Theme::accent);

    setColour (juce::PopupMenu::backgroundColourId, Theme::windowBg.interpolatedWith (Theme::accent, 0.06f));
    setColour (juce::PopupMenu::textColourId, Theme::text);
    setColour (juce::PopupMenu::headerTextColourId, Theme::accent);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, Theme::accent.withAlpha (0.3f));
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);

    setColour (juce::ToggleButton::textColourId, Theme::text);
    setColour (juce::ToggleButton::tickColourId, Theme::accent);

    setColour (juce::BubbleComponent::backgroundColourId, juce::Colour (0xf0221538));
    setColour (juce::BubbleComponent::outlineColourId, Theme::accent.withAlpha (0.6f));
    setColour (juce::TooltipWindow::textColourId, Theme::text);

    setColour (juce::AlertWindow::backgroundColourId, Theme::windowBg);
    setColour (juce::AlertWindow::textColourId, Theme::text);
    setColour (juce::AlertWindow::outlineColourId, Theme::accent.withAlpha (0.4f));
}

juce::Font GlassLookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return juce::FontOptions (juce::jmin (13.5f, (float) buttonHeight * 0.5f), juce::Font::bold);
}

void GlassLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    if (down) r = r.reduced (0.5f).translated (0.0f, 0.5f);   // pressed: sinks slightly
    const float rad = juce::jmin (r.getHeight() * 0.5f, 8.0f);
    const bool primary = (bool) b.getProperties().getWithDefault ("primary", false);
    const bool on = b.getToggleState() || primary;
    const auto onCol = primary ? Theme::accent : b.findColour (juce::TextButton::buttonOnColourId);

    if (over && b.isEnabled())
    {
        g.setColour ((on ? onCol : Theme::accent).withAlpha (0.16f));   // soft hover glow
        g.fillRoundedRectangle (r.expanded (2.5f), rad + 2.5f);
    }

    if (on)
    {
        g.setColour (onCol.withAlpha (0.22f));
        g.fillRoundedRectangle (r.expanded (1.5f), rad + 1.5f);
        g.setGradientFill (juce::ColourGradient (onCol.brighter (over ? 0.35f : 0.2f), r.getX(), r.getY(),
                                                 onCol.darker (0.4f), r.getX(), r.getBottom(), false));
        g.fillRoundedRectangle (r, rad);
    }
    else
    {
        g.setColour (juce::Colours::white.withAlpha (down ? 0.04f : (over ? 0.11f : 0.06f)));
        g.fillRoundedRectangle (r, rad);
    }

    juce::ColourGradient sheen (juce::Colours::white.withAlpha (on ? 0.22f : 0.10f), r.getX(), r.getY(),
                                juce::Colours::white.withAlpha (0.0f), r.getX(), r.getCentreY(), false);
    g.setGradientFill (sheen);
    g.fillRoundedRectangle (r, rad);

    g.setColour (juce::Colours::white.withAlpha (over ? 0.26f : 0.12f));
    g.drawRoundedRectangle (r.reduced (0.5f), rad, 1.0f);

    if (! b.isEnabled())
    {
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (r, rad);
    }
}

void GlassLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool)
{
    auto r = b.getLocalBounds().toFloat();
    const float d = juce::jmin (15.0f, r.getHeight() - 4.0f);
    auto dot = juce::Rectangle<float> (r.getX() + 2.0f, r.getCentreY() - d * 0.5f, d, d);
    const bool on = b.getToggleState();

    if (on)
    {
        g.setColour (Theme::accent.withAlpha (0.35f));
        g.fillEllipse (dot.expanded (3.0f));
        g.setGradientFill (juce::ColourGradient (Theme::accent2, dot.getX(), dot.getY(), Theme::accent, dot.getRight(), dot.getBottom(), false));
        g.fillEllipse (dot);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.fillEllipse (dot.reduced (d * 0.32f));
    }
    else
    {
        g.setColour (juce::Colours::white.withAlpha (over ? 0.14f : 0.07f));
        g.fillEllipse (dot);
        g.setColour (juce::Colours::white.withAlpha (0.28f));
        g.drawEllipse (dot.reduced (0.5f), 1.0f);
    }

    g.setColour (on ? Theme::text : Theme::textDim);
    if (! b.isEnabled())
        g.setColour (Theme::textDim.withAlpha (0.35f));
    g.setFont (juce::FontOptions (13.0f));
    g.drawText (b.getButtonText(), r.withTrimmedLeft (d + 9.0f), juce::Justification::centredLeft, true);
}

void GlassLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                         float startAngle, float endAngle, juce::Slider& s)
{
    const auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (2.0f);
    const float d = juce::jmin (bounds.getWidth(), bounds.getHeight());
    const auto c = bounds.getCentre();
    const auto r = juce::Rectangle<float> (d, d).withCentre (c);
    const float lineW = juce::jmax (2.0f, d * 0.075f);
    const float radius = (d - lineW) * 0.5f;
    const bool enabled = s.isEnabled();

    juce::Path track;
    track.addCentredArc (c.x, c.y, radius, radius, 0.0f, startAngle, endAngle, true);
    g.setColour (juce::Colours::white.withAlpha (0.10f));
    g.strokePath (track, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
    const float zeroPos = bipolar ? (float) s.valueToProportionOfLength (0.0) : 0.0f;
    const float a0 = startAngle + zeroPos * (endAngle - startAngle);
    const float a1 = startAngle + pos * (endAngle - startAngle);

    if (std::abs (a1 - a0) > 0.01f && enabled)
    {
        juce::Path v;
        v.addCentredArc (c.x, c.y, radius, radius, 0.0f, juce::jmin (a0, a1), juce::jmax (a0, a1), true);
        const auto acc = s.findColour (juce::Slider::rotarySliderFillColourId);
        g.setColour (acc.withAlpha (0.22f));
        g.strokePath (v, juce::PathStrokeType (lineW * 2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setGradientFill (juce::ColourGradient (Theme::accent2, r.getX(), r.getBottom(), acc, r.getRight(), r.getY(), false));
        g.strokePath (v, juce::PathStrokeType (lineW, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // glass knob body
    const auto body = r.reduced (lineW * 2.0f);
    g.setGradientFill (juce::ColourGradient (Theme::accent.withMultipliedBrightness (0.38f).withMultipliedSaturation (0.75f), body.getCentreX(), body.getY(),
                                             Theme::windowBg, body.getCentreX(), body.getBottom(), false));
    g.fillEllipse (body);

    auto sheenArea = body.reduced (body.getWidth() * 0.08f).withHeight (body.getHeight() * 0.5f);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.24f), sheenArea.getCentreX(), sheenArea.getY(),
                                             juce::Colours::white.withAlpha (0.0f), sheenArea.getCentreX(), sheenArea.getBottom(), false));
    g.fillEllipse (sheenArea);

    g.setColour (juce::Colours::white.withAlpha (0.18f));
    g.drawEllipse (body.reduced (0.5f), 1.0f);

    const auto p1 = c.getPointOnCircumference (body.getWidth() * 0.14f, a1);
    const auto p2 = c.getPointOnCircumference (body.getWidth() * 0.40f, a1);
    g.setColour (juce::Colours::white.withAlpha (enabled ? 0.92f : 0.3f));
    g.drawLine ({ p1, p2 }, juce::jmax (1.6f, d * 0.055f));
}

void GlassLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float sliderPos, float minPos, float maxPos,
                                         juce::Slider::SliderStyle style, juce::Slider& s)
{
    if (style != juce::Slider::LinearVertical && style != juce::Slider::LinearHorizontal)
    {
        juce::LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, sliderPos, minPos, maxPos, style, s);
        return;
    }

    const bool vertical = style == juce::Slider::LinearVertical;
    const auto b = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
    const float tw = vertical ? juce::jmin (6.0f, b.getWidth() * 0.25f) : juce::jmin (5.0f, b.getHeight() * 0.25f);

    const auto track = vertical ? juce::Rectangle<float> (b.getCentreX() - tw * 0.5f, b.getY(), tw, b.getHeight())
                                : juce::Rectangle<float> (b.getX(), b.getCentreY() - tw * 0.5f, b.getWidth(), tw);
    Glass::well (g, track, tw * 0.5f);

    const bool bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0 && s.getSkewFactor() == 1.0;
    juce::Rectangle<float> fill;
    if (vertical)
        fill = track.withTop (sliderPos);
    else if (bipolar)
    {
        const float zero = b.getX() + (float) s.valueToProportionOfLength (0.0) * b.getWidth();
        fill = juce::Rectangle<float>::leftTopRightBottom (juce::jmin (zero, sliderPos), track.getY(), juce::jmax (zero, sliderPos), track.getBottom());
    }
    else
        fill = track.withRight (sliderPos);

    if (s.isEnabled())
    {
        g.setGradientFill (Glass::accentGradient (fill, vertical));
        g.fillRoundedRectangle (fill, tw * 0.5f);
    }

    // glass thumb
    const auto thumb = vertical ? juce::Rectangle<float> (juce::jmin (b.getWidth() - 2.0f, 36.0f), 16.0f).withCentre ({ b.getCentreX(), sliderPos })
                                : juce::Rectangle<float> (12.0f, juce::jmin (b.getHeight() - 2.0f, 20.0f)).withCentre ({ sliderPos, b.getCentreY() });

    g.setColour (Theme::accent.withAlpha (0.25f));
    g.fillRoundedRectangle (thumb.expanded (2.0f), 6.0f);
    g.setGradientFill (juce::ColourGradient (Theme::accent.withMultipliedBrightness (0.5f).withMultipliedSaturation (0.7f), thumb.getX(), thumb.getY(),
                                             Theme::windowBg.interpolatedWith (Theme::accent, 0.08f), thumb.getX(), thumb.getBottom(), false));
    g.fillRoundedRectangle (thumb, 4.0f);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.30f), thumb.getX(), thumb.getY(),
                                             juce::Colours::white.withAlpha (0.0f), thumb.getX(), thumb.getCentreY(), false));
    g.fillRoundedRectangle (thumb, 4.0f);
    g.setColour (juce::Colours::white.withAlpha (0.35f));
    g.drawRoundedRectangle (thumb.reduced (0.5f), 4.0f, 1.0f);

    g.setColour (Theme::accent2);
    if (vertical)
        g.fillRect (thumb.withSizeKeepingCentre (thumb.getWidth() - 10.0f, 2.0f));
    else
        g.fillRect (thumb.withSizeKeepingCentre (2.0f, thumb.getHeight() - 8.0f));
}

juce::Label* GlassLookAndFeel::createSliderTextBox (juce::Slider& s)
{
    auto* l = juce::LookAndFeel_V4::createSliderTextBox (s);
    l->setFont (juce::FontOptions (12.0f));
    l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
    l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    return l;
}

void GlassLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box)
{
    auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h).reduced (0.5f);
    const float rad = juce::jmin (8.0f, r.getHeight() * 0.5f);

    g.setColour (juce::Colours::white.withAlpha (box.isMouseOver (true) ? 0.11f : 0.065f));
    g.fillRoundedRectangle (r, rad);
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.10f), r.getX(), r.getY(),
                                             juce::Colours::white.withAlpha (0.0f), r.getX(), r.getCentreY(), false));
    g.fillRoundedRectangle (r, rad);
    g.setColour (box.hasKeyboardFocus (true) ? Theme::accent : juce::Colours::white.withAlpha (0.15f));
    g.drawRoundedRectangle (r, rad, 1.0f);

    const float ax = r.getRight() - 14.0f, ay = r.getCentreY();
    juce::Path arrow;
    arrow.startNewSubPath (ax - 4.0f, ay - 2.0f);
    arrow.lineTo (ax, ay + 2.5f);
    arrow.lineTo (ax + 4.0f, ay - 2.0f);
    g.setColour (Theme::text.withAlpha (box.isEnabled() ? 0.8f : 0.3f));
    g.strokePath (arrow, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void GlassLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (8, 1, box.getWidth() - 30, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

void GlassLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
{
    const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h);
    g.setGradientFill (juce::ColourGradient (Theme::windowBg.interpolatedWith (Theme::accent, 0.10f), 0.0f, 0.0f, Theme::windowBg, 0.0f, (float) h, false));
    g.fillRect (r);
    g.setColour (Theme::accent.withAlpha (0.35f));
    g.drawRect (r, 1.0f);
}

void GlassLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                          bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                                          const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColour)
{
    if (isHighlighted && isActive && ! isSeparator)
    {
        auto r = area.toFloat().reduced (3.0f, 1.0f);
        g.setGradientFill (juce::ColourGradient (Theme::accent.withAlpha (0.45f), r.getX(), r.getY(),
                                                 Theme::accent2.withAlpha (0.25f), r.getRight(), r.getY(), false));
        g.fillRoundedRectangle (r, 5.0f);
        juce::LookAndFeel_V4::drawPopupMenuItem (g, area, isSeparator, isActive, false, isTicked, hasSubMenu, text,
                                                 shortcutKeyText, icon, textColour);
        return;
    }

    juce::LookAndFeel_V4::drawPopupMenuItem (g, area, isSeparator, isActive, isHighlighted, isTicked, hasSubMenu, text,
                                             shortcutKeyText, icon, textColour);
}

void GlassLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                                      int thumbStart, int thumbSize, bool over, bool down)
{
    const auto thumb = vertical ? juce::Rectangle<int> (x, thumbStart, w, thumbSize)
                                : juce::Rectangle<int> (thumbStart, y, thumbSize, h);
    const auto t = thumb.toFloat().reduced (vertical ? 3.0f : 1.0f, vertical ? 1.0f : 3.0f);
    g.setColour ((down ? Theme::accent : juce::Colours::white).withAlpha (down ? 0.7f : (over ? 0.35f : 0.18f)));
    g.fillRoundedRectangle (t, juce::jmin (t.getWidth(), t.getHeight()) * 0.5f);
}

void GlassLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor&)
{
    g.setColour (juce::Colours::black.withAlpha (0.3f));
    g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h), 7.0f);
}

void GlassLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& ed)
{
    g.setColour (ed.hasKeyboardFocus (true) ? Theme::accent : juce::Colours::white.withAlpha (0.14f));
    g.drawRoundedRectangle (juce::Rectangle<float> (0.5f, 0.5f, (float) w - 1.0f, (float) h - 1.0f), 7.0f, 1.0f);
}

void GlassLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h)
{
    const auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h);
    g.setColour (juce::Colour (0xf61c1130));
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (Theme::accent.withAlpha (0.5f));
    g.drawRoundedRectangle (r.reduced (0.5f), 6.0f, 1.0f);
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (13.0f));
    g.drawFittedText (text, r.reduced (8.0f, 4.0f).toNearestInt(), juce::Justification::centredLeft, 4);
}

void GlassLookAndFeel::drawBubble (juce::Graphics& g, juce::BubbleComponent&, const juce::Point<float>&, const juce::Rectangle<float>& body)
{
    g.setColour (juce::Colour (0xf61c1130));
    g.fillRoundedRectangle (body, 6.0f);
    g.setColour (Theme::accent.withAlpha (0.6f));
    g.drawRoundedRectangle (body.reduced (0.5f), 6.0f, 1.0f);
}

// Section headers are full-width bands, so menus read as labelled groups (Automation, Value, ...).
void GlassLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName)
{
    auto band = area.toFloat().reduced (2.0f, 2.0f);
    g.setGradientFill (juce::ColourGradient (Theme::accent.withAlpha (0.30f), band.getX(), band.getY(),
                                             Theme::accent.withAlpha (0.12f), band.getRight(), band.getY(), false));
    g.fillRoundedRectangle (band, 3.0f);
    g.setColour (Theme::text.withAlpha (0.92f));
    g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
    g.drawFittedText (sectionName, band.reduced (8.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
}
