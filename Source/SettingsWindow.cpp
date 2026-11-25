#include "SettingsWindow.h"
#include "Mascot.h"
#include "Exporter.h"

namespace SettingsWindow
{
namespace
{
    constexpr int kMargin = 30, kRowH = 50, kNoteH = 40, kPad = 18;

    // A pill switch with a sliding knob.
    class Switch : public juce::ToggleButton, private juce::Timer
    {
    public:
        Switch() { setMouseCursor (juce::MouseCursor::PointingHandCursor); }
        void setInitial (bool on) { setToggleState (on, juce::dontSendNotification); pos = on ? 1.0f : 0.0f; }

        void paintButton (juce::Graphics& g, bool over, bool) override
        {
            const float target = getToggleState() ? 1.0f : 0.0f;
            if (std::abs (pos - target) > 0.001f && ! isTimerRunning()) startTimerHz (60);
            auto r = getLocalBounds().toFloat().withSizeKeepingCentre (42.0f, 24.0f);
            const float rad = r.getHeight() * 0.5f;
            g.setColour (Theme::text.withAlpha (over ? 0.16f : 0.11f));
            g.fillRoundedRectangle (r, rad);
            if (pos > 0.0f)
            {
                g.setGradientFill (Glass::accentGradient (r, false));
                g.setOpacity (pos);
                g.fillRoundedRectangle (r, rad);
                g.setOpacity (1.0f);
            }
            g.setColour (Theme::text.withAlpha (0.12f + 0.1f * pos));
            g.drawRoundedRectangle (r.reduced (0.5f), rad, 1.0f);
            const float d = r.getHeight() - 6.0f;
            auto knob = juce::Rectangle<float> (d, d).withCentre ({ r.getX() + 3.0f + d * 0.5f + pos * (r.getWidth() - d - 6.0f), r.getCentreY() });
            g.setColour (juce::Colours::black.withAlpha (0.3f));
            g.fillEllipse (knob.translated (0.0f, 1.0f));
            g.setColour (juce::Colours::white.withAlpha (0.75f + 0.25f * pos));
            g.fillEllipse (knob);
        }

    private:
        void timerCallback() override
        {
            const float target = getToggleState() ? 1.0f : 0.0f;
            pos += (target - pos) * 0.35f;
            if (std::abs (pos - target) < 0.01f) { pos = target; stopTimer(); }
            repaint();
        }
        float pos = 0.0f;
    };

    // A page: big title, then sections drawn as rounded cards with one setting per row.
    class Page : public juce::Component
    {
    public:
        Page (juce::String t, juce::String sub) : title (std::move (t)), subtitle (std::move (sub)) {}

        void header (const juce::String& text) { rows.push_back ({ Kind::Header, text }); }
        void note (const juce::String& text)   { rows.push_back ({ Kind::Note, text }); }

        void toggle (const juce::String& label, std::function<bool()> get, std::function<void (bool)> set)
        {
            auto* b = new Switch();
            b->setInitial (get());
            b->onClick = [b, set] { set (b->getToggleState()); };
            add (label, b, Kind::Switch);
        }

        void choice (const juce::String& label, const juce::StringArray& options, std::function<int()> get, std::function<void (int)> set)
        {
            auto* c = new juce::ComboBox();
            c->addItemList (options, 1);
            c->setSelectedItemIndex (get(), juce::dontSendNotification);
            c->onChange = [c, set] { set (c->getSelectedItemIndex()); };
            add (label, c, Kind::Combo);
        }

        void slider (const juce::String& label, double lo, double hi, double step, const juce::String& suffix,
                     std::function<double()> get, std::function<void (double)> set)
        {
            auto* s = new juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight);
            s->setRange (lo, hi, step);
            s->setTextValueSuffix (suffix);
            s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 22);
            s->setValue (get(), juce::dontSendNotification);
            s->onValueChange = [s, set] { set (s->getValue()); };
            add (label, s, Kind::Slider);
        }

        void button (const juce::String& label, const juce::String& text, std::function<void()> click)
        {
            auto* b = new juce::TextButton (text);
            b->onClick = std::move (click);
            add (label, b, Kind::Button);
        }

