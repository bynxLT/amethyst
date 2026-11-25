#include "Builtins.h"

namespace
{
    struct Ring
    {
        std::vector<float> buf;
        int w = 0;
        void setMax (int samples) { buf.assign ((size_t) juce::jmax (4, samples), 0.0f); w = 0; }
        void push (float x) { buf[(size_t) w] = x; w = (w + 1) % (int) buf.size(); }
        float read (double d) const
        {
            const int len = (int) buf.size();
            d = juce::jlimit (1.0, (double) len - 2.0, d);
            double pos = w - 1 - d;
            while (pos < 0) pos += len;
            const int i0 = (int) pos;
            const float f = (float) (pos - i0);
            return buf[(size_t) i0] + (buf[(size_t) ((i0 + 1) % len)] - buf[(size_t) i0]) * f;
        }
    };

    //==========================================================================
    class DeEsserPlugin : public AmethystPlugin
    {
    public:
        DeEsserPlugin() : AmethystPlugin ("amethyst.deesser", "Silk De-Esser", "Dynamics", "Tames harsh S and T sounds", makeLayout())
        {
            sections = {
                { "Detection", { "listen", "freq", "thresh" }, 0 },
                { "Reduction", { "mode", "range" }, 0 },
            };
            presets = {
                { "Default", {} },
                { "Gentle",          { { "thresh", -24 }, { "range", 6 } } },
                { "Female Vocal",    { { "freq", 8000 }, { "thresh", -30 }, { "range", 10 } } },
                { "Male Vocal",      { { "freq", 5500 }, { "thresh", -30 }, { "range", 10 } } },
                { "Harsh Sibilance", { { "freq", 7000 }, { "thresh", -36 }, { "range", 16 } } },
                { "Wide Tame",       { { "mode", 1 }, { "freq", 6000 }, { "thresh", -26 }, { "range", 6 } } },
            };
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::toggle ("listen", "Listen", false));
            l.add (AmParams::num ("freq", "Freq", 2000.0f, 12000.0f, 6500.0f, "Hz", 0, 6000.0f));
            l.add (AmParams::num ("thresh", "Thresh", -60.0f, 0.0f, -28.0f, "dB", 1));
            l.add (AmParams::choice ("mode", "Mode", { "Split", "Wide" }, 0));
            l.add (AmParams::num ("range", "Range", 0.0f, 24.0f, 10.0f, "dB", 1));
            return l;
        }

        void prepareToPlay (double sr, int) override { sampleRate = sr; lastF = -1.0f; env = 0.0f; gain = 1.0f; }

        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buffer.getNumSamples();
            const int nCh = juce::jmin (2, buffer.getNumChannels());
            const float f = get ("freq");
            if (f != lastF)
            {
                lastF = f;
                const auto bp = juce::IIRCoefficients::makeBandPass (sampleRate, f, 1.4);
                const auto hp = juce::IIRCoefficients::makeHighPass (sampleRate, f * 0.6, 0.7);
                sc.setCoefficients (bp);
                for (auto& h : highSplit) h.setCoefficients (hp);
            }

            const float thr = get ("thresh"), range = get ("range");
            const bool split = getInt ("mode") == 0, listen = getBool ("listen");
            const float att = (float) std::exp (-1.0 / (0.0005 * sampleRate)), rel = (float) std::exp (-1.0 / (0.06 * sampleRate));
            const float gSmooth = (float) std::exp (-1.0 / (0.002 * sampleRate));
            float peak = -100.0f, maxGr = 0.0f;

            for (int i = 0; i < n; ++i)
            {
                const float l = buffer.getSample (0, i), r = nCh > 1 ? buffer.getSample (1, i) : l;
                const float side = sc.processSingleSampleRaw (0.5f * (l + r));
                const float a = std::abs (side);
                env = a > env ? att * env + (1.0f - att) * a : rel * env + (1.0f - rel) * a;
                const float envDb = juce::Decibels::gainToDecibels (env, -120.0f);
                peak = juce::jmax (peak, envDb);
                const float gr = juce::jlimit (0.0f, range, (envDb - thr) * 0.75f);
                maxGr = juce::jmax (maxGr, gr);
                const float target = juce::Decibels::decibelsToGain (-gr);
                gain = gSmooth * gain + (1.0f - gSmooth) * target;

                for (int c = 0; c < nCh; ++c)
                {
                    const float x = c == 0 ? l : r;
                    float y;
                    if (listen)
                        y = side;
                    else if (split)
                    {
                        const float hi = highSplit[(size_t) c].processSingleSampleRaw (x);
                        y = (x - hi) + hi * gain;
                    }
                    else
                        y = x * gain;
                    buffer.setSample (c, i, y);
                }
            }

