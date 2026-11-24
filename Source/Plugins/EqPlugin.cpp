#include "Builtins.h"

namespace
{
    using Coeffs = juce::dsp::IIR::Coefficients<float>;
    constexpr int kBands = 7;
    const juce::StringArray kTypes { "Low cut", "Low shelf", "Peak", "High shelf", "High cut", "Notch" };
    enum Type { LowCut, LowShelf, Peak, HighShelf, HighCut, Notch };

    const juce::Colour bandColours[kBands] = { juce::Colour (0xff7d6bff), juce::Colour (0xff9b6bff), juce::Colour (0xffc06bff),
                                               juce::Colour (0xffe86bf0), juce::Colour (0xffff6bb5), juce::Colour (0xffff8f8f),
                                               juce::Colour (0xff6bd5ff) };

    juce::String bandId (int b, const char* what) { return "b" + juce::String (b + 1) + "_" + what; }

    Coeffs::Ptr makeCoeffs (int type, double sr, float f, float gainDb, float q)
    {
        f = juce::jlimit (10.0f, (float) (sr * 0.49), f);
        q = juce::jmax (0.05f, q);
        const float g = juce::Decibels::decibelsToGain (gainDb);
        switch (type)
        {
            case LowCut:    return Coeffs::makeHighPass (sr, f, q);
            case LowShelf:  return Coeffs::makeLowShelf (sr, f, q, g);
            case HighShelf: return Coeffs::makeHighShelf (sr, f, q, g);
            case HighCut:   return Coeffs::makeLowPass (sr, f, q);
            case Notch:     return Coeffs::makeNotch (sr, f, q);
            default:        return Coeffs::makePeakFilter (sr, f, q, g);
        }
    }

    bool usesGain (int type) { return type == LowShelf || type == Peak || type == HighShelf; }

    //==========================================================================
    class EqPlugin : public AmethystPlugin
    {
    public:
        EqPlugin() : AmethystPlugin ("amethyst.eq", "Facet EQ", "EQ", "7-band parametric equalizer", makeLayout())
        {
            for (int b = 0; b < kBands; ++b)
                sections.push_back ({ "Band " + juce::String (b + 1), { bandId (b, "on"), bandId (b, "type"), bandId (b, "freq"), bandId (b, "gain"), bandId (b, "q") },
                                      b < 4 ? 0 : 1, 40 });
            sections.push_back ({ "Output", { "out" }, 1, 44 });

            presets = {
                { "Default", {} },
                { "Vocal Presence", { { "b1_type", LowCut }, { "b1_freq", 90 }, { "b1_q", 0.7f }, { "b3_freq", 320 }, { "b3_gain", -2.5f }, { "b3_q", 1.2f },
                                      { "b5_freq", 3200 }, { "b5_gain", 3.0f }, { "b5_q", 1.0f }, { "b7_freq", 12000 }, { "b7_gain", 3.5f } } },
                { "Air & Sparkle",  { { "b6_freq", 8000 }, { "b6_gain", 2.0f }, { "b7_freq", 14000 }, { "b7_gain", 5.0f }, { "b7_q", 0.6f } } },
                { "De-mud",         { { "b2_freq", 250 }, { "b2_gain", -4.0f }, { "b2_q", 1.4f }, { "b3_freq", 500 }, { "b3_gain", -2.0f } } },
                { "Warmth",         { { "b1_freq", 120 }, { "b1_gain", 3.0f }, { "b7_gain", -2.0f } } },
                { "Old Telephone",  { { "b1_type", LowCut }, { "b1_freq", 450 }, { "b1_q", 1.2f }, { "b4_freq", 1500 }, { "b4_gain", 6.0f }, { "b4_q", 1.0f },
                                      { "b7_type", HighCut }, { "b7_freq", 3200 }, { "b7_q", 1.2f } } },
                { "Radio",          { { "b1_type", LowCut }, { "b1_freq", 200 }, { "b4_freq", 2200 }, { "b4_gain", 4.0f }, { "b7_type", HighCut }, { "b7_freq", 6500 } } },
                { "Low Cut 80 Hz",  { { "b1_type", LowCut }, { "b1_freq", 80 }, { "b1_q", 0.7f } } },
                { "Kick",           { { "b1_freq", 60 }, { "b1_gain", 4.0f }, { "b3_freq", 400 }, { "b3_gain", -5.0f }, { "b5_freq", 3500 }, { "b5_gain", 3.0f } } },
                { "Tight Sizzle",   { { "b1_type", LowCut }, { "b1_freq", 250 }, { "b6_freq", 9000 }, { "b6_gain", 4.0f }, { "b6_q", 2.0f } } },
            };
        }