        // a full-width component inside a card (the device selector, the shortcut list)
        void embed (juce::Component* c, int height)
        {
            add ({}, c, Kind::Embed);
            rows.back().height = height;
        }

        int contentHeight() { layout (juce::jmax (getWidth(), 400)); return totalHeight; }

        void paint (juce::Graphics& g) override
        {
            layout (getWidth());
            const int w = getWidth() - 2 * kMargin;
            g.setColour (Theme::text);
            g.setFont (juce::FontOptions (26.0f, juce::Font::bold));
            g.drawText (title, kMargin, 22, w, 34, juce::Justification::centredLeft);
            g.setColour (Theme::textDim);
            g.setFont (juce::FontOptions (13.5f));
            g.drawText (subtitle, kMargin, 56, w, 20, juce::Justification::centredLeft, true);

            for (auto& c : cards)
            {
                auto r = c.toFloat();
                g.setColour (juce::Colours::black.withAlpha (0.18f));
                g.fillRoundedRectangle (r.translated (0.0f, 2.0f), 12.0f);
                g.setGradientFill (juce::ColourGradient (Theme::text.withAlpha (0.075f), r.getX(), r.getY(),
                                                         Theme::text.withAlpha (0.04f), r.getX(), r.getBottom(), false));
                g.fillRoundedRectangle (r, 12.0f);
                g.setColour (Theme::text.withAlpha (0.09f));
                g.drawRoundedRectangle (r.reduced (0.5f), 12.0f, 1.0f);
            }

            for (size_t i = 0; i < rows.size(); ++i)
            {
                auto& row = rows[i];
                const auto r = row.bounds;
                if (row.kind == Kind::Header)
                {
                    g.setColour (Theme::accent);
                    g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
                    g.drawText (row.label.toUpperCase(), r.withTrimmedLeft (4), juce::Justification::centredLeft);
                    continue;
                }
                if (! row.firstInCard)
                {
                    g.setColour (Theme::text.withAlpha (0.07f));
                    g.fillRect (r.getX() + kPad, r.getY(), r.getWidth() - 2 * kPad, 1);
                }
                if (row.kind == Kind::Note)
                {
                    g.setColour (Theme::textDim);
                    g.setFont (juce::FontOptions (12.5f));
                    g.drawFittedText (row.label, r.reduced (kPad, 4), juce::Justification::centredLeft, 2);
                }
                else if (row.kind != Kind::Embed)
                {
                    const int right = row.control != nullptr ? row.control->getX() - 14 : r.getRight() - kPad;
                    g.setColour (Theme::text);
                    g.setFont (juce::FontOptions (14.5f));
                    g.drawFittedText (row.label, r.getX() + kPad, r.getY(), right - r.getX() - kPad, r.getHeight(),
                                      juce::Justification::centredLeft, 2, 0.9f);
                }
            }
        }