            storeMax (meterIn, peak);
            storeMax (meterGr, maxGr);
        }

        std::unique_ptr<juce::Component> createVisual() override
        {
            return std::make_unique<MeterHistory> (meterIn, meterGr, [this] { return get ("thresh"); });
        }
        int getVisualHeight() const override { return 130; }

        std::atomic<float> meterIn { -100.0f }, meterGr { 0.0f };

    private:
        juce::IIRFilter sc;
        std::array<juce::IIRFilter, 2> highSplit;
        float lastF = -1.0f, env = 0.0f, gain = 1.0f;
    };

    //==========================================================================
    float shape (int mode, float x)
    {
        switch (mode)
        {
            case 0: return std::tanh (x);                                                        // tape
            case 1: return x >= 0.0f ? std::tanh (x) : std::tanh (x * 0.6f) / 0.6f * 0.82f;     // tube (asymmetric)
            case 2: return std::copysign (1.0f - std::exp (-std::abs (3.0f * x)), x) * 0.9f;   // fuzz
            default: return x;
        }
    }

    class WarmthPlugin : public AmethystPlugin
    {
    public:
        WarmthPlugin() : AmethystPlugin ("amethyst.warmth", "Ember", "Saturation", "Tape / tube / fuzz / crush", makeLayout())
        {
            sections = {
                { "Drive", { "mode", "drive", "tone" }, 0 },
                { "Output", { "mix", "out" }, 0 },
            };
            presets = {
                { "Default", {} },
                { "Tape Warmth", { { "drive", 8 }, { "tone", -10 } } },
                { "Tube Glow",   { { "mode", 1 }, { "drive", 10 }, { "tone", 10 } } },
                { "Vocal Grit",  { { "mode", 1 }, { "drive", 16 }, { "mix", 40 } } },
                { "Fuzz Box",    { { "mode", 2 }, { "drive", 24 }, { "mix", 70 }, { "tone", 20 } } },
                { "Lo-fi Crush", { { "mode", 3 }, { "drive", 24 }, { "mix", 80 }, { "tone", -30 } } },
                { "Radio",       { { "mode", 2 }, { "drive", 12 }, { "tone", 60 }, { "mix", 60 } } },
            };
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::choice ("mode", "Mode", { "Tape", "Tube", "Fuzz", "Crush" }, 0));
            l.add (AmParams::num ("drive", "Drive", 0.0f, 36.0f, 6.0f, "dB", 1));
            l.add (AmParams::num ("tone", "Tone", -100.0f, 100.0f, 0.0f, "%", 0));
            l.add (AmParams::num ("mix", "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
            l.add (AmParams::num ("out", "Output", -24.0f, 12.0f, 0.0f, "dB", 1));
            return l;
        }

        void prepareToPlay (double sr, int) override { sampleRate = sr; lastTone = -999.0f; hold.fill (0.0f); holdCount = 0; }

        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buffer.getNumSamples();
            const int nCh = juce::jmin (2, buffer.getNumChannels());
            const int mode = getInt ("mode");
            const float driveDb = get ("drive");
            const float drive = juce::Decibels::decibelsToGain (driveDb);
            const float comp = juce::Decibels::decibelsToGain (-driveDb * 0.5f + get ("out"));
            const float mix = get ("mix") * 0.01f;

            const float tone = get ("tone");
            if (tone != lastTone)
            {
                lastTone = tone;
                const float db = tone * 0.06f;
                for (int c = 0; c < 2; ++c)
                {
                    lowShelf[(size_t) c].setCoefficients (juce::IIRCoefficients::makeLowShelf (sampleRate, 300.0, 0.7, juce::Decibels::decibelsToGain (-db)));
                    highShelf[(size_t) c].setCoefficients (juce::IIRCoefficients::makeHighShelf (sampleRate, 3000.0, 0.7, juce::Decibels::decibelsToGain (db)));
                }
            }

            const float bits = 16.0f - driveDb / 36.0f * 12.0f;
            const float steps = std::pow (2.0f, bits - 1.0f);
            const int holdLen = 1 + (int) (driveDb / 6.0f);

            for (int c = 0; c < nCh; ++c)
            {
                auto* d = buffer.getWritePointer (c);
                for (int i = 0; i < n; ++i)
                {
                    const float x = d[i];
                    float y;
                    if (mode == 3)
                    {
                        if (c == 0 && ++holdCount >= holdLen) holdCount = 0;
                        if (holdCount == 0) hold[(size_t) c] = std::round (x * steps) / steps;
                        y = hold[(size_t) c];
                    }
                    else
                    {
                        y = shape (mode, x * drive) * comp;
                    }
                    y = highShelf[(size_t) c].processSingleSampleRaw (lowShelf[(size_t) c].processSingleSampleRaw (y));
                    d[i] = x + mix * (y - x);
                }
            }
        }

        std::unique_ptr<juce::Component> createVisual() override;
        int getVisualHeight() const override { return 140; }

    private:
        std::array<juce::IIRFilter, 2> lowShelf, highShelf;
        float lastTone = -999.0f;
        std::array<float, 2> hold {};
        int holdCount = 0;
    };

    // Transfer curve of the current saturation setting.
    class CurveVisual : public juce::Component, private juce::Timer
    {
    public:
        explicit CurveVisual (WarmthPlugin& p) : plugin (p) { startTimerHz (15); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            Glass::well (g, r, 12.0f);
            auto plot = r.reduced (16.0f, 12.0f);
            plot = plot.withSizeKeepingCentre (juce::jmin (plot.getWidth(), plot.getHeight() * 2.2f), plot.getHeight());

            g.setColour (juce::Colours::white.withAlpha (0.07f));
            g.drawHorizontalLine ((int) plot.getCentreY(), plot.getX(), plot.getRight());
            g.drawVerticalLine ((int) plot.getCentreX(), plot.getY(), plot.getBottom());

            const int mode = plugin.getInt ("mode");
            const float driveDb = plugin.get ("drive");
            const float drive = juce::Decibels::decibelsToGain (driveDb);
            const float steps = std::pow (2.0f, 16.0f - driveDb / 36.0f * 12.0f - 1.0f);
            const float mix = plugin.get ("mix") * 0.01f;

            juce::Path p;
            for (int i = 0; i <= 200; ++i)
            {
                const float x = -1.0f + 2.0f * (float) i / 200.0f;
                float y = mode == 3 ? std::round (x * juce::jmin (steps, 24.0f)) / juce::jmin (steps, 24.0f) : shape (mode, x * drive) / juce::jmax (1.0f, shape (mode, drive));
                y = x + mix * (y - x);
                const float px = plot.getX() + plot.getWidth() * (float) i / 200.0f;
                const float py = plot.getCentreY() - y * plot.getHeight() * 0.48f;
                if (i == 0) p.startNewSubPath (px, py); else p.lineTo (px, py);
            }
            g.setColour (Theme::accent2.withAlpha (0.3f));
            g.strokePath (p, juce::PathStrokeType (6.0f));
            g.setGradientFill (juce::ColourGradient (Theme::accent, plot.getX(), 0.0f, Theme::accent2, plot.getRight(), 0.0f, false));
            g.strokePath (p, juce::PathStrokeType (2.2f));
        }

    private:
        void timerCallback() override { repaint(); }
        WarmthPlugin& plugin;
    };

    std::unique_ptr<juce::Component> WarmthPlugin::createVisual() { return std::make_unique<CurveVisual> (*this); }

    //==========================================================================
    class DoublerPlugin : public AmethystPlugin
    {
    public:
        DoublerPlugin() : AmethystPlugin ("amethyst.doubler", "Twin Voice", "Vocal", "Instant double-tracking", makeLayout())
        {
            sections = {
                { "Voices", { "delay", "detune", "human" }, 0 },
                { "Image", { "width", "dry", "mix" }, 0 },
            };
            presets = {
                { "Default", {} },
                { "Tight Double",      { { "delay", 12 }, { "detune", 6 }, { "width", 70 } } },
                { "Wide Double",       { { "delay", 25 }, { "detune", 12 }, { "width", 100 } } },
                { "Big Chorus Vox",    { { "delay", 35 }, { "detune", 20 }, { "human", 70 }, { "mix", 60 } } },
                { "Subtle Thickener",  { { "delay", 15 }, { "detune", 4 }, { "mix", 30 }, { "width", 50 } } },
                { "Doubles Only",      { { "dry", 0 }, { "mix", 80 }, { "width", 100 } } },
            };
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::num ("delay", "Delay", 5.0f, 60.0f, 22.0f, "ms", 1));
            l.add (AmParams::num ("detune", "Detune", 0.0f, 40.0f, 10.0f, "ct", 1));
            l.add (AmParams::num ("human", "Humanize", 0.0f, 100.0f, 40.0f, "%", 0));
            l.add (AmParams::num ("width", "Width", 0.0f, 100.0f, 80.0f, "%", 0));
            l.add (AmParams::num ("dry", "Dry", 0.0f, 100.0f, 100.0f, "%", 0));
            l.add (AmParams::num ("mix", "Doubles", 0.0f, 100.0f, 50.0f, "%", 0));
            return l;
        }

        void prepareToPlay (double sr, int) override
        {
            sampleRate = sr;
            line.setMax ((int) (sr * 0.12));
            phase[0] = 0.0; phase[1] = 1.3;
            wander[0] = wander[1] = 0.0f;
            target[0] = target[1] = 0.0f;
        }

        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buffer.getNumSamples();
            const int nCh = juce::jmin (2, buffer.getNumChannels());
            const double baseMs = get ("delay");
            const double cents = get ("detune");
            const float human = get ("human") * 0.01f;
            const float width = get ("width") * 0.01f, dry = get ("dry") * 0.01f, mix = get ("mix") * 0.01f;

            static constexpr double rates[2] = { 0.41, 0.29 };
            double amp[2];
            for (int v = 0; v < 2; ++v)
                amp[v] = (std::pow (2.0, cents / 1200.0) - 1.0) / (juce::MathConstants<double>::twoPi * rates[v]) * sampleRate; // samples

            const float panL = 0.5f - 0.5f * width, panR = 0.5f + 0.5f * width;
            const float wanderCoef = (float) std::exp (-1.0 / (0.25 * sampleRate));

            for (int i = 0; i < n; ++i)
            {
                const float l = buffer.getSample (0, i), r = nCh > 1 ? buffer.getSample (1, i) : l;
                line.push (0.5f * (l + r));

                float voice[2];
                for (int v = 0; v < 2; ++v)
                {
                    if (rng.nextInt (4096) == 0)
                        target[v] = (rng.nextFloat() * 2.0f - 1.0f) * human;
                    wander[v] = wanderCoef * wander[v] + (1.0f - wanderCoef) * target[v];

                    const double d = baseMs * 0.001 * sampleRate * (v == 0 ? 1.0 : 1.37) * (1.0 + 0.25 * wander[v])
                                     + amp[v] * (1.0 + std::sin (phase[v]));
                    voice[v] = line.read (d);
                    phase[v] += juce::MathConstants<double>::twoPi * rates[v] / sampleRate;
                }

                const float outL = voice[0] * (1.0f - panL) + voice[1] * (1.0f - panR);
                const float outR = voice[0] * panL + voice[1] * panR;
                buffer.setSample (0, i, l * dry + outL * mix);
                if (nCh > 1) buffer.setSample (1, i, r * dry + outR * mix);
            }
        }

    private:
        Ring line;
        double phase[2] {};
        float wander[2] {}, target[2] {};
        juce::Random rng;
    };

    //==========================================================================
    // Delay-line pitch shifter with two crossfaded read heads.
    class ShiftPlugin : public AmethystPlugin
    {
    public:
        ShiftPlugin() : AmethystPlugin ("amethyst.shift", "Shift", "Pitch", "Pitch shifter for harmonies & octaves", makeLayout())
        {
            sections = {
                { "Pitch", { "semis", "fine", "window" }, 0 },
                { "Mix", { "dry", "wet" }, 0 },
            };
            presets = {
                { "Default", {} },
                { "Octave Up",      { { "semis", 12 }, { "wet", 60 } } },
                { "Octave Down",    { { "semis", -12 }, { "wet", 60 }, { "window", 80 } } },
                { "Fifth Up",       { { "semis", 7 }, { "wet", 50 } } },
                { "Harmony Third",  { { "semis", 4 }, { "wet", 45 } } },
                { "Chipmunk",       { { "semis", 12 }, { "dry", 0 }, { "wet", 100 } } },
                { "Monster",        { { "semis", -7 }, { "dry", 0 }, { "wet", 100 }, { "window", 90 } } },
                { "Subtle Detune",  { { "fine", 12 }, { "wet", 50 }, { "window", 35 } } },
            };
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::num ("semis", "Semitones", -12.0f, 12.0f, 0.0f, "st", 0, 0.0f, 1.0f));
            l.add (AmParams::num ("fine", "Fine", -100.0f, 100.0f, 0.0f, "ct", 0));
            l.add (AmParams::num ("window", "Window", 20.0f, 120.0f, 50.0f, "ms", 0));
            l.add (AmParams::num ("dry", "Dry", 0.0f, 100.0f, 100.0f, "%", 0));
            l.add (AmParams::num ("wet", "Wet", 0.0f, 100.0f, 70.0f, "%", 0));
            return l;
        }

        void prepareToPlay (double sr, int) override
        {
            sampleRate = sr;
            for (auto& l : lines) l.setMax ((int) (sr * 0.3));
            phasor = 0.0;
        }

        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buffer.getNumSamples();
            const int nCh = juce::jmin (2, buffer.getNumChannels());
            const double ratio = std::pow (2.0, (get ("semis") + get ("fine") / 100.0) / 12.0);
            const double win = get ("window") * 0.001 * sampleRate;
            const double inc = (1.0 - ratio) / win;
            const float dry = get ("dry") * 0.01f, wet = get ("wet") * 0.01f;

            for (int i = 0; i < n; ++i)
            {
                double p2 = phasor + 0.5;
                if (p2 >= 1.0) p2 -= 1.0;
                const float g1 = (float) std::sin (juce::MathConstants<double>::pi * phasor);
                const float g2 = (float) std::sin (juce::MathConstants<double>::pi * p2);

                for (int c = 0; c < nCh; ++c)
                {
                    const float x = buffer.getSample (c, i);
                    lines[(size_t) c].push (x);
                    const float y = g1 * lines[(size_t) c].read (2.0 + phasor * win) + g2 * lines[(size_t) c].read (2.0 + p2 * win);
                    buffer.setSample (c, i, x * dry + y * wet);
                }

                phasor += inc;
                if (phasor >= 1.0) phasor -= 1.0;
                if (phasor < 0.0)  phasor += 1.0;
            }
        }

    private:
        std::array<Ring, 2> lines;
        double phasor = 0.0;
    };
}

