#include "Builtins.h"

namespace
{
    float coef (double timeMs, double sr) { return (float) std::exp (-1.0 / (juce::jmax (0.01, timeMs) * 0.001 * sr)); }

    //==========================================================================
    // Dynamics: saturating pre-gain, limiter or compressor, noise gate, parallel mix, safety ceiling.
    class DynamicsPlugin : public AmethystPlugin
    {
    public:
        DynamicsPlugin() : AmethystPlugin ("amethyst.dynamics", "Prism Dynamics", "Dynamics", "Limiter / compressor / gate", makeLayout())
        {
            sections = {
                { "Loudness", { "gain", "sat", "ceil" }, 0 },
                { "Compressor", { "mode", "thresh", "ratio", "knee" }, 0 },
                { "Envelope", { "att", "rel", "mix" }, 1 },
                { "Noise gate", { "g_thr", "g_rel", "g_range" }, 1 },
            };

            presets = {
                { "Default", {} },
                { "Vocal Leveler",        { { "mode", 1 }, { "thresh", -20 }, { "ratio", 3 }, { "att", 8 }, { "rel", 120 }, { "gain", 3 } } },
                { "Vocal Squash",         { { "mode", 1 }, { "thresh", -28 }, { "ratio", 6 }, { "att", 3 }, { "rel", 80 }, { "gain", 6 }, { "sat", 15 } } },
                { "Gentle Glue",          { { "mode", 1 }, { "thresh", -12 }, { "ratio", 2 }, { "knee", 6 }, { "att", 20 }, { "rel", 250 }, { "gain", 1.5f } } },
                { "Max Loudness",         { { "gain", 9 }, { "ceil", -0.3f }, { "rel", 60 } } },
                { "Max Loudness (warm)",  { { "gain", 9 }, { "sat", 35 }, { "ceil", -0.3f }, { "rel", 60 } } },
                { "Drum Smash",           { { "mode", 1 }, { "thresh", -30 }, { "ratio", 10 }, { "att", 1 }, { "rel", 60 }, { "gain", 8 }, { "mix", 60 } } },
                { "Parallel Crush",       { { "mode", 1 }, { "thresh", -35 }, { "ratio", 20 }, { "att", 0.5f }, { "rel", 50 }, { "gain", 10 }, { "mix", 35 } } },
                { "Noise Gate",           { { "g_thr", -50 }, { "g_range", 60 }, { "g_rel", 120 } } },
                { "Breath Tamer",         { { "mode", 1 }, { "thresh", -18 }, { "ratio", 2.5f }, { "g_thr", -42 }, { "g_range", 12 }, { "g_rel", 200 } } },
            };
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::num ("gain", "Gain", -12.0f, 24.0f, 0.0f, "dB", 1));
            l.add (AmParams::num ("sat", "Sat", 0.0f, 100.0f, 0.0f, "%", 0));
            l.add (AmParams::num ("ceil", "Ceiling", -24.0f, 0.0f, -0.3f, "dB", 1));
            l.add (AmParams::choice ("mode", "Mode", { "Limit", "Comp" }, 0));
            l.add (AmParams::num ("thresh", "Thresh", -48.0f, 0.0f, -18.0f, "dB", 1));
            l.add (AmParams::num ("ratio", "Ratio", 1.0f, 20.0f, 4.0f, ":1", 1, 4.0f));
            l.add (AmParams::num ("knee", "Knee", 0.0f, 12.0f, 3.0f, "dB", 1));
            l.add (AmParams::num ("att", "Attack", 0.05f, 100.0f, 5.0f, "ms", 2, 5.0f));
            l.add (AmParams::num ("rel", "Release", 5.0f, 1500.0f, 120.0f, "ms", 0, 150.0f));
            l.add (AmParams::num ("mix", "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
            l.add (AmParams::num ("g_thr", "Thresh", -90.0f, -10.0f, -90.0f, "dB", 1));
            l.add (AmParams::num ("g_rel", "Release", 10.0f, 1000.0f, 150.0f, "ms", 0, 150.0f));
            l.add (AmParams::num ("g_range", "Range", 0.0f, 90.0f, 40.0f, "dB", 0));
            return l;
        }

        void prepareToPlay (double sr, int) override { sampleRate = sr; env = 0.0f; gateEnv = 0.0f; gateGain = 1.0f; }

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buffer.getNumSamples();
            const int nCh = juce::jmin (2, buffer.getNumChannels());
            auto* L = buffer.getWritePointer (0);
            auto* R = nCh > 1 ? buffer.getWritePointer (1) : nullptr;

            const float inGain = juce::Decibels::decibelsToGain (get ("gain"));
            const float sat = get ("sat") * 0.01f;
            const float drive = 1.0f + sat * 4.0f, driveNorm = std::tanh (drive);
            const float ceilDb = get ("ceil"), ceiling = juce::Decibels::decibelsToGain (ceilDb);
            const bool comp = getInt ("mode") == 1;
            const float thr = comp ? get ("thresh") : ceilDb;
            const float ratio = get ("ratio"), knee = get ("knee");
            const float att = coef (get ("att"), sampleRate), rel = coef (get ("rel"), sampleRate);
            const float mix = get ("mix") * 0.01f;
            const float gThr = get ("g_thr"), gRange = get ("g_range");
            const bool gateOn = gThr > -89.5f;
            const float gAtt = coef (1.0, sampleRate), gRel = coef (get ("g_rel"), sampleRate), gOpen = coef (2.0, sampleRate);
            const float gateFloor = juce::Decibels::decibelsToGain (-gRange);

            float peakIn = 0.0f, maxGr = 0.0f;

            for (int i = 0; i < n; ++i)
            {
                float l = L[i] * inGain, r = (R != nullptr ? R[i] : L[i]) * inGain;
                if (sat > 0.0f)
                {
                    l += sat * (std::tanh (drive * l) / driveNorm - l);
                    r += sat * (std::tanh (drive * r) / driveNorm - r);
                }
                const float dryL = l, dryR = r;
                const float level = juce::jmax (std::abs (l), std::abs (r));
                peakIn = juce::jmax (peakIn, level);

                // gate
                float gg = 1.0f;
                if (gateOn)
                {
                    gateEnv = level > gateEnv ? gAtt * gateEnv + (1.0f - gAtt) * level : gRel * gateEnv + (1.0f - gRel) * level;
                    const float target = juce::Decibels::gainToDecibels (gateEnv, -120.0f) < gThr ? gateFloor : 1.0f;
                    const float c = target > gateGain ? gOpen : gRel;
                    gateGain = c * gateGain + (1.0f - c) * target;
                    gg = gateGain;
                }

                // compressor / limiter
                env = level > env ? att * env + (1.0f - att) * level : rel * env + (1.0f - rel) * level;
                const float envDb = juce::Decibels::gainToDecibels (env, -120.0f);
                const float over = envDb - thr;
                float gr = 0.0f;
                if (! comp)
                    gr = juce::jmax (0.0f, over);
                else if (2.0f * over < -knee)
                    gr = 0.0f;
                else if (2.0f * over > knee || knee < 0.01f)
                    gr = over * (1.0f - 1.0f / ratio);
                else
                    gr = (1.0f - 1.0f / ratio) * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee);

                maxGr = juce::jmax (maxGr, gr);
                const float g = juce::Decibels::decibelsToGain (-gr) * gg;

                l = dryL + mix * (l * g - dryL);
                r = dryR + mix * (r * g - dryR);
                if (mix < 1.0f && gateOn) { l *= juce::jmax (gg, mix); r *= juce::jmax (gg, mix); }

                L[i] = softCeiling (l, ceiling);
                if (R != nullptr)
                    R[i] = softCeiling (r, ceiling);
            }