        static Layout makeLayout()
        {
            Layout l;
            const float freqs[kBands] = { 60, 180, 500, 1200, 3000, 7000, 14000 };
            const int types[kBands] = { LowShelf, Peak, Peak, Peak, Peak, Peak, HighShelf };
            for (int b = 0; b < kBands; ++b)
            {
                l.add (AmParams::toggle (bandId (b, "on"), "On", true));
                l.add (AmParams::choice (bandId (b, "type"), "Type", kTypes, types[b]));
                l.add (AmParams::num (bandId (b, "freq"), "Freq", 20.0f, 20000.0f, freqs[b], "Hz", 0, 1000.0f));
                l.add (AmParams::num (bandId (b, "gain"), "Gain", -18.0f, 18.0f, 0.0f, "dB", 1));
                l.add (AmParams::num (bandId (b, "q"), "Q", 0.1f, 18.0f, (types[b] == Peak ? 1.0f : 0.7f), {}, 2, 1.2f));
            }
            l.add (AmParams::num ("out", "Gain", -18.0f, 18.0f, 0.0f, "dB", 1));
            return l;
        }

        void prepareToPlay (double sr, int) override
        {
            sampleRate = sr;
            for (auto& b : bands)
            {
                b.lastType = -1;
                for (auto& f : b.filters)
                    f.reset();
            }
        }

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buffer.getNumSamples();
            const int nCh = juce::jmin (2, buffer.getNumChannels());

            for (int b = 0; b < kBands; ++b)
                updateBand (b);

            for (int c = 0; c < nCh; ++c)
            {
                auto* d = buffer.getWritePointer (c);
                for (auto& band : bands)
                {
                    if (! band.active)
                        continue;
                    auto& f = band.filters[(size_t) c];
                    for (int i = 0; i < n; ++i)
                        d[i] = f.processSample (d[i]);
                }
            }

            buffer.applyGain (juce::Decibels::decibelsToGain (get ("out")));

            // feed the analyzer
            int w = scopeWrite.load();
            for (int i = 0; i < n; ++i)
            {
                float v = buffer.getSample (0, i);
                if (nCh > 1) v = 0.5f * (v + buffer.getSample (1, i));
                scope[(size_t) w] = v;
                w = (w + 1) & (kScopeSize - 1);
            }
            scopeWrite.store (w);
        }

        std::unique_ptr<juce::Component> createVisual() override;
        int getVisualHeight() const override { return 300; }
        int getMinimumEditorWidth() const override { return 760; }

        static constexpr int kScopeSize = 4096;
        std::array<float, kScopeSize> scope {};
        std::atomic<int> scopeWrite { 0 };
        double getRate() const { return sampleRate; }

    private:
        struct Band
        {
            std::array<juce::dsp::IIR::Filter<float>, 2> filters;
            int lastType = -1;
            float lastF = 0, lastG = 0, lastQ = 0;
            bool active = false;
        };

        void updateBand (int b)
        {
            auto& band = bands[(size_t) b];
            const bool on = params (bandId (b, "on")) > 0.5f;
            const int type = (int) std::lround (params (bandId (b, "type")));
            const float f = params (bandId (b, "freq")), g = params (bandId (b, "gain")), q = params (bandId (b, "q"));

            band.active = on && ! (usesGain (type) && std::abs (g) < 0.05f);
            if (! band.active)
                return;

            if (type != band.lastType || f != band.lastF || g != band.lastG || q != band.lastQ)
            {
                auto c = makeCoeffs (type, sampleRate, f, g, q);
                for (auto& filt : band.filters)
                    filt.coefficients = c;
                if (type != band.lastType)
                    for (auto& filt : band.filters)
                        filt.reset();
                band.lastType = type; band.lastF = f; band.lastG = g; band.lastQ = q;
            }
        }

