#include "Builtins.h"

namespace
{
    //==========================================================================
    // Tape Stop: switch "Play" off and the audio winds down like a tape machine losing power;
    // switch it back on and it spins up again. Automate "Play" for drops.
    class TapeStopPlugin : public AmethystPlugin
    {
    public:
        TapeStopPlugin() : AmethystPlugin ("amethyst.tapestop", "Tape Stop", "Creative", "Wind-down and spin-up tape effect", makeLayout())
        {
            sections = { { "Timing", { "stop", "start", "curve" }, 0, 62 } };
            presets = {
                { "Default", {} },
                { "Quick Brake",  { { "stop", 250 }, { "start", 120 } } },
                { "Long Wind-down", { { "stop", 2500 }, { "start", 900 }, { "curve", 70 } } },
                { "Vinyl Spin-up", { { "stop", 600 }, { "start", 1200 }, { "curve", 30 } } },
                { "Glitch Stutter", { { "stop", 90 }, { "start", 60 }, { "curve", 0 } } },
            };
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::toggle ("play", "Play", true));
            l.add (AmParams::num ("stop", "Stop time", 30.0f, 4000.0f, 800.0f, "ms", 0, 600.0f));
            l.add (AmParams::num ("start", "Start time", 30.0f, 4000.0f, 350.0f, "ms", 0, 400.0f));
            l.add (AmParams::num ("curve", "Curve", 0.0f, 100.0f, 50.0f, "%", 0));
            return l;
        }

        void prepareToPlay (double sr, int) override
        {
            sampleRate = sr;
            size = (int) (sr * 8.0);
            for (auto& b : ring) b.assign ((size_t) size, 0.0f);
            w = 0;
            lag = 0.0;
            speed = getBool ("play") ? 1.0 : 0.0;
            following = true;
            xfade = 0;
        }

        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            if (size <= 0)
                return;
            const int n = buffer.getNumSamples();
            const int nCh = juce::jmin (2, buffer.getNumChannels());
            const bool play = getBool ("play");
            const double downStep = 1.0 / juce::jmax (1.0, get ("stop") * 0.001 * sampleRate);
            const double upStep = 1.0 / juce::jmax (1.0, get ("start") * 0.001 * sampleRate);
            const float curve = get ("curve") * 0.01f;   // 0 = linear, 1 = heavy ease
            const int xfadeLen = (int) (0.03 * sampleRate);