            storeMax (meterIn, juce::Decibels::gainToDecibels (peakIn, -100.0f));
            storeMax (meterGr, maxGr);
        }

        std::unique_ptr<juce::Component> createVisual() override
        {
            return std::make_unique<MeterHistory> (meterIn, meterGr, [this] { return getInt ("mode") == 1 ? get ("thresh") : get ("ceil"); });
        }
        int getVisualHeight() const override { return 150; }

        std::atomic<float> meterIn { -100.0f }, meterGr { 0.0f };

    private:
        float env = 0.0f, gateEnv = 0.0f, gateGain = 1.0f;
    };

    //==========================================================================
    // Reverb: pre-delay, early reflections, lush late tail, modulation, filters, width.
    class ReverbPlugin : public AmethystPlugin
    {
    public:
        ReverbPlugin() : AmethystPlugin ("amethyst.reverb", "Geode Reverb", "Reverb", "Shimmering algorithmic space", makeLayout())
        {
            tailSeconds = 10.0;
            sections = {
                { "Space", { "predelay", "size", "diff" }, 0, 52 },
                { "Tail", { "decay", "damp", "mod", "speed" }, 0, 52 },
                { "Tone", { "lowcut", "highcut", "width" }, 1, 52 },
                { "Mix", { "dry", "er", "wet" }, 1, 52 },
            };

            presets = {
                { "Default", {} },
                { "Vocal Plate",  { { "predelay", 30 }, { "size", 45 }, { "decay", 55 }, { "damp", 35 }, { "diff", 80 }, { "lowcut", 200 }, { "highcut", 8000 }, { "wet", 28 }, { "er", 15 } } },
                { "Small Studio", { { "predelay", 5 }, { "size", 25 }, { "decay", 30 }, { "damp", 60 }, { "er", 40 }, { "wet", 22 } } },
                { "Large Hall",   { { "predelay", 40 }, { "size", 85 }, { "decay", 80 }, { "damp", 40 }, { "wet", 32 } } },
                { "Cathedral",    { { "predelay", 60 }, { "size", 100 }, { "decay", 95 }, { "damp", 25 }, { "mod", 30 }, { "wet", 38 }, { "dry", 85 } } },
                { "Drum Room",    { { "predelay", 0 }, { "size", 35 }, { "decay", 25 }, { "damp", 50 }, { "er", 60 }, { "wet", 25 } } },
                { "The Venue",    { { "predelay", 25 }, { "size", 70 }, { "decay", 60 }, { "er", 35 }, { "wet", 30 } } },
                { "Ambient Wash", { { "predelay", 80 }, { "size", 100 }, { "decay", 98 }, { "mod", 60 }, { "speed", 0.3f }, { "wet", 55 }, { "dry", 70 }, { "lowcut", 300 } } },
            };
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::num ("predelay", "Pre-delay", 0.0f, 250.0f, 20.0f, "ms", 0, 60.0f));
            l.add (AmParams::num ("size", "Size", 0.0f, 100.0f, 55.0f, "%", 0));
            l.add (AmParams::num ("diff", "Diffusion", 0.0f, 100.0f, 60.0f, "%", 0));
            l.add (AmParams::num ("decay", "Decay", 0.0f, 100.0f, 55.0f, "%", 0));
            l.add (AmParams::num ("damp", "Damping", 0.0f, 100.0f, 45.0f, "%", 0));
            l.add (AmParams::num ("mod", "Mod", 0.0f, 100.0f, 20.0f, "%", 0));
            l.add (AmParams::num ("speed", "Speed", 0.05f, 5.0f, 0.6f, "Hz", 2, 1.0f));
            l.add (AmParams::num ("lowcut", "Low cut", 20.0f, 1000.0f, 120.0f, "Hz", 0, 200.0f));
            l.add (AmParams::num ("highcut", "High cut", 1000.0f, 20000.0f, 9000.0f, "Hz", 0, 5000.0f));
            l.add (AmParams::num ("width", "Width", 0.0f, 100.0f, 100.0f, "%", 0));
            l.add (AmParams::num ("dry", "Dry", 0.0f, 100.0f, 100.0f, "%", 0));
            l.add (AmParams::num ("er", "Early", 0.0f, 100.0f, 25.0f, "%", 0));
            l.add (AmParams::num ("wet", "Wet", 0.0f, 100.0f, 30.0f, "%", 0));
            return l;
        }

        void prepareToPlay (double sr, int bs) override
        {
            sampleRate = sr;
            const int maxPre = (int) (sr * 0.3) + 2;
            preBuf.setSize (2, maxPre);
            preBuf.clear();
            erBuf.setSize (2, (int) (sr * 0.25) + 2);
            erBuf.clear();
            modBuf.setSize (2, (int) (sr * 0.03) + 4);
            modBuf.clear();
            work.setSize (4, juce::jmax (bs, 512));
            preW = erW = modW = 0;
            reverb.reset();
            reverb.setSampleRate (sr);
            lastLow = lastHigh = -1.0f;
            lfo = 0.0;
        }

        void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buffer.getNumSamples();
            const int nCh = juce::jmin (2, buffer.getNumChannels());
            if (work.getNumSamples() < n)
                work.setSize (4, n, false, false, true);

            updateFilters();

            const float size = get ("size") * 0.01f, diff = get ("diff") * 0.01f;
            const int pre = juce::jlimit (0, preBuf.getNumSamples() - 2, (int) (get ("predelay") * 0.001 * sampleRate));
            juce::Reverb::Parameters p;
            p.roomSize = 0.3f + 0.69f * get ("decay") * 0.01f;
            p.damping = get ("damp") * 0.01f;
            p.width = get ("width") * 0.01f;
            p.wetLevel = 1.0f;
            p.dryLevel = 0.0f;
            reverb.setParameters (p);

            // the late tail runs hot; scale so 100% wet sits near unity
            const float dry = get ("dry") * 0.01f, erLvl = get ("er") * 0.01f, wet = get ("wet") * 0.01f * 0.5f;
            const float modDepth = get ("mod") * 0.01f * (float) (sampleRate * 0.003);
            const double lfoInc = juce::MathConstants<double>::twoPi * get ("speed") / sampleRate;

            static constexpr float tapsMs[2][6] = { { 7.1f, 13.3f, 19.7f, 29.1f, 41.3f, 59.9f }, { 9.3f, 15.7f, 23.9f, 33.1f, 47.9f, 67.7f } };
            int taps[2][6];
            float tapGain[6];
            for (int k = 0; k < 6; ++k)
            {
                tapGain[k] = std::pow (0.78f, (float) k) * (0.35f + 0.65f * diff) * 0.55f;
                for (int c = 0; c < 2; ++c)
                    taps[c][k] = juce::jlimit (1, erBuf.getNumSamples() - 2, (int) (tapsMs[c][k] * (0.4f + 1.4f * size) * 0.001 * sampleRate));
            }

            auto* lateL = work.getWritePointer (0);
            auto* lateR = work.getWritePointer (1);
            auto* earlyL = work.getWritePointer (2);
            auto* earlyR = work.getWritePointer (3);
            const int preLen = preBuf.getNumSamples(), erLen = erBuf.getNumSamples();

            for (int i = 0; i < n; ++i)
            {
                const float inL = buffer.getSample (0, i);
                const float inR = nCh > 1 ? buffer.getSample (1, i) : inL;

                preBuf.setSample (0, preW, inL);
                preBuf.setSample (1, preW, inR);
                const int rp = (preW - pre + preLen) % preLen;
                const float pl = preBuf.getSample (0, rp), pr = preBuf.getSample (1, rp);
                preW = (preW + 1) % preLen;

                erBuf.setSample (0, erW, pl);
                erBuf.setSample (1, erW, pr);
                float el = 0.0f, er = 0.0f;
                for (int k = 0; k < 6; ++k)
                {
                    el += tapGain[k] * erBuf.getSample ((k & 1), (erW - taps[0][k] + erLen) % erLen);
                    er += tapGain[k] * erBuf.getSample (1 - (k & 1), (erW - taps[1][k] + erLen) % erLen);
                }
                erW = (erW + 1) % erLen;

                earlyL[i] = el;
                earlyR[i] = er;
                lateL[i] = pl + 0.3f * el;
                lateR[i] = pr + 0.3f * er;
            }

            reverb.processStereo (lateL, lateR, n);

            // modulation (gentle chorus on the tail)
            if (modDepth > 0.5f)
            {
                const int modLen = modBuf.getNumSamples();
                for (int i = 0; i < n; ++i)
                {
                    modBuf.setSample (0, modW, lateL[i]);
                    modBuf.setSample (1, modW, lateR[i]);
                    for (int c = 0; c < 2; ++c)
                    {
                        const double d = 2.0 + modDepth * (1.0 + std::sin (lfo + c * juce::MathConstants<double>::halfPi));
                        double pos = modW - d;
                        while (pos < 0) pos += modLen;
                        const int i0 = (int) pos;
                        const float frac = (float) (pos - i0);
                        const float a = modBuf.getSample (c, i0), b = modBuf.getSample (c, (i0 + 1) % modLen);
                        (c == 0 ? lateL : lateR)[i] = a + (b - a) * frac;
                    }
                    modW = (modW + 1) % modLen;
                    lfo += lfoInc;
                }
                if (lfo > juce::MathConstants<double>::twoPi * 1000.0) lfo = std::fmod (lfo, juce::MathConstants<double>::twoPi);
            }

            for (int c = 0; c < 2; ++c)
            {
                lowCut[(size_t) c].processSamples (work.getWritePointer (c), n);
                highCut[(size_t) c].processSamples (work.getWritePointer (c), n);
                lowCutE[(size_t) c].processSamples (work.getWritePointer (c + 2), n);
                highCutE[(size_t) c].processSamples (work.getWritePointer (c + 2), n);
            }

            for (int c = 0; c < nCh; ++c)
            {
                auto* d = buffer.getWritePointer (c);
                const auto* late = work.getReadPointer (c);
                const auto* early = work.getReadPointer (c + 2);
                for (int i = 0; i < n; ++i)
                    d[i] = d[i] * dry + early[i] * erLvl + late[i] * wet;
            }

            storeMax (activity, wet * buffer.getMagnitude (0, n));
        }

        std::unique_ptr<juce::Component> createVisual() override;
        int getVisualHeight() const override { return 170; }

        std::atomic<float> activity { 0.0f };

    private:
        void updateFilters()
        {
            const float lc = get ("lowcut"), hc = juce::jmin ((float) (sampleRate * 0.45), get ("highcut"));
            if (lc == lastLow && hc == lastHigh)
                return;
            lastLow = lc; lastHigh = hc;
            const auto hp = juce::IIRCoefficients::makeHighPass (sampleRate, lc);
            const auto lp = juce::IIRCoefficients::makeLowPass (sampleRate, hc);
            for (int c = 0; c < 2; ++c)
            {
                lowCut[(size_t) c].setCoefficients (hp);  highCut[(size_t) c].setCoefficients (lp);
                lowCutE[(size_t) c].setCoefficients (hp); highCutE[(size_t) c].setCoefficients (lp);
            }
        }

        juce::Reverb reverb;
        juce::AudioBuffer<float> preBuf, erBuf, modBuf, work;
        int preW = 0, erW = 0, modW = 0;
        double lfo = 0.0;
        std::array<juce::IIRFilter, 2> lowCut, highCut, lowCutE, highCutE;
        float lastLow = -1.0f, lastHigh = -1.0f;
    };

    // Rotating wireframe gem: width follows Size, height follows Decay, glow follows the wet signal.
    class GeodeVisual : public juce::Component, private juce::Timer
    {
    public:
        explicit GeodeVisual (ReverbPlugin& p) : plugin (p) { startTimerHz (30); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            Glass::well (g, r, 12.0f);

            const float size = plugin.get ("size") * 0.01f, decay = plugin.get ("decay") * 0.01f;
            const float mod = plugin.get ("mod") * 0.01f;
            const auto c = r.getCentre();
            const float radius = r.getHeight() * (0.18f + 0.22f * size);
            const float halfH = r.getHeight() * (0.16f + 0.24f * decay);

            juce::ColourGradient halo (Theme::accent.withAlpha (0.12f + 0.5f * glow), c.x, c.y,
                                       Theme::accent.withAlpha (0.0f), c.x + radius * 2.2f, c.y, true);
            g.setGradientFill (halo);
            g.fillEllipse (juce::Rectangle<float> (radius * 4.4f, radius * 4.4f).withCentre (c));

            constexpr int N = 10;
            auto project = [&] (float x, float y, float z)
            {
                const float persp = 1.0f / (1.0f + z * 0.0025f);
                return juce::Point<float> (c.x + x * persp, c.y + y * persp);
            };

            juce::Point<float> ring[N], innerRing[N];
            float depth[N];
            const auto top = project (0.0f, -halfH, 0.0f), bottom = project (0.0f, halfH, 0.0f);
            for (int i = 0; i < N; ++i)
            {
                const float a = angle + juce::MathConstants<float>::twoPi * (float) i / N;
                const float wob = 1.0f + mod * 0.08f * std::sin (angle * 3.0f + (float) i);
                const float x = std::cos (a) * radius * wob, z = std::sin (a) * radius * wob;
                ring[i] = project (x, 0.0f, z);
                innerRing[i] = project (x * 0.55f, -halfH * 0.55f, z * 0.55f);
                depth[i] = 0.35f + 0.65f * (0.5f - 0.5f * std::sin (a));
            }

            for (int i = 0; i < N; ++i)
            {
                const auto col = Theme::wave.interpolatedWith (Theme::accent2, (float) i / N).withAlpha (0.25f + 0.6f * depth[i]);
                g.setColour (col);
                g.drawLine ({ ring[i], ring[(i + 1) % N] }, 1.4f);
                g.drawLine ({ ring[i], top }, 1.0f);
                g.drawLine ({ ring[i], bottom }, 1.0f);
                g.setColour (col.withMultipliedAlpha (0.5f));
                g.drawLine ({ innerRing[i], innerRing[(i + 1) % N] }, 1.0f);
            }

            g.setColour (Theme::textDim);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("SIZE " + juce::String ((int) (size * 100)) + "%   DECAY " + juce::String ((int) (decay * 100)) + "%",
                        r.reduced (12.0f, 8.0f).toNearestInt(), juce::Justification::bottomLeft);
        }

    private:
        void timerCallback() override
        {
            angle += 0.012f + 0.03f * plugin.get ("speed") * plugin.get ("mod") * 0.01f;
            glow = juce::jmax (plugin.activity.exchange (0.0f) * 2.0f, glow * 0.92f);
            glow = juce::jmin (glow, 1.0f);
            repaint();
        }

        ReverbPlugin& plugin;
        float angle = 0.0f, glow = 0.0f;
    };

    std::unique_ptr<juce::Component> ReverbPlugin::createVisual() { return std::make_unique<GeodeVisual> (*this); }
}

std::unique_ptr<AmethystPlugin> Builtins::makeDynamics() { return std::make_unique<DynamicsPlugin>(); }
std::unique_ptr<AmethystPlugin> Builtins::makeReverb()   { return std::make_unique<ReverbPlugin>(); }