        void resized() override
        {
            layout (getWidth());
            for (auto& row : rows)
            {
                if (row.control == nullptr) continue;
                auto r = row.bounds.reduced (kPad, 0);
                const int cy = r.getCentreY();
                switch (row.kind)
                {
                    case Kind::Switch: row.control->setBounds (r.getRight() - 46, cy - 14, 46, 28); break;
                    case Kind::Combo:  row.control->setBounds (r.getRight() - juce::jmin (270, r.getWidth() / 2), cy - 15, juce::jmin (270, r.getWidth() / 2), 30); break;
                    case Kind::Slider: row.control->setBounds (r.getRight() - juce::jmin (300, r.getWidth() / 2), cy - 13, juce::jmin (300, r.getWidth() / 2), 26); break;
                    case Kind::Button:
                    {
                        auto* b = static_cast<juce::TextButton*> (row.control.get());
                        const int bw = juce::jmax (130, juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (14.0f), b->getButtonText()) + 40);
                        b->setBounds (r.getRight() - bw, cy - 15, bw, 30);
                        break;
                    }
                    case Kind::Embed:  row.control->setBounds (row.bounds.reduced (10, 10)); break;
                    default: break;
                }
            }
        }

    private:
        enum class Kind { Header, Note, Switch, Combo, Slider, Button, Embed };
        struct Row
        {
            Kind kind;
            juce::String label;
            std::unique_ptr<juce::Component> control;
            int height = 0;
            juce::Rectangle<int> bounds;
            bool firstInCard = false;
        };

        void add (const juce::String& label, juce::Component* c, Kind k)
        {
            addAndMakeVisible (c);
            rows.push_back ({ k, label, std::unique_ptr<juce::Component> (c) });
        }

        void layout (int width)
        {
            cards.clear();
            const int w = width - 2 * kMargin;
            int y = 96, cardTop = -1;
            bool first = true;
            auto closeCard = [&]
            {
                if (cardTop >= 0) cards.add ({ kMargin, cardTop, w, y - cardTop });
                cardTop = -1;
            };
            for (auto& row : rows)
            {
                if (row.kind == Kind::Header)
                {
                    closeCard();
                    if (! first) y += 22;
                    row.bounds = { kMargin, y, w, 20 };
                    y += 28;
                    first = false;
                    continue;
                }
                row.firstInCard = cardTop < 0;
                if (cardTop < 0) cardTop = y;
                first = false;
                const int h = row.kind == Kind::Embed ? row.height : row.kind == Kind::Note ? kNoteH : kRowH;
                row.bounds = { kMargin, y, w, h };
                y += h;
            }
            closeCard();
            totalHeight = y + 30;
        }

        juce::String title, subtitle;
        std::vector<Row> rows;
        juce::Array<juce::Rectangle<int>> cards;
        int totalHeight = 200;
    };

    // Viewport wrapper so long pages scroll.
    class Scroller : public juce::Viewport
    {
    public:
        explicit Scroller (Page* p) : page (p) { setViewedComponent (p, true); setScrollBarsShown (true, false); setScrollBarThickness (8); }
        void resized() override
        {
            juce::Viewport::resized();
            page->setSize (getMaximumVisibleWidth(), 10);
            page->setSize (getMaximumVisibleWidth(), juce::jmax (getMaximumVisibleHeight(), page->contentHeight()));
        }
    private:
        Page* page;
    };

    // Keyboard shortcuts as key badges and names, from lines of "<keys padded to 16><name>".
    class ShortcutList : public juce::Component
    {
    public:
        explicit ShortcutList (const juce::String& text)
        {
            for (auto& line : juce::StringArray::fromLines (text))
                if (line.length() > 16 && line.substring (0, 16).trim().isNotEmpty())
                    items.push_back ({ line.substring (0, 16).trim(), line.substring (16).trim() });
        }
        static constexpr int rowH = 36;
        int idealHeight() const { return (int) items.size() * rowH + 12; }

        void paint (juce::Graphics& g) override
        {
            const juce::FontOptions keyFont (12.5f, juce::Font::bold), nameFont (14.0f);
            for (size_t i = 0; i < items.size(); ++i)
            {
                auto r = juce::Rectangle<int> (0, 6 + (int) i * rowH, getWidth(), rowH);
                if (i > 0)
                {
                    g.setColour (Theme::text.withAlpha (0.06f));
                    g.fillRect (r.getX() + 8, r.getY(), r.getWidth() - 16, 1);
                }
                // one badge per key in a combination (Ctrl + Shift + S)
                float x = 10.0f;
                juce::StringArray keys;
                keys.addTokens (items[i].keys, "+", {});
                if (items[i].keys == "+") keys = { "+" };
                for (int k = 0; k < keys.size(); ++k)
                {
                    const auto key = keys[k].trim();
                    const float w = juce::jmax (26.0f, (float) juce::GlyphArrangement::getStringWidthInt (keyFont, key) + 16.0f);
                    auto chip = juce::Rectangle<float> (x, (float) r.getCentreY() - 12.0f, w, 24.0f);
                    g.setColour (juce::Colours::black.withAlpha (0.35f));
                    g.fillRoundedRectangle (chip.translated (0.0f, 1.5f), 6.0f);
                    g.setColour (Theme::text.withAlpha (0.1f));
                    g.fillRoundedRectangle (chip, 6.0f);
                    g.setColour (Theme::text.withAlpha (0.16f));
                    g.drawRoundedRectangle (chip.reduced (0.5f), 6.0f, 1.0f);
                    g.setColour (Theme::text);
                    g.setFont (keyFont);
                    g.drawText (key, chip, juce::Justification::centred);
                    x += w + 4.0f;
                    if (k < keys.size() - 1)
                    {
                        g.setColour (Theme::textDim);
                        g.drawText ("+", juce::Rectangle<float> (x, chip.getY(), 10.0f, chip.getHeight()), juce::Justification::centred);
                        x += 14.0f;
                    }
                }
                g.setColour (Theme::text.withAlpha (0.88f));
                g.setFont (nameFont);
                g.drawText (items[i].name, r.withTrimmedLeft (190).withTrimmedRight (10), juce::Justification::centredLeft, true);
            }
        }
    private:
        struct Item { juce::String keys, name; };
        std::vector<Item> items;
    };

    // Left-hand navigation with drawn icons.
    class Sidebar : public juce::Component
    {
    public:
        std::function<void (int)> onSelect;
        juce::StringArray items;
        int selected = 0;

        void paint (juce::Graphics& g) override
        {
            auto b = getLocalBounds().toFloat();
            g.setColour (juce::Colours::black.withAlpha (0.22f));
            g.fillRect (b);
            g.setColour (Theme::text.withAlpha (0.07f));
            g.fillRect (b.removeFromRight (1.0f));

            // brand
            auto gem = juce::Rectangle<float> (22.0f, 26.0f, 22.0f, 22.0f);
            juce::Path p;
            p.startNewSubPath (gem.getCentreX(), gem.getY());
            p.lineTo (gem.getRight(), gem.getY() + gem.getHeight() * 0.38f);
            p.lineTo (gem.getCentreX(), gem.getBottom());
            p.lineTo (gem.getX(), gem.getY() + gem.getHeight() * 0.38f);
            p.closeSubPath();
            g.setGradientFill (Glass::accentGradient (gem, true));
            g.fillPath (p);
            g.setColour (Theme::text);
            g.setFont (juce::FontOptions (19.0f, juce::Font::bold));
            g.drawText ("Settings", 54, 24, getWidth() - 60, 26, juce::Justification::centredLeft);

            for (int i = 0; i < items.size(); ++i)
            {
                auto r = itemBounds (i).toFloat();
                const bool sel = i == selected, hov = i == hover;
                if (sel)
                {
                    g.setColour (Theme::accent.withAlpha (0.17f));
                    g.fillRoundedRectangle (r, 9.0f);
                    g.setGradientFill (Glass::accentGradient (r, true));
                    g.fillRoundedRectangle (r.withWidth (3.0f).reduced (0.0f, 9.0f), 1.5f);
                }
                else if (hov)
                {
                    g.setColour (Theme::text.withAlpha (0.06f));
                    g.fillRoundedRectangle (r, 9.0f);
                }
                const auto c = sel ? Theme::text : Theme::text.withAlpha (hov ? 0.85f : 0.62f);
                drawIcon (g, i, r.withX (r.getX() + 14.0f).withWidth (18.0f).withSizeKeepingCentre (18.0f, 18.0f), sel ? Theme::accent.brighter (0.3f) : c);
                g.setColour (c);
                g.setFont (juce::FontOptions (14.5f, sel ? juce::Font::bold : juce::Font::plain));
                g.drawText (items[i], r.withTrimmedLeft (44.0f), juce::Justification::centredLeft);
            }

            g.setColour (Theme::textDim.withAlpha (0.45f));
            g.setFont (juce::FontOptions (11.5f));
            g.drawText ((Mascot::iconOn() ? "Anime Edition  v" : "AMETHYST 2026  v") + juce::String (ProjectInfo::versionString), 22, getHeight() - 34, getWidth() - 30, 18, juce::Justification::centredLeft);
        }

        void mouseMove (const juce::MouseEvent& e) override { setHover (indexAt (e.getPosition())); }
        void mouseExit (const juce::MouseEvent&) override   { setHover (-1); }
        void mouseDown (const juce::MouseEvent& e) override
        {
            const int i = indexAt (e.getPosition());
            if (i >= 0 && i != selected) { selected = i; repaint(); if (onSelect) onSelect (i); }
        }

    private:
        int hover = -1;
        juce::Rectangle<int> itemBounds (int i) const { return { 12, 76 + i * 44, getWidth() - 24, 38 }; }
        int indexAt (juce::Point<int> p) const
        {
            for (int i = 0; i < items.size(); ++i) if (itemBounds (i).contains (p)) return i;
            return -1;
        }
        void setHover (int i)
        {
            if (i == hover) return;
            hover = i;
            setMouseCursor (i >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
            repaint();
        }

        static void drawIcon (juce::Graphics& g, int i, juce::Rectangle<float> r, juce::Colour c)
        {
            juce::Path p;
            const float x = r.getX(), y = r.getY(), s = r.getWidth();
            switch (i)
            {
                case 0:   // speaker
                    p.startNewSubPath (x, y + s * 0.36f); p.lineTo (x + s * 0.25f, y + s * 0.36f); p.lineTo (x + s * 0.52f, y + s * 0.1f);
                    p.lineTo (x + s * 0.52f, y + s * 0.9f); p.lineTo (x + s * 0.25f, y + s * 0.64f); p.lineTo (x, y + s * 0.64f); p.closeSubPath();
                    p.addCentredArc (x + s * 0.55f, y + s * 0.5f, s * 0.22f, s * 0.22f, 0.0f, 0.6f, 2.55f, true);
                    p.addCentredArc (x + s * 0.55f, y + s * 0.5f, s * 0.42f, s * 0.42f, 0.0f, 0.7f, 2.45f, true);
                    break;
                case 1:   // microphone
                    p.addRoundedRectangle (x + s * 0.33f, y, s * 0.34f, s * 0.6f, s * 0.17f);
                    p.addCentredArc (x + s * 0.5f, y + s * 0.42f, s * 0.3f, s * 0.3f, 0.0f, juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi * 3.0f, true);
                    p.startNewSubPath (x + s * 0.5f, y + s * 0.72f); p.lineTo (x + s * 0.5f, y + s * 0.95f);
                    p.startNewSubPath (x + s * 0.3f, y + s * 0.95f); p.lineTo (x + s * 0.7f, y + s * 0.95f);
                    break;
                case 2:   // palette
                    p.addEllipse (r.reduced (1.0f));
                    p.addEllipse (x + s * 0.27f, y + s * 0.3f, s * 0.12f, s * 0.12f);
                    p.addEllipse (x + s * 0.47f, y + s * 0.2f, s * 0.12f, s * 0.12f);
                    p.addEllipse (x + s * 0.62f, y + s * 0.4f, s * 0.12f, s * 0.12f);
                    break;
                case 3:   // folder
                    p.startNewSubPath (x, y + s * 0.18f); p.lineTo (x + s * 0.38f, y + s * 0.18f); p.lineTo (x + s * 0.48f, y + s * 0.3f);
                    p.lineTo (x + s, y + s * 0.3f); p.lineTo (x + s, y + s * 0.86f); p.lineTo (x, y + s * 0.86f); p.closeSubPath();
                    break;
                case 4:   // globe
                    p.addEllipse (r.reduced (1.0f));
                    p.addEllipse (x + s * 0.3f, y + 1.0f, s * 0.4f, s - 2.0f);
                    p.startNewSubPath (x + 1.0f, y + s * 0.5f); p.lineTo (x + s - 1.0f, y + s * 0.5f);
                    break;
                default:  // keyboard
                    p.addRoundedRectangle (x, y + s * 0.2f, s, s * 0.6f, 2.5f);
                    for (int k = 0; k < 4; ++k) p.addRectangle (x + s * (0.15f + 0.2f * (float) k), y + s * 0.35f, s * 0.1f, s * 0.08f);
                    p.startNewSubPath (x + s * 0.28f, y + s * 0.6f); p.lineTo (x + s * 0.72f, y + s * 0.6f);
                    break;
            }
            g.setColour (c);
            g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
    };

    class Window : public juce::Component
    {
    public:
        Window (Host h, int initialTab) : host (std::move (h))
        {
            auto& e = *host.engine;
            auto* st = host.props->getUserSettings();
            const int tier = host.tier ? host.tier() : 2;
            auto boolSetting = [st] (const char* key, bool def) { return [st, key, def] { return st->getBoolValue (key, def); }; };
            auto saveBool = [st] (const char* key) { return [st, key] (bool v) { st->setValue (key, v); }; };

            // ---- Audio
            {
                auto* p = new Page ("Audio", "Your audio interface, inputs and outputs.");
                p->header ("Device");
                p->embed (new juce::AudioDeviceSelectorComponent (e.deviceManager, 0, 16, 2, 2, false, false, false, false), 380);
                addPage ("Audio", p);
            }

            // ---- Recording
            {
                auto* p = new Page ("Recording", "Count-in, monitoring and the metronome.");
                p->header ("Recording");
                p->choice ("Count-in", { "Off", "1 bar", "2 bars" }, [this] { return host.getCountIn ? host.getCountIn() : 0; },
                           [this] (int i) { if (host.setCountIn) host.setCountIn (i); });
                p->toggle ("Low-latency monitoring while recording", [&e] { return e.lowLatencyMonitoring.load(); },
                           [&e, st] (bool v) { e.lowLatencyMonitoring = v; st->setValue ("lowLatencyMonitoring", v); });
                p->toggle ("Return to start on stop", [&e] { return e.returnOnStop.load(); },
                           [&e, st] (bool v) { e.returnOnStop = v; st->setValue ("returnOnStop", v); });
                p->toggle ("Auto-crossfade overlapping clips", [&e] { return e.autoCrossfade.load(); },
                           [&e, st] (bool v) { e.autoCrossfade = v; st->setValue ("autoCrossfade", v); });
                p->header ("Metronome");
                p->choice ("Sound", { "Classic beep", "Woodblock", "Hi-hat" }, [&e] { return e.metronomeSound.load(); },
                           [&e, st] (int i) { e.metronomeSound = i; st->setValue ("metronomeSound", i); });
                p->slider ("Volume", 0.0, 150.0, 5.0, " %", [&e] { return e.metronomeVolume.load() * 100.0; },
                           [&e, st] (double v) { e.metronomeVolume = (float) (v / 100.0); st->setValue ("metronomeVolume", v / 100.0); });
                p->toggle ("Accent the first beat", [&e] { return e.metronomeAccent.load(); },
                           [&e, st] (bool v) { e.metronomeAccent = v; st->setValue ("metronomeAccent", v); });
                p->header ("Monitoring");
                p->choice ("Listen in", { "Stereo", "Mono check", "Side (L-R) check", "Left only", "Right only" }, [&e] { return e.monitorMode.load(); },
                           [&e] (int i) { e.monitorMode = i; });
                p->note ("Monitor modes are for checking only - exports are never affected.");
                addPage ("Recording", p);
            }

            // ---- Appearance
            {
                auto* p = new Page ("Appearance", "Themes, background and what you see at start-up.");
                p->header ("Look");
                juce::StringArray themes;
                for (int i = 0; i < Theme::kNumThemes; ++i)
                    themes.add (juce::String (Theme::kThemes[i].name) + (tier >= Theme::kThemes[i].tier ? "" : Theme::kThemes[i].tier >= 2 ? "  (Ultra Pro)" : "  (Pro)"));
                p->choice ("Colour theme", themes, [st] { return juce::jlimit (0, Theme::kNumThemes - 1, st->getIntValue ("colourTheme", 0)); },
                           [this, tier] (int i)
                {
                    if (tier < Theme::kThemes[i].tier) { if (host.proPrompt) host.proPrompt (juce::String (Theme::kThemes[i].name) + " colour theme", Theme::kThemes[i].tier); return; }
                    if (host.setTheme) host.setTheme (i);
                });
                p->choice ("App icon & logo", { "Amethyst gem", "Anime angel" }, [this] { return host.getIconStyle ? host.getIconStyle() : 0; },
                           [this] (int i) { if (host.setIconStyle) host.setIconStyle (i); });
                p->toggle ("Angel Easter eggs (she pops up now and then)", boolSetting ("mascotEggs", true), saveBool ("mascotEggs"));
                p->button ("Background picture", "Choose...", [this] { if (host.chooseBackground) host.chooseBackground(); });
                p->button ("Reset the background", "Use the default", [this] { if (host.removeBackground) host.removeBackground(); });
                p->slider ("Background haze", 0.0, 100.0, 1.0, " %", [st] { return st->getDoubleValue ("backgroundHaze", 0.6) * 100.0; },
                           [this] (double v) { if (host.setHaze) host.setHaze ((float) (v / 100.0)); });
                p->header ("Start-up");
                p->toggle ("Startup animation", boolSetting ("startupAnimation", true), saveBool ("startupAnimation"));
                p->toggle ("Show the welcome screen", boolSetting ("showWelcome", true), saveBool ("showWelcome"));
                p->toggle ("Show \"What's new\" after updates", boolSetting ("showWhatsNew", true), saveBool ("showWhatsNew"));
                addPage ("Appearance", p);
            }

            // ---- Files & export
            {
                auto* p = new Page ("Files & export", "How audio files import, export loudness and where things are kept.");
                p->header ("Audio files");
                p->toggle ("WAV / AIFF / FLAC play untouched (no automatic edge fades)", [&e] { return e.losslessUntouched.load(); },
                           [&e, st] (bool v) { e.losslessUntouched = v; st->setValue ("losslessUntouched", v); });
                p->toggle ("Trim MP3 encoder padding on import", [&e] { return e.trimMp3Padding.load(); },
                           [&e, st] (bool v) { e.trimMp3Padding = v; st->setValue ("trimMp3Padding", v); });
                p->header ("Export");
                p->choice ("Loudness target" + juce::String (tier >= 1 ? "" : "  (Pro)"),
                           { "Off (as mixed)", "Spotify / YouTube -14 LUFS", "Apple Music -16 LUFS", "Loud / club -9 LUFS", "Podcast -19 LUFS" },
                           [] { const double t = Exporter::loudnessTarget(); return t == -14.0 ? 1 : t == -16.0 ? 2 : t == -9.0 ? 3 : t == -19.0 ? 4 : 0; },
                           [this, st, tier] (int i)
                {
                    static const double values[] = { 0.0, -14.0, -16.0, -9.0, -19.0 };
                    if (i > 0 && tier < 1) { if (host.proPrompt) host.proPrompt ("Loudness-matched export", 1); return; }
                    Exporter::loudnessTarget() = values[i];
                    st->setValue ("exportLoudnessTarget", values[i]);
                });
                p->header ("Folders");
                p->button ("Recorded takes", "Open folder", [] { AudioEngine::getTakesFolder().createDirectory(); AudioEngine::getTakesFolder().startAsProcess(); });
                p->button ("Projects", "Open folder", [] { AudioEngine::getProjectsFolder().createDirectory(); AudioEngine::getProjectsFolder().startAsProcess(); });
                p->button ("Templates", "Open folder", [] { auto f = AudioEngine::getAppFolder().getChildFile ("Templates"); f.createDirectory(); f.startAsProcess(); });
                p->header ("Plug-ins");
                p->button ("VST3 plug-ins", "Plug-in manager...", [this] { if (host.openPluginManager) host.openPluginManager(); });
                addPage ("Files & export", p);
            }

            // ---- Online
            {
                auto* p = new Page ("Online", "Updates and your Discord status.");
                p->header ("Updates");
                p->toggle ("Check for updates automatically", boolSetting ("autoUpdate", true), saveBool ("autoUpdate"));
                p->button ("Version " + juce::String (ProjectInfo::versionString), "Check now", [this] { if (host.checkUpdates) host.checkUpdates(); });
                p->header ("Discord");
                auto discord = [this, st] (const char* key)
                {
                    return std::function<void (bool)> ([this, st, key] (bool v) { st->setValue (key, v); if (host.discordChanged) host.discordChanged(); });
                };
                p->toggle ("Show my activity on Discord", boolSetting ("discordEnabled", true), discord ("discordEnabled"));
                p->toggle ("Show the project name", boolSetting ("discordShowProject", false), discord ("discordShowProject"));
                p->toggle ("Show my plan (Pro / Ultra Pro)", boolSetting ("discordShowPlan", true), discord ("discordShowPlan"));
                addPage ("Online", p);
            }

            // ---- Shortcuts
            {
                auto* p = new Page ("Shortcuts", "Every command is also in the command palette (Ctrl+K).");
                auto* list = new ShortcutList (host.shortcutsText ? host.shortcutsText() : juce::String());
                p->header ("Keyboard");
                p->embed (list, list->idealHeight() + 8);
                addPage ("Shortcuts", p);
            }

            sidebar.onSelect = [this] (int i) { showPage (i); };
            addAndMakeVisible (sidebar);
            showPage (juce::jlimit (0, pages.size() - 1, initialTab));
            setSize (880, 620);
        }

        void showPage (int i)
        {
            sidebar.selected = i;
            sidebar.repaint();
            for (int k = 0; k < pages.size(); ++k)
                pages[k]->setVisible (k == i);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setGradientFill (juce::ColourGradient (Theme::bgTop.brighter (0.05f), r.getX(), r.getY(), Theme::bgBottom, r.getRight(), r.getBottom(), false));
            g.fillRect (r);
            // a soft glow behind the content
            g.setGradientFill (juce::ColourGradient (Theme::glow1.withAlpha (0.16f), r.getRight() - 120.0f, 40.0f,
                                                     Theme::glow1.withAlpha (0.0f), r.getRight() - 120.0f, 460.0f, true));
            g.fillRect (r);
        }

        void resized() override
        {
            auto r = getLocalBounds();
            sidebar.setBounds (r.removeFromLeft (210));
            for (auto* p : pages) p->setBounds (r);
        }

    private:
        void addPage (const juce::String& name, Page* p)
        {
            auto* s = pages.add (new Scroller (p));
            addChildComponent (s);
            sidebar.items.add (name);
        }

        Host host;
        Sidebar sidebar;
        juce::OwnedArray<Scroller> pages;
    };
}

std::unique_ptr<juce::Component> create (Host host, int initialTab)
{
    return std::make_unique<Window> (std::move (host), initialTab);
}

void show (Host host, juce::Component* centreAround, int initialTab)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new Window (std::move (host), initialTab));
    o.dialogTitle = "AMETHYST Settings";
    o.dialogBackgroundColour = Theme::windowBg;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.componentToCentreAround = centreAround;
    o.launchAsync();
}