            for (int i = 0; i < n; ++i)
            {
                float in[2] = { buffer.getSample (0, i), nCh > 1 ? buffer.getSample (1, i) : 0.0f };
                for (int c = 0; c < 2; ++c)
                    ring[(size_t) c][(size_t) w] = in[c];

                if (play && speed < 1.0)       { speed = juce::jmin (1.0, speed + upStep); }
                else if (! play && speed > 0.0) { speed = juce::jmax (0.0, speed - downStep); following = false; }

                // perceived playback rate: blend of linear and eased ramps
                const double eased = speed * speed * (3.0 - 2.0 * speed);
                const double rate = speed + (eased - speed) * curve;

                float out[2] = { in[0], in[1] };
                if (! following)
                {
                    if (rate <= 0.0)
                    {
                        lag = 0.0;            // fully stopped: a restart spins up from "now"
                        out[0] = out[1] = 0.0f;
                    }
                    else
                    {
                        lag = juce::jlimit (0.0, (double) size - 4.0, lag + 1.0 - rate);   // write moves 1, read moves rate
                        double pos = (double) w - lag;
                        if (pos < 0.0) pos += size;
                        const int i0 = (int) pos % size;
                        const int i1 = (i0 + 1) % size;
                        const float f = (float) (pos - std::floor (pos));
                        const float level = (float) juce::jmin (1.0, rate * 6.0);
                        float tape[2];
                        for (int c = 0; c < 2; ++c)
                            tape[c] = (ring[(size_t) c][(size_t) i0] + (ring[(size_t) c][(size_t) i1] - ring[(size_t) c][(size_t) i0]) * f) * level;

                        if (play && speed >= 1.0)
                        {
                            // back up to speed: crossfade from the lagging tape to the live signal
                            const float t = (float) juce::jmin (1.0, (double) ++xfade / juce::jmax (1, xfadeLen));
                            for (int c = 0; c < 2; ++c)
                                out[c] = tape[c] * (1.0f - t) + in[c] * t;
                            if (t >= 1.0f) { following = true; xfade = 0; lag = 0.0; }
                        }
                        else
                        {
                            xfade = 0;
                            out[0] = tape[0];
                            out[1] = tape[1];
                        }
                    }
                }

                buffer.setSample (0, i, out[0]);
                if (nCh > 1) buffer.setSample (1, i, out[1]);
                w = (w + 1) % size;
            }
            displaySpeed.store ((float) speed);
        }

        std::unique_ptr<juce::Component> createVisual() override;
        int getVisualHeight() const override { return 170; }
        int getMinimumEditorWidth() const override { return 320; }

        std::atomic<float> displaySpeed { 1.0f };

    private:
        std::array<std::vector<float>, 2> ring;
        int size = 0, w = 0, xfade = 0;
        double lag = 0.0, speed = 1.0;
        bool following = true;
    };

    // Two spinning reels with a big play/stop button in the middle.
    class TapeVisual : public juce::Component, private juce::Timer
    {
    public:
        explicit TapeVisual (TapeStopPlugin& p) : plugin (p) { startTimerHz (40); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            Glass::well (g, r, 14.0f);
            const float s = plugin.displaySpeed.load();
            const float reel = juce::jmin (r.getHeight() * 0.36f, r.getWidth() * 0.18f);
            const auto left = juce::Point<float> (r.getX() + r.getWidth() * 0.22f, r.getCentreY() - 6.0f);
            const auto right = juce::Point<float> (r.getRight() - r.getWidth() * 0.22f, r.getCentreY() - 6.0f);

            // tape path between the reels
            g.setColour (Theme::accent.withAlpha (0.35f));
            g.drawLine (left.x, left.y + reel, right.x, right.y + reel, 2.0f);

            for (auto c : { left, right })
            {
                g.setColour (juce::Colours::white.withAlpha (0.06f));
                g.fillEllipse (juce::Rectangle<float> (reel * 2, reel * 2).withCentre (c));
                g.setColour (Theme::accent.withAlpha (0.25f + 0.5f * s));
                g.drawEllipse (juce::Rectangle<float> (reel * 2, reel * 2).withCentre (c), 2.0f);
                for (int k = 0; k < 3; ++k)
                {
                    const float a = angle + (float) k * juce::MathConstants<float>::twoPi / 3.0f;
                    juce::Path spoke;
                    spoke.addPieSegment (juce::Rectangle<float> (reel * 1.5f, reel * 1.5f).withCentre (c), a, a + 0.75f, 0.45f);
                    g.setColour (Theme::text.withAlpha (0.18f + 0.25f * s));
                    g.fillPath (spoke);
                }
                g.setColour (Theme::text.withAlpha (0.8f));
                g.fillEllipse (juce::Rectangle<float> (reel * 0.3f, reel * 0.3f).withCentre (c));
            }

            const auto btn = buttonArea();
            const bool on = plugin.getBool ("play");
            g.setColour ((on ? Theme::accent : Theme::record).withAlpha (hover ? 0.35f : 0.22f));
            g.fillEllipse (btn);
            g.setColour (on ? Theme::accent : Theme::record);
            g.drawEllipse (btn, 1.5f);
            g.setColour (Theme::text);
            const auto icon = btn.reduced (btn.getWidth() * 0.32f);
            if (on)
            {
                juce::Path tri;
                tri.addTriangle (icon.getX() + 2, icon.getY(), icon.getX() + 2, icon.getBottom(), icon.getRight(), icon.getCentreY());
                g.fillPath (tri);
            }
            else
                g.fillRoundedRectangle (icon, 2.0f);

            g.setColour (Theme::textDim);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (on ? (s < 0.999f ? "SPINNING UP" : "PLAYING  -  CLICK TO STOP") : (s > 0.001f ? "WINDING DOWN" : "STOPPED  -  CLICK TO PLAY"),
                        r.reduced (10.0f).toNearestInt(), juce::Justification::centredBottom);
        }

        void mouseMove (const juce::MouseEvent& e) override { setHover (buttonArea().contains (e.position)); }
        void mouseExit (const juce::MouseEvent&) override { setHover (false); }
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (buttonArea().contains (e.position))
                plugin.setParam ("play", plugin.getBool ("play") ? 0.0f : 1.0f);
        }

    private:
        juce::Rectangle<float> buttonArea() const
        {
            const auto r = getLocalBounds().toFloat();
            const float d = juce::jmin (54.0f, r.getHeight() * 0.42f);
            return juce::Rectangle<float> (d, d).withCentre ({ r.getCentreX(), r.getCentreY() - 6.0f });
        }
        void setHover (bool h) { if (h != hover) { hover = h; repaint(); } }
        void timerCallback() override
        {
            angle += 0.18f * plugin.displaySpeed.load();
            repaint();
        }

        TapeStopPlugin& plugin;
        float angle = 0.0f;
        bool hover = false;
    };

    std::unique_ptr<juce::Component> TapeStopPlugin::createVisual() { return std::make_unique<TapeVisual> (*this); }

    //==========================================================================
    // Polish: one-click mastering for the master channel. Pick a style, set the intensity,
    // optionally nudge low / mid / high. EQ -> glue compression -> saturation -> width -> limiter.
    struct MasterStyle
    {
        const char* name;
        juce::uint32 colour;
        float lowDb, presenceDb, airDb, ratio, attackMs, sat, width, loudDb;
    };

    const MasterStyle kStyles[] = {
        { "Balanced",  0xffb47dff, 1.0f,  0.5f,  1.0f, 2.0f, 20.0f, 0.10f, 1.05f, 4.0f },
        { "Warm",      0xffff9a5c, 2.5f, -0.5f, -1.0f, 2.0f, 25.0f, 0.35f, 1.00f, 4.0f },
        { "Bright",    0xff6fd6ff, -0.5f, 1.5f,  3.0f, 2.0f, 15.0f, 0.10f, 1.10f, 4.0f },
        { "Tape",      0xffffc85c, 1.5f,  0.0f, -1.5f, 2.5f, 30.0f, 0.60f, 0.95f, 4.0f },
        { "Natural",   0xff7dffb0, 0.5f,  0.0f,  0.5f, 1.5f, 30.0f, 0.00f, 1.00f, 2.0f },
        { "Wide",      0xff8f9bff, 0.5f,  0.5f,  1.5f, 2.0f, 20.0f, 0.10f, 1.35f, 3.5f },
        { "Cinematic", 0xffff70d9, 2.0f, -1.0f,  2.0f, 1.8f, 35.0f, 0.15f, 1.25f, 3.0f },
        { "Punch",     0xffff5c7a, 2.0f,  1.0f,  0.5f, 4.0f, 45.0f, 0.25f, 1.00f, 6.0f },
    };
    constexpr int kNumStyles = (int) (sizeof (kStyles) / sizeof (kStyles[0]));

    juce::StringArray styleNames()
    {
        juce::StringArray s;
        for (auto& st : kStyles) s.add (st.name);
        return s;
    }

    class PolishPlugin : public AmethystPlugin
    {
    public:
        PolishPlugin() : AmethystPlugin ("amethyst.master", "Polish", "Mastering", "One-click mastering for your mix", makeLayout())
        {
            sections = { { "Intensity", { "intensity" }, 0, 62 }, { "Tone", { "eq_on", "low", "mid", "high" }, 0, 52 } };
            presets.push_back ({ "Default", {} });
            for (int i = 0; i < kNumStyles; ++i)
            {
                Preset p;
                p.name = kStyles[i].name;
                p.values.push_back (PresetValue ("style", (double) i));
                presets.push_back (p);
            }
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::choice ("style", "Style", styleNames(), 0));
            l.add (AmParams::num ("intensity", "Intensity", 0.0f, 100.0f, 50.0f, "%", 0));
            l.add (AmParams::toggle ("eq_on", "EQ", false));
            l.add (AmParams::num ("low", "Low", -6.0f, 6.0f, 0.0f, "dB", 1));
            l.add (AmParams::num ("mid", "Mid", -6.0f, 6.0f, 0.0f, "dB", 1));
            l.add (AmParams::num ("high", "High", -6.0f, 6.0f, 0.0f, "dB", 1));
            return l;
        }

        void prepareToPlay (double sr, int bs) override
        {
            sampleRate = sr;
            juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, bs), 1 };
            for (auto& ch : filters)
                for (auto& f : ch) { f.prepare (spec); f.reset(); }
            env = 0.0f;
            limGain = 1.0f;
            lastKey = -1.0f;
        }

        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const auto& st = kStyles[juce::jlimit (0, kNumStyles - 1, getInt ("style"))];
            const float k = get ("intensity") / 50.0f;   // 0..2, 1 = "normal"
            const bool eqOn = getBool ("eq_on");
            const float uLow = eqOn ? get ("low") : 0.0f, uMid = eqOn ? get ("mid") : 0.0f, uHigh = eqOn ? get ("high") : 0.0f;

            // Re-design the filters only when something changed.
            const float key = (float) getInt ("style") * 1000.0f + k * 37.0f + uLow * 3.1f + uMid * 5.3f + uHigh * 7.7f;
            if (key != lastKey)
            {
                lastKey = key;
                using C = juce::dsp::IIR::Coefficients<float>;
                const double sr = sampleRate;
                auto g = [] (float db) { return juce::Decibels::decibelsToGain (db); };
                const std::array<C::Ptr, 5> c {
                    C::makeLowShelf  (sr, 95.0,    0.7f, g (st.lowDb * k + uLow)),
                    C::makePeakFilter (sr, 1000.0, 0.8f, g (uMid)),
                    C::makePeakFilter (sr, 3200.0, 0.9f, g (st.presenceDb * k)),
                    C::makeHighShelf (sr, 11000.0, 0.7f, g (st.airDb * k + uHigh)),
                    C::makeHighPass  (sr, 25.0,    0.7f),
                };
                for (auto& ch : filters)
                    for (size_t i = 0; i < ch.size(); ++i)
                        *ch[i].coefficients = *c[i];
            }

            const int n = buffer.getNumSamples();
            const int nCh = juce::jmin (2, buffer.getNumChannels());
            for (int c = 0; c < nCh; ++c)
            {
                juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers() + c, 1, (size_t) n);
                juce::dsp::ProcessContextReplacing<float> ctx (block);
                for (auto& f : filters[(size_t) c])
                    f.process (ctx);
            }

            const float ratio = 1.0f + (st.ratio - 1.0f) * juce::jmin (k, 1.5f);
            const float slope = 1.0f - 1.0f / ratio;
            const float thr = -16.0f;
            const float att = (float) std::exp (-1.0 / (st.attackMs * 0.001 * sampleRate));
            const float rel = (float) std::exp (-1.0 / (0.220 * sampleRate));
            const float sat = st.sat * k;
            const float satNorm = std::tanh (1.0f + 2.0f * sat);
            const float width = 1.0f + (st.width - 1.0f) * k;
            const float makeup = juce::Decibels::decibelsToGain (st.loudDb * k);
            const float ceiling = juce::Decibels::decibelsToGain (-0.3f);
            const float limRel = (float) std::exp (-1.0 / (0.080 * sampleRate));

            for (int i = 0; i < n; ++i)
            {
                float l = buffer.getSample (0, i);
                float r = nCh > 1 ? buffer.getSample (1, i) : l;

                // glue compressor (stereo linked)
                const float level = juce::jmax (std::abs (l), std::abs (r));
                env = level > env ? att * env + (1.0f - att) * level : rel * env + (1.0f - rel) * level;
                const float over = juce::Decibels::gainToDecibels (env, -120.0f) - thr;
                const float gr = juce::Decibels::decibelsToGain (-juce::jmax (0.0f, over) * slope);
                l *= gr; r *= gr;

                // saturation
                if (sat > 0.001f)
                {
                    const float d = 1.0f + 2.0f * sat;
                    l = l + sat * (std::tanh (d * l) / satNorm - l);
                    r = r + sat * (std::tanh (d * r) / satNorm - r);
                }

                // width (mid / side)
                const float m = 0.5f * (l + r), s = 0.5f * (l - r) * width;
                l = (m + s) * makeup;
                r = (m - s) * makeup;

                // peak limiter + soft ceiling
                const float peak = juce::jmax (std::abs (l), std::abs (r));
                const float target = peak * limGain > ceiling ? ceiling / juce::jmax (peak, 1.0e-6f) : 1.0f;
                limGain = target < limGain ? target : limRel * limGain + (1.0f - limRel) * target;
                l = softCeiling (l * limGain, ceiling);
                r = softCeiling (r * limGain, ceiling);

                buffer.setSample (0, i, l);
                if (nCh > 1) buffer.setSample (1, i, r);
            }
        }

        std::unique_ptr<juce::Component> createVisual() override;
        int getVisualHeight() const override { return 214; }
        int getMinimumEditorWidth() const override { return 380; }

    private:
        std::array<std::array<juce::dsp::IIR::Filter<float>, 5>, 2> filters;
        float env = 0.0f, limGain = 1.0f, lastKey = -1.0f;
    };

    // Style picker: a grid of glass tiles, each with its own colour and glyph.
    class StyleGrid : public juce::Component, private juce::Timer
    {
    public:
        explicit StyleGrid (PolishPlugin& p) : plugin (p) { startTimerHz (15); }

        void paint (juce::Graphics& g) override
        {
            Glass::well (g, getLocalBounds().toFloat(), 14.0f);
            const int sel = plugin.getInt ("style");
            for (int i = 0; i < kNumStyles; ++i)
            {
                const auto t = tile (i);
                const auto col = juce::Colour (kStyles[i].colour);
                const bool on = i == sel;
                g.setColour (on ? col.withAlpha (0.18f) : juce::Colours::white.withAlpha (i == hover ? 0.07f : 0.035f));
                g.fillRoundedRectangle (t, 8.0f);
                g.setColour (on ? col : juce::Colours::white.withAlpha (0.10f));
                g.drawRoundedRectangle (t.reduced (0.5f), 8.0f, on ? 1.5f : 1.0f);

                auto glyphArea = t.withTrimmedBottom (t.getHeight() * 0.38f).reduced (t.getHeight() * 0.14f);
                drawGlyph (g, i, glyphArea.withSizeKeepingCentre (glyphArea.getHeight(), glyphArea.getHeight()), on ? col : col.withAlpha (0.55f));
                g.setColour (on ? Theme::text : Theme::textDim);
                g.setFont (juce::FontOptions (12.0f, on ? juce::Font::bold : juce::Font::plain));
                g.drawText (kStyles[i].name, t.withTrimmedTop (t.getHeight() * 0.62f).toNearestInt(), juce::Justification::centred);
            }
        }

        void mouseMove (const juce::MouseEvent& e) override { const int h = hit (e.position); if (h != hover) { hover = h; repaint(); } }
        void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }
        void mouseDown (const juce::MouseEvent& e) override
        {
            const int h = hit (e.position);
            if (h >= 0)
                plugin.setParam ("style", (float) h);
            repaint();
        }

    private:
        juce::Rectangle<float> tile (int i) const
        {
            auto r = getLocalBounds().toFloat().reduced (10.0f);
            const float w = r.getWidth() / 4.0f, h = r.getHeight() / 2.0f;
            return juce::Rectangle<float> (r.getX() + w * (float) (i % 4), r.getY() + h * (float) (i / 4), w, h).reduced (4.0f);
        }
        int hit (juce::Point<float> p) const
        {
            for (int i = 0; i < kNumStyles; ++i)
                if (tile (i).contains (p)) return i;
            return -1;
        }

        static void drawGlyph (juce::Graphics& g, int i, juce::Rectangle<float> a, juce::Colour c)
        {
            g.setColour (c);
            const auto ctr = a.getCentre();
            const float s = a.getWidth() * 0.5f;
            juce::Path p;
            switch (i)
            {
                case 0: // balanced: gem
                    p.startNewSubPath (ctr.x, ctr.y - s); p.lineTo (ctr.x + s, ctr.y - s * 0.2f); p.lineTo (ctr.x, ctr.y + s);
                    p.lineTo (ctr.x - s, ctr.y - s * 0.2f); p.closeSubPath();
                    g.strokePath (p, juce::PathStrokeType (1.6f));
                    g.drawLine (ctr.x - s, ctr.y - s * 0.2f, ctr.x + s, ctr.y - s * 0.2f, 1.2f);
                    break;
                case 1: // warm: ember
                    p.addEllipse (a.reduced (s * 0.35f));
                    g.fillPath (p);
                    for (int k = 0; k < 8; ++k)
                    {
                        const float ang = (float) k * juce::MathConstants<float>::twoPi / 8.0f;
                        g.drawLine (juce::Line<float> (ctr.getPointOnCircumference (s * 0.78f, ang), ctr.getPointOnCircumference (s, ang)), 1.4f);
                    }
                    break;
                case 2: // bright: sparkle
                    for (int k = 0; k < 4; ++k)
                    {
                        const float ang = (float) k * juce::MathConstants<float>::halfPi;
                        g.drawLine (juce::Line<float> (ctr, ctr.getPointOnCircumference (s, ang)), 1.8f);
                        g.drawLine (juce::Line<float> (ctr, ctr.getPointOnCircumference (s * 0.5f, ang + juce::MathConstants<float>::pi / 4.0f)), 1.2f);
                    }
                    break;
                case 3: // tape: cassette
                    g.drawRoundedRectangle (a.reduced (0.0f, s * 0.3f), 3.0f, 1.6f);
                    g.drawEllipse (juce::Rectangle<float> (s * 0.5f, s * 0.5f).withCentre ({ ctr.x - s * 0.4f, ctr.y }), 1.4f);
                    g.drawEllipse (juce::Rectangle<float> (s * 0.5f, s * 0.5f).withCentre ({ ctr.x + s * 0.4f, ctr.y }), 1.4f);
                    break;
                case 4: // natural: leaf
                    p.startNewSubPath (ctr.x, ctr.y + s);
                    p.quadraticTo (ctr.x - s, ctr.y, ctr.x, ctr.y - s);
                    p.quadraticTo (ctr.x + s, ctr.y, ctr.x, ctr.y + s);
                    g.strokePath (p, juce::PathStrokeType (1.6f));
                    g.drawLine (ctr.x, ctr.y + s, ctr.x, ctr.y - s * 0.4f, 1.2f);
                    break;
                case 5: // wide: arrows apart
                    g.drawArrow (juce::Line<float> (ctr.x - s * 0.2f, ctr.y, ctr.x - s, ctr.y), 1.6f, 6.0f, 6.0f);
                    g.drawArrow (juce::Line<float> (ctr.x + s * 0.2f, ctr.y, ctr.x + s, ctr.y), 1.6f, 6.0f, 6.0f);
                    break;
                case 6: // cinematic: wide screen
                    g.drawRoundedRectangle (a.reduced (0.0f, s * 0.35f), 2.0f, 1.6f);
                    g.drawLine (a.getX() + s * 0.3f, a.getBottom() - s * 0.2f, a.getRight() - s * 0.3f, a.getBottom() - s * 0.2f, 1.2f);
                    break;
                default: // punch: lightning
                    p.startNewSubPath (ctr.x + s * 0.3f, ctr.y - s); p.lineTo (ctr.x - s * 0.4f, ctr.y + s * 0.1f);
                    p.lineTo (ctr.x + s * 0.05f, ctr.y + s * 0.1f); p.lineTo (ctr.x - s * 0.3f, ctr.y + s);
                    p.lineTo (ctr.x + s * 0.45f, ctr.y - s * 0.15f); p.lineTo (ctr.x - s * 0.02f, ctr.y - s * 0.15f); p.closeSubPath();
                    g.fillPath (p);
                    break;
            }
        }

        void timerCallback() override
        {
            const int s = plugin.getInt ("style");
            if (s != lastStyle) { lastStyle = s; repaint(); }
        }

        PolishPlugin& plugin;
        int hover = -1, lastStyle = -1;
    };

    std::unique_ptr<juce::Component> PolishPlugin::createVisual() { return std::make_unique<StyleGrid> (*this); }
}

std::unique_ptr<AmethystPlugin> Builtins::makeTapeStop() { return std::make_unique<TapeStopPlugin>(); }
std::unique_ptr<AmethystPlugin> Builtins::makePolish()   { return std::make_unique<PolishPlugin>(); }
juce::StringArray Builtins::masterStyleNames()           { return styleNames(); }