std::unique_ptr<AmethystPlugin> Builtins::makeDeEsser() { return std::make_unique<DeEsserPlugin>(); }
std::unique_ptr<AmethystPlugin> Builtins::makeWarmth()  { return std::make_unique<WarmthPlugin>(); }
std::unique_ptr<AmethystPlugin> Builtins::makeDoubler() { return std::make_unique<DoublerPlugin>(); }
std::unique_ptr<AmethystPlugin> Builtins::makeShift()   { return std::make_unique<ShiftPlugin>(); }

//==============================================================================
namespace
{
    struct Entry { const char* id; std::unique_ptr<AmethystPlugin> (*make)(); };
    const Entry kEntries[] = {
        { "amethyst.eq", Builtins::makeEq },             { "amethyst.dynamics", Builtins::makeDynamics },
        { "amethyst.reverb", Builtins::makeReverb },     { "amethyst.echo", Builtins::makeEcho },
        { "amethyst.prism", Builtins::makePrism },       { "amethyst.shine", Builtins::makeShine },
        { "amethyst.deesser", Builtins::makeDeEsser },   { "amethyst.warmth", Builtins::makeWarmth },
        { "amethyst.doubler", Builtins::makeDoubler },   { "amethyst.shift", Builtins::makeShift },
        { "amethyst.tune", Builtins::makeTune },
        { "amethyst.tapestop", Builtins::makeTapeStop }, { "amethyst.master", Builtins::makePolish },
        { "amethyst.rider", Builtins::makeRider },       { "amethyst.denoise", Builtins::makeDenoise },
        { "amethyst.harmony", Builtins::makeHarmony },   { "amethyst.imager", Builtins::makeImager },
        { "amethyst.transient", Builtins::makeTransient }, { "amethyst.gate", Builtins::makeGate },
        { "amethyst.autofilter", Builtins::makeAutoFilter }, { "amethyst.loudness", Builtins::makeLoudness },
    };
}

juce::Array<juce::PluginDescription> Builtins::getDescriptions()
{
    juce::Array<juce::PluginDescription> list;
    for (auto& e : kEntries)
    {
        juce::PluginDescription d;
        e.make()->fillInPluginDescription (d);
        list.add (d);
    }
    return list;
}

std::unique_ptr<juce::AudioPluginInstance> Builtins::create (const juce::String& identifier)
{
    for (auto& e : kEntries)
        if (identifier == e.id)
            return e.make();
    return nullptr;
}
