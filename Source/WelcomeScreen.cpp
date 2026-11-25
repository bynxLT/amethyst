#include "WelcomeScreen.h"
#include "Mascot.h"
#include "GlassLookAndFeel.h"

namespace
{
    const char* kWhatsNew[] = {
        "Count-in and punch-in recording",
        "Autosave with crash recovery",
        "Automation clips for any knob",
        "New Geode Reverb engine, sidechain and 3-band Prism Dynamics",
        "Smoother noise gate and compressor",
        "Nightcore Maker and master presets (Polish)",
        "Colour themes for Pro and Ultra Pro",
    };

    void drawGlyph (juce::Graphics& g, int kind, juce::Rectangle<float> r, juce::Colour c)
    {
        g.setColour (c);
        juce::Path p;
        switch (kind)
        {
            case 0:   // blank page with a plus
                g.drawRoundedRectangle (r.reduced (r.getWidth() * 0.18f, r.getHeight() * 0.06f), 3.0f, 1.8f);
                g.drawLine (r.getCentreX() - r.getWidth() * 0.14f, r.getCentreY(), r.getCentreX() + r.getWidth() * 0.14f, r.getCentreY(), 1.8f);
                g.drawLine (r.getCentreX(), r.getCentreY() - r.getWidth() * 0.14f, r.getCentreX(), r.getCentreY() + r.getWidth() * 0.14f, 1.8f);
                break;
            case 1:   // microphone
            {
                auto head = r.withSizeKeepingCentre (r.getWidth() * 0.34f, r.getHeight() * 0.5f).translated (0.0f, -r.getHeight() * 0.12f);
                g.fillRoundedRectangle (head, head.getWidth() * 0.5f);
                p.addCentredArc (r.getCentreX(), head.getCentreY() + head.getHeight() * 0.1f, r.getWidth() * 0.27f, r.getHeight() * 0.27f, 0.0f,
                                 juce::MathConstants<float>::pi * 0.55f, juce::MathConstants<float>::pi * 1.45f, true);
                g.strokePath (p, juce::PathStrokeType (1.8f));
                g.drawLine (r.getCentreX(), head.getBottom() + r.getHeight() * 0.13f, r.getCentreX(), r.getBottom() - r.getHeight() * 0.06f, 1.8f);
                break;
            }
            case 2:   // waveform / beat
            {
                const float hs[] = { 0.2f, 0.5f, 0.85f, 0.4f, 0.65f, 0.3f, 0.75f };
                const float w = r.getWidth() / 9.0f;
                for (int i = 0; i < 7; ++i)
                {
                    const float h = r.getHeight() * 0.8f * hs[i];
                    g.fillRoundedRectangle (r.getX() + w * (1.0f + (float) i), r.getCentreY() - h * 0.5f, w * 0.6f, h, w * 0.3f);
                }
                break;
            }
            default:  // FL / import arrow
                g.drawArrow (juce::Line<float> (r.getCentreX(), r.getY() + r.getHeight() * 0.1f, r.getCentreX(), r.getBottom() - r.getHeight() * 0.25f), 1.8f, 10.0f, 8.0f);
                g.drawLine (r.getX() + r.getWidth() * 0.2f, r.getBottom() - r.getHeight() * 0.1f, r.getRight() - r.getWidth() * 0.2f, r.getBottom() - r.getHeight() * 0.1f, 1.8f);
                break;
        }
    }
}

void WelcomeScreen::Tile::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f);
    if (down) r = r.reduced (1.0f);
    g.setColour (Theme::accent.withAlpha (over ? 0.20f : 0.0f));
    g.fillRoundedRectangle (r.expanded (2.0f), 14.0f);
    Glass::panel (g, r, 12.0f, over ? 1.5f : 1.0f);
    auto glyph = r.removeFromTop (r.getHeight() * 0.55f).reduced (r.getWidth() * 0.32f, 10.0f);
    drawGlyph (g, glyphKind, glyph.withSizeKeepingCentre (juce::jmin (glyph.getWidth(), glyph.getHeight()), juce::jmin (glyph.getWidth(), glyph.getHeight())),
               over ? Theme::accent2 : Theme::accent);
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (14.5f, juce::Font::bold));
    g.drawText (title, r.removeFromTop (20.0f).toNearestInt(), juce::Justification::centred);
    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (11.5f));
    g.drawFittedText (subtitle, r.reduced (8.0f, 2.0f).toNearestInt(), juce::Justification::centredTop, 2);
    if (isLocked)
    {
        g.setColour (Theme::accent2.withAlpha (0.85f));
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        g.drawText ("PRO", getLocalBounds().reduced (10, 8), juce::Justification::topRight);
    }
}