namespace
{
    class WhatsNewCard : public juce::Component
    {
    public:
        WhatsNewCard (juce::String v, juce::StringArray list) : version (std::move (v)), items (std::move (list))
        {
            okBtn.onClick = [this] { if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState (0); };
            addAndMakeVisible (okBtn);
            setSize (520, 150 + 26 * items.size());
        }
        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setGradientFill (juce::ColourGradient (Theme::bgTop.brighter (0.1f), r.getX(), r.getY(), Theme::bgBottom, r.getRight(), r.getBottom(), false));
            g.fillRect (r);
            auto area = getLocalBounds().reduced (26, 20);
            g.setColour (Theme::accent);
            g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
            g.drawText ("UPDATED TO " + version, area.removeFromTop (18), juce::Justification::centredLeft);
            g.setColour (Theme::text);
            g.setFont (juce::FontOptions (24.0f, juce::Font::bold));
            g.drawText ("What's new", area.removeFromTop (36), juce::Justification::centredLeft);
            area.removeFromTop (6);
            for (auto& it : items)
            {
                auto row = area.removeFromTop (26);
                g.setColour (Theme::accent2);
                g.fillEllipse (row.removeFromLeft (14).toFloat().withSizeKeepingCentre (6.0f, 6.0f));
                g.setColour (Theme::text.withAlpha (0.92f));
                g.setFont (juce::FontOptions (14.0f));
                g.drawText (it, row.withTrimmedLeft (6), juce::Justification::centredLeft, true);
            }
        }
        void resized() override { okBtn.setBounds (getWidth() - 26 - 110, getHeight() - 20 - 32, 110, 32); }
    private:
        juce::String version;
        juce::StringArray items;
        juce::TextButton okBtn { "Got it" };
    };
}

void showWhatsNew (const juce::String& version, const juce::StringArray& items, juce::Component* centreAround)
{
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (new WhatsNewCard (version, items));
    o.dialogTitle = "What's new in AMETHYST 2026";
    o.dialogBackgroundColour = Theme::windowBg;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = centreAround;
    o.launchAsync();
}
}