        float params (const juce::String& id) const { return apvts.getRawParameterValue (id)->load(); }

        std::array<Band, kBands> bands;
    };

    //==========================================================================
    // Curve display: drag nodes (freq/gain), scroll for Q, double-click to flatten, right-click for type.
    class EqDisplay : public juce::Component, private juce::Timer
    {
    public:
        explicit EqDisplay (EqPlugin& p) : plugin (p)
        {
            spectrum.fill (-100.0f);
            startTimerHz (30);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            Glass::well (g, r, 12.0f);
            plot = r.reduced (14.0f, 12.0f).withTrimmedBottom (14.0f);

            // grid
            g.setFont (juce::FontOptions (10.0f));
            for (float f : { 30.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f })
            {
                const float x = freqToX (f);
                g.setColour (juce::Colours::white.withAlpha (0.06f));
                g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
                g.setColour (Theme::textDim.withAlpha (0.55f));
                g.drawText (f >= 1000.0f ? juce::String (f / 1000.0f, 0) + "k" : juce::String ((int) f), (int) x - 20, (int) plot.getBottom() + 2, 40, 12,
                            juce::Justification::centred);
            }
            for (float db : { -12.0f, -6.0f, 0.0f, 6.0f, 12.0f })
            {
                const float y = gainToY (db);
                g.setColour (juce::Colours::white.withAlpha (db == 0.0f ? 0.16f : 0.06f));
                g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
                g.setColour (Theme::textDim.withAlpha (0.5f));
                g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), (int) plot.getRight() - 26, (int) y - 12, 24, 12, juce::Justification::centredRight);
            }

            // spectrum
            {
                juce::Path s;
                s.startNewSubPath (plot.getX(), plot.getBottom());
                const double sr = plugin.getRate();
                for (int x = 0; x <= (int) plot.getWidth(); x += 2)
                {
                    const float f = xToFreq (plot.getX() + (float) x);
                    const float bin = (float) (f * kFft / sr);
                    const int b0 = juce::jlimit (1, kFft / 2 - 2, (int) bin);
                    const float frac = bin - (float) b0;
                    const float db = spectrum[(size_t) b0] + (spectrum[(size_t) b0 + 1] - spectrum[(size_t) b0]) * frac;
                    const float y = plot.getBottom() - plot.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 90.0f) / 90.0f);
                    s.lineTo (plot.getX() + (float) x, y);
                }
                s.lineTo (plot.getRight(), plot.getBottom());
                s.closeSubPath();
                g.setGradientFill (juce::ColourGradient (Theme::accent2.withAlpha (0.28f), plot.getX(), plot.getY(),
                                                         Theme::accent.withAlpha (0.04f), plot.getX(), plot.getBottom(), false));
                g.fillPath (s);
            }

            // response curve
            juce::Path curve;
            const double sr = plugin.getRate();
            std::array<Coeffs::Ptr, kBands> coeffs;
            for (int b = 0; b < kBands; ++b)
                coeffs[(size_t) b] = bandActive (b) ? makeCoeffs (bandType (b), sr, param (b, "freq"), param (b, "gain"), param (b, "q")) : nullptr;

            const float outDb = plugin.apvts.getRawParameterValue ("out")->load();
            for (int x = 0; x <= (int) plot.getWidth(); ++x)
            {
                const float f = xToFreq (plot.getX() + (float) x);
                double mag = 1.0;
                for (auto& c : coeffs)
                    if (c != nullptr)
                        mag *= c->getMagnitudeForFrequency (f, sr);
                const float y = gainToY ((float) juce::Decibels::gainToDecibels (mag, -60.0) + outDb);
                if (x == 0) curve.startNewSubPath (plot.getX(), y); else curve.lineTo (plot.getX() + (float) x, y);
            }

            juce::Path fillArea (curve);
            fillArea.lineTo (plot.getRight(), gainToY (0.0f));
            fillArea.lineTo (plot.getX(), gainToY (0.0f));
            fillArea.closeSubPath();
            g.setColour (Theme::accent.withAlpha (0.10f));
            g.fillPath (fillArea);

            g.setColour (Theme::accent.withAlpha (0.35f));
            g.strokePath (curve, juce::PathStrokeType (6.0f));
            g.setColour (juce::Colours::white);
            g.strokePath (curve, juce::PathStrokeType (2.0f));

            // nodes
            for (int b = 0; b < kBands; ++b)
            {
                const auto pos = nodePos (b);
                const bool on = param (b, "on") > 0.5f;
                const auto col = bandColours[b];
                const float rad = (b == hover || b == dragging) ? 12.0f : 10.0f;

                if (on)
                {
                    g.setColour (col.withAlpha (0.35f));
                    g.fillEllipse (juce::Rectangle<float> (rad * 2.8f, rad * 2.8f).withCentre (pos));
                }
                g.setColour (on ? col.withAlpha (0.9f) : juce::Colour (0xff2a2142));
                g.fillEllipse (juce::Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (pos));
                g.setColour (juce::Colours::white.withAlpha (on ? 0.9f : 0.35f));
                g.drawEllipse (juce::Rectangle<float> (rad * 2.0f, rad * 2.0f).withCentre (pos), 1.2f);
                g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
                g.drawText (juce::String (b + 1), juce::Rectangle<float> (20.0f, 20.0f).withCentre (pos).toNearestInt(), juce::Justification::centred);
            }

            if (hover >= 0 || dragging >= 0)
            {
                const int b = dragging >= 0 ? dragging : hover;
                const auto txt = kTypes[bandType (b)] + "   " + formatFreq (param (b, "freq"))
                                 + (usesGain (bandType (b)) ? "   " + juce::String (param (b, "gain"), 1) + " dB" : juce::String())
                                 + "   Q " + juce::String (param (b, "q"), 2);
                g.setColour (Theme::text);
                g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
                g.drawText (txt, plot.toNearestInt().removeFromTop (18).withTrimmedLeft (6), juce::Justification::centredLeft);
            }
            else
            {
                g.setColour (Theme::textDim.withAlpha (0.6f));
                g.setFont (juce::FontOptions (11.0f));
                g.drawText ("Drag a node  -  scroll for Q  -  double-click to flatten  -  right-click for filter type",
                            plot.toNearestInt().removeFromTop (18).withTrimmedLeft (6), juce::Justification::centredLeft);
            }
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            const int h = nodeAt (e.position);
            if (h != hover) { hover = h; repaint(); }
        }

        void mouseExit (const juce::MouseEvent&) override { hover = -1; repaint(); }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const int b = nodeAt (e.position);
            if (b < 0)
                return;

            if (e.mods.isPopupMenu())
            {
                juce::PopupMenu m;
                m.addSectionHeader ("Band " + juce::String (b + 1));
                for (int t = 0; t < kTypes.size(); ++t)
                    m.addItem (1 + t, kTypes[t], true, bandType (b) == t);
                m.addSeparator();
                m.addItem (100, "Enabled", true, param (b, "on") > 0.5f);
                juce::Component::SafePointer<EqDisplay> safe (this);
                m.showMenuAsync (juce::PopupMenu::Options(), [safe, b] (int r)
                {
                    if (safe == nullptr || r == 0) return;
                    if (r == 100) safe->plugin.setParam (bandId (b, "on"), safe->param (b, "on") > 0.5f ? 0.0f : 1.0f);
                    else          safe->plugin.setParam (bandId (b, "type"), (float) (r - 1));
                });
                return;
            }

            dragging = b;
            if (param (b, "on") < 0.5f)
                plugin.setParam (bandId (b, "on"), 1.0f);
            for (auto* id : { "freq", "gain" })
                if (auto* p = plugin.apvts.getParameter (bandId (b, id)))
                    p->beginChangeGesture();
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragging < 0)
                return;
            auto setRaw = [this] (const juce::String& id, float v)
            {
                if (auto* p = plugin.apvts.getParameter (id))
                    p->setValueNotifyingHost (p->convertTo0to1 (v));
            };
            setRaw (bandId (dragging, "freq"), juce::jlimit (20.0f, 20000.0f, xToFreq (e.position.x)));
            if (usesGain (bandType (dragging)))
                setRaw (bandId (dragging, "gain"), juce::jlimit (-18.0f, 18.0f, yToGain (e.position.y)));
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (dragging >= 0)
                for (auto* id : { "freq", "gain" })
                    if (auto* p = plugin.apvts.getParameter (bandId (dragging, id)))
                        p->endChangeGesture();
            dragging = -1;
        }

        void mouseDoubleClick (const juce::MouseEvent& e) override
        {
            const int b = nodeAt (e.position);
            if (b >= 0)
                plugin.setParam (bandId (b, "gain"), 0.0f);
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
        {
            const int b = nodeAt (e.position);
            if (b >= 0)
                plugin.setParam (bandId (b, "q"), juce::jlimit (0.1f, 18.0f, param (b, "q") * (w.deltaY > 0 ? 1.12f : 1.0f / 1.12f)));
        }

    private:
        static constexpr int kFftOrder = 11, kFft = 1 << kFftOrder;

        void timerCallback() override
        {
            const int w = plugin.scopeWrite.load();
            for (int i = 0; i < kFft; ++i)
                fftData[(size_t) i] = plugin.scope[(size_t) ((w - kFft + i + EqPlugin::kScopeSize) & (EqPlugin::kScopeSize - 1))];
            std::fill (fftData.begin() + kFft, fftData.end(), 0.0f);
            window.multiplyWithWindowingTable (fftData.data(), (size_t) kFft);
            fft.performFrequencyOnlyForwardTransform (fftData.data());

            for (int i = 0; i < kFft / 2; ++i)
            {
                const float db = juce::Decibels::gainToDecibels (fftData[(size_t) i] / (kFft * 0.25f), -100.0f);
                spectrum[(size_t) i] = db > spectrum[(size_t) i] ? db : spectrum[(size_t) i] * 0.85f + db * 0.15f;
            }
            repaint();
        }

        float param (int b, const char* what) const { return plugin.apvts.getRawParameterValue (bandId (b, what))->load(); }
        int bandType (int b) const { return (int) std::lround (param (b, "type")); }
        bool bandActive (int b) const { return param (b, "on") > 0.5f && ! (usesGain (bandType (b)) && std::abs (param (b, "gain")) < 0.05f); }

        float freqToX (float f) const { return plot.getX() + plot.getWidth() * (std::log10 (f / 20.0f) / 3.0f); }
        float xToFreq (float x) const { return 20.0f * std::pow (10.0f, 3.0f * (x - plot.getX()) / juce::jmax (1.0f, plot.getWidth())); }
        float gainToY (float db) const { return plot.getCentreY() - plot.getHeight() * 0.5f * (db / 20.0f); }
        float yToGain (float y) const { return (plot.getCentreY() - y) / (plot.getHeight() * 0.5f) * 20.0f; }

        juce::Point<float> nodePos (int b) const
        {
            const float g = usesGain (bandType (b)) ? param (b, "gain") : 0.0f;
            return { freqToX (param (b, "freq")), gainToY (g) };
        }

        int nodeAt (juce::Point<float> p) const
        {
            int best = -1;
            float bestD = 15.0f;
            for (int b = 0; b < kBands; ++b)
            {
                const float d = nodePos (b).getDistanceFrom (p);
                if (d < bestD) { bestD = d; best = b; }
            }
            return best;
        }

        static juce::String formatFreq (float f) { return f >= 1000.0f ? juce::String (f / 1000.0f, 2) + " kHz" : juce::String ((int) f) + " Hz"; }

        EqPlugin& plugin;
        juce::Rectangle<float> plot;
        int hover = -1, dragging = -1;

        juce::dsp::FFT fft { kFftOrder };
        juce::dsp::WindowingFunction<float> window { (size_t) kFft, juce::dsp::WindowingFunction<float>::hann };
        std::array<float, kFft * 2> fftData {};
        std::array<float, kFft / 2> spectrum {};
    };

    std::unique_ptr<juce::Component> EqPlugin::createVisual() { return std::make_unique<EqDisplay> (*this); }
}

std::unique_ptr<AmethystPlugin> Builtins::makeEq() { return std::make_unique<EqPlugin>(); }