void WelcomeScreen::RecentRow::paintButton (juce::Graphics& g, bool over, bool)
{
    auto r = getLocalBounds().toFloat().reduced (0.0f, 1.0f);
    if (over)
    {
        g.setColour (Theme::accent.withAlpha (0.16f));
        g.fillRoundedRectangle (r, 6.0f);
    }
    auto icon = r.removeFromLeft (r.getHeight()).reduced (7.0f);
    g.setColour (file.hasFileExtension ("flp") ? Theme::monitor : Theme::accent);
    g.fillRoundedRectangle (icon, 3.0f);
    g.setColour (Theme::background);
    g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
    g.drawText (file.hasFileExtension ("flp") ? "FL" : "AM", icon.toNearestInt(), juce::Justification::centred);
    r.removeFromLeft (4.0f);
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (13.5f, juce::Font::bold));
    g.drawText (file.getFileNameWithoutExtension(), r.removeFromTop (r.getHeight() * 0.56f).toNearestInt(), juce::Justification::bottomLeft, true);
    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (file.getLastModificationTime().formatted ("%d %b %Y  %H:%M") + "   " + file.getParentDirectory().getFileName(),
                r.toNearestInt(), juce::Justification::topLeft, true);
}

WelcomeScreen::WelcomeScreen (Callbacks c, juce::Array<juce::File> recent, int t, juce::String planName, bool showAtStart)
    : cb (std::move (c)), tier (t), plan (std::move (planName))
{
    setWantsKeyboardFocus (true);

    auto addTile = [this] (const juce::String& title, const juce::String& sub, int glyph, std::function<void()> fn, bool locked = false)
    {
        auto* tl = tiles.add (new Tile (title, sub, glyph, locked));
        tl->onClick = [fn] { if (fn) fn(); };
        addAndMakeVisible (tl);
    };
    addTile ("Empty project", "8 tracks, nothing loaded", 0, cb.newEmpty);
    addTile ("Vocal session", tier >= 1 ? "Beat + vocal tracks with a ready vocal chain" : "Beat + vocal tracks (chain with Pro)", 1, cb.vocalSession, tier < 1);
    addTile ("Mix a beat", "Drop in stems, mastering on the Master", 2, cb.beatSession);
    addTile ("Import FL Studio", "Open an .flp: clips, mixer, effects", 3, cb.importFl);

    for (int i = 0; i < juce::jmin (6, recent.size()); ++i)
    {
        auto* row = recentRows.add (new RecentRow (recent[i]));
        const auto f = recent[i];
        row->onClick = [this, f] { if (cb.openRecent) cb.openRecent (f); };
        addAndMakeVisible (row);
    }

    openBtn.onClick = [this] { if (cb.open) cb.open(); };
    importBtn.onClick = [this] { if (cb.importFl) cb.importFl(); };
    closeBtn.onClick = [this] { if (cb.close) cb.close(); };
    GlassLookAndFeel::makePrimary (closeBtn);
    showToggle.setToggleState (showAtStart, juce::dontSendNotification);
    showToggle.onClick = [this] { if (cb.setShowAtStart) cb.setShowAtStart (showToggle.getToggleState()); };
    for (auto* b : std::initializer_list<juce::Component*> { &openBtn, &importBtn, &closeBtn, &showToggle })
        addAndMakeVisible (b);
    for (auto* ch : getChildren())
        ch->setWantsKeyboardFocus (false);
}

juce::Rectangle<int> WelcomeScreen::card() const
{
    return getLocalBounds().withSizeKeepingCentre (juce::jmin (980, getWidth() - 60), juce::jmin (600, getHeight() - 60));
}

void WelcomeScreen::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black.withAlpha (0.55f));   // dim the studio behind
    const auto c = card().toFloat();
    g.setColour (Theme::windowBg.withAlpha (0.92f));
    g.fillRoundedRectangle (c, 18.0f);
    Glass::panel (g, c, 18.0f, 1.3f);

    auto r = card().reduced (28, 22);
    auto header = r.removeFromTop (60);
    drawLogoMark (g, header.removeFromLeft (52).toFloat().reduced (4.0f));
    header.removeFromLeft (10);
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    g.drawText ("Welcome to " + Mascot::appTitle(), header.removeFromTop (34), juce::Justification::bottomLeft);
    g.setColour (Theme::textDim);
    g.setFont (juce::FontOptions (13.0f));
    g.drawText ("Pick up where you left off, or start something new.", header, juce::Justification::topLeft);

    // plan chip
    {
        const auto chipCol = tier >= 2 ? Theme::accent2 : tier == 1 ? Theme::accent : Theme::textDim;
        auto chip = card().reduced (28, 22).removeFromTop (60).removeFromRight (150).withSizeKeepingCentre (130, 28).toFloat();
        g.setColour (chipCol.withAlpha (0.2f));
        g.fillRoundedRectangle (chip, 14.0f);
        g.setColour (chipCol);
        g.drawRoundedRectangle (chip, 14.0f, 1.2f);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (plan.toUpperCase(), chip.toNearestInt(), juce::Justification::centred);
    }

    r.removeFromTop (18);
    auto cols = r.withTrimmedBottom (50);
    auto left = cols.removeFromLeft (cols.getWidth() * 58 / 100);
    cols.removeFromLeft (24);
    auto recentArea = cols.removeFromTop (cols.getHeight() * 60 / 100);

    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.drawText ("START", left.removeFromTop (22), juce::Justification::centredLeft);
    g.drawText ("RECENT", recentArea.removeFromTop (22), juce::Justification::centredLeft);
    if (recentRows.isEmpty())
    {
        g.setColour (Theme::textDim);
        g.setFont (juce::FontOptions (12.5f));
        g.drawText ("Projects you open or save show up here.", recentArea.removeFromTop (30), juce::Justification::centredLeft);
    }

    cols.removeFromTop (10);
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    g.drawText ("WHAT'S NEW", cols.removeFromTop (22), juce::Justification::centredLeft);
    g.setFont (juce::FontOptions (12.5f));
    for (auto* line : kWhatsNew)
    {
        auto row = cols.removeFromTop (19);
        if (row.getBottom() > cols.getBottom() + 19) break;
        g.setColour (Theme::accent);
        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ (float) row.getX() + 4.0f, (float) row.getCentreY() }));
        g.setColour (Theme::textDim);
        g.drawText (line, row.withTrimmedLeft (14), juce::Justification::centredLeft, true);
    }
}

void WelcomeScreen::resized()
{
    auto r = card().reduced (28, 22);
    r.removeFromTop (60 + 18);
    auto bottom = r.removeFromBottom (36);
    closeBtn.setBounds (bottom.removeFromRight (110));
    bottom.removeFromRight (10);
    showToggle.setBounds (bottom.removeFromRight (150));
    openBtn.setBounds (bottom.removeFromLeft (150));
    bottom.removeFromLeft (8);
    importBtn.setBounds (bottom.removeFromLeft (170));

    r.removeFromBottom (14);
    auto left = r.removeFromLeft (r.getWidth() * 58 / 100);
    r.removeFromLeft (24);
    left.removeFromTop (28);
    const int tw = (left.getWidth() - 12) / 2, th = (left.getHeight() - 12) / 2;
    for (int i = 0; i < tiles.size(); ++i)
        tiles[i]->setBounds (left.getX() + (i % 2) * (tw + 12), left.getY() + (i / 2) * (th + 12), tw, th);

    auto recentArea = r.removeFromTop (r.getHeight() * 60 / 100);
    recentArea.removeFromTop (26);
    for (auto* row : recentRows)
        row->setBounds (recentArea.removeFromTop (40));
}

bool WelcomeScreen::keyPressed (const juce::KeyPress& k)
{
    if (k == juce::KeyPress::escapeKey) { if (cb.close) cb.close(); return true; }
    return false;
}

void WelcomeScreen::mouseDown (const juce::MouseEvent& e)
{
    if (! card().contains (e.getPosition()) && cb.close)
        cb.close();
}
