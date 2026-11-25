#include "Builtins.h"

namespace
{
    float coef (double timeMs, double sr) { return (float) std::exp (-1.0 / (juce::jmax (0.01, timeMs) * 0.001 * sr)); }

    //==========================================================================
    // Dynamics: saturating pre-gain, limiter or compressor, noise gate, parallel mix, safety ceiling.
    juce::StringArray sidechainChoices()
    {
        juce::StringArray s { "Off (own signal)" };
        for (int i = 1; i <= 32; ++i)
            s.add ("Track " + juce::String (i));
        return s;
    }

    // Prism Dynamics
    //  Compressor / limiter: the static curve gives a target gain reduction per sample, and that
    //  *gain reduction* is smoothed (attack when it grows, release when it falls) in dB. Smoothing the
    //  gain rather than the level keeps fast settings clean. RMS or peak detection, soft knee,
    //  three characters, auto makeup.
    //  Gate: hysteresis (opens at the threshold, closes a few dB lower), hold, a quick smooth open and a
    //  release that eases down in dB. Its detector ignores rumble below ~80 Hz.
    class DynamicsPlugin : public AmethystPlugin
    {
    public:
        DynamicsPlugin() : AmethystPlugin ("amethyst.dynamics", "Prism Dynamics", "Dynamics", "Comp / limit / gate / sidechain", makeLayout())
        {
            sections = {
                { "Loudness", { "gain", "sat", "ceil" }, 0 },
                { "Compressor", { "mode", "character", "thresh", "ratio", "knee" }, 0 },
                { "Envelope", { "detect", "auto_mk", "att", "rel", "mix" }, 1 },
                { "Noise gate", { "g_thr", "g_range", "g_hold", "g_rel" }, 1 },
                { "Sidechain", { "sc_track", "sc_hpf" }, 2 },
                { "Multiband", { "bands", "x_low", "x_high", "amt_low", "amt_mid", "amt_high" }, 2 },
            };

            presets = {
                { "Default", {} },
                { "Vocal Leveler",        { { "mode", 1 }, { "character", 1 }, { "detect", 1 }, { "thresh", -20 }, { "ratio", 3 }, { "att", 10 }, { "rel", 150 }, { "auto_mk", 1 } } },
                { "Vocal Squash",         { { "mode", 1 }, { "character", 2 }, { "thresh", -28 }, { "ratio", 6 }, { "att", 3 }, { "rel", 90 }, { "gain", 2 }, { "auto_mk", 1 } } },
                { "Gentle Glue",          { { "mode", 1 }, { "character", 1 }, { "detect", 1 }, { "thresh", -14 }, { "ratio", 2 }, { "knee", 8 }, { "att", 25 }, { "rel", 250 }, { "auto_mk", 1 } } },
                { "Vintage Vocal",        { { "mode", 1 }, { "character", 2 }, { "detect", 1 }, { "thresh", -22 }, { "ratio", 4 }, { "knee", 6 }, { "att", 12 }, { "rel", 180 }, { "auto_mk", 1 } } },
                { "Max Loudness",         { { "gain", 9 }, { "ceil", -0.3f }, { "rel", 60 } } },
                { "Max Loudness (warm)",  { { "gain", 9 }, { "sat", 35 }, { "ceil", -0.3f }, { "rel", 60 } } },
                { "Drum Smash",           { { "mode", 1 }, { "thresh", -30 }, { "ratio", 10 }, { "att", 1 }, { "rel", 60 }, { "gain", 4 }, { "mix", 60 }, { "auto_mk", 1 } } },
                { "Parallel Crush",       { { "mode", 1 }, { "character", 2 }, { "thresh", -35 }, { "ratio", 20 }, { "att", 0.5f }, { "rel", 50 }, { "mix", 35 }, { "auto_mk", 1 } } },
                { "Noise Gate (gentle)",  { { "g_thr", -48 }, { "g_range", 18 }, { "g_hold", 80 }, { "g_rel", 200 } } },
                { "Noise Gate (tight)",   { { "g_thr", -42 }, { "g_range", 40 }, { "g_hold", 40 }, { "g_rel", 120 } } },
                { "Breath Tamer",         { { "mode", 1 }, { "character", 1 }, { "detect", 1 }, { "thresh", -18 }, { "ratio", 2.5f }, { "auto_mk", 1 },
                                            { "g_thr", -45 }, { "g_range", 10 }, { "g_hold", 60 }, { "g_rel", 250 } } },
                { "Duck Under Vocals (set sidechain)", { { "mode", 1 }, { "detect", 1 }, { "thresh", -30 }, { "ratio", 4 }, { "att", 10 }, { "rel", 220 }, { "sc_track", 1 } } },
                { "Kick Pump (set sidechain)",         { { "mode", 1 }, { "thresh", -32 }, { "ratio", 8 }, { "att", 1 }, { "rel", 160 } } },
                { "Multiband Vocal",      { { "mode", 1 }, { "bands", 1 }, { "detect", 1 }, { "thresh", -22 }, { "ratio", 3 }, { "att", 6 }, { "rel", 120 }, { "auto_mk", 1 },
                                            { "x_low", 180 }, { "x_high", 4500 }, { "amt_low", 70 }, { "amt_mid", 100 }, { "amt_high", 60 } } },
                { "Multiband Master Glue", { { "mode", 1 }, { "bands", 1 }, { "character", 1 }, { "detect", 1 }, { "thresh", -14 }, { "ratio", 2 }, { "knee", 6 },
                                            { "att", 25 }, { "rel", 250 }, { "amt_low", 100 }, { "amt_mid", 60 }, { "amt_high", 80 }, { "auto_mk", 1 } } },
            };
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::num ("gain", "Gain", -12.0f, 24.0f, 0.0f, "dB", 1));
            l.add (AmParams::num ("sat", "Sat", 0.0f, 100.0f, 0.0f, "%", 0));
            l.add (AmParams::num ("ceil", "Ceiling", -24.0f, 0.0f, -0.3f, "dB", 1));
            l.add (AmParams::choice ("mode", "Mode", { "Limit", "Comp" }, 0));
            l.add (AmParams::choice ("character", "Character", { "Clean", "Smooth", "Vintage" }, 1));
            l.add (AmParams::num ("thresh", "Thresh", -48.0f, 0.0f, -18.0f, "dB", 1));
            l.add (AmParams::num ("ratio", "Ratio", 1.0f, 20.0f, 4.0f, ":1", 1, 4.0f));
            l.add (AmParams::num ("knee", "Knee", 0.0f, 12.0f, 4.0f, "dB", 1));
            l.add (AmParams::choice ("detect", "Detect", { "Peak", "RMS" }, 1));
            l.add (AmParams::toggle ("auto_mk", "Auto gain", false));
            l.add (AmParams::num ("att", "Attack", 0.05f, 100.0f, 8.0f, "ms", 2, 5.0f));
            l.add (AmParams::num ("rel", "Release", 5.0f, 1500.0f, 150.0f, "ms", 0, 150.0f));
            l.add (AmParams::num ("mix", "Mix", 0.0f, 100.0f, 100.0f, "%", 0));
            l.add (AmParams::num ("g_thr", "Thresh", -90.0f, -10.0f, -90.0f, "dB", 1));
            l.add (AmParams::num ("g_range", "Range", 0.0f, 80.0f, 20.0f, "dB", 0));
            l.add (AmParams::num ("g_hold", "Hold", 0.0f, 500.0f, 60.0f, "ms", 0, 80.0f));
            l.add (AmParams::num ("g_rel", "Release", 10.0f, 1000.0f, 200.0f, "ms", 0, 150.0f));
            l.add (AmParams::choice ("sc_track", "Listen to", sidechainChoices(), 0));
            l.add (AmParams::num ("sc_hpf", "SC low cut", 0.0f, 500.0f, 0.0f, "Hz", 0, 120.0f));
            l.add (AmParams::choice ("bands", "Bands", { "Single", "3-band" }, 0));
            l.add (AmParams::num ("x_low", "Low / mid", 60.0f, 1000.0f, 200.0f, "Hz", 0, 250.0f));
            l.add (AmParams::num ("x_high", "Mid / high", 1000.0f, 12000.0f, 3000.0f, "Hz", 0, 3500.0f));
            l.add (AmParams::num ("amt_low", "Low", 0.0f, 100.0f, 100.0f, "%", 0));
            l.add (AmParams::num ("amt_mid", "Mid", 0.0f, 100.0f, 100.0f, "%", 0));
            l.add (AmParams::num ("amt_high", "High", 0.0f, 100.0f, 100.0f, "%", 0));
            return l;
        }

        void prepareToPlay (double sr, int bs) override
        {
            sampleRate = sr;
            grDb = 0.0f; grSlow = 0.0f; rms = 0.0f;
            bandGr.fill (0.0f); bandRms.fill (0.0f);
            gateOpen = false; gateHold = 0; gateGainDb = 0.0f; gateEnv = 0.0f;
            gateHp.fill (0.0f);
            juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, bs), 2 };
            for (auto* f : { &lowSplit, &highSplit, &scLowSplit, &scHighSplit })
            {
                f->prepare (spec);
                f->setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
                f->reset();
            }
            scHp.fill (0.0f);
        }

        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buffer.getNumSamples();
            const int nCh = juce::jmin (2, buffer.getNumChannels());
            auto* L = buffer.getWritePointer (0);
            auto* R = nCh > 1 ? buffer.getWritePointer (1) : nullptr;
            const double sr = sampleRate;

            const float inGain = juce::Decibels::decibelsToGain (get ("gain"));
            const float sat = get ("sat") * 0.01f;
            const float drive = 1.0f + sat * 4.0f, driveNorm = std::tanh (drive);
            const float ceilDb = get ("ceil"), ceiling = juce::Decibels::decibelsToGain (ceilDb);
            const bool comp = getInt ("mode") == 1;
            const int character = comp ? getInt ("character") : 0;
            const bool useRms = comp && getInt ("detect") == 1;
            const float thr = comp ? get ("thresh") : ceilDb;
            const float ratio = comp ? get ("ratio") : 100.0f;
            const float knee = comp ? get ("knee") : 0.0f;
            const float attMs = comp ? get ("att") : juce::jmin (get ("att"), 1.0f);
            const float att = coef (attMs, sr), rel = coef (get ("rel"), sr);
            const float relSlowC = coef (get ("rel") * 4.0, sr);
            const float grSlowC = coef (600.0, sr);
            const float rmsC = coef (8.0, sr);
            const float mix = get ("mix") * 0.01f;
            // auto makeup: about half the reduction the curve applies 12 dB over the threshold region
            const float autoMk = comp && getBool ("auto_mk") ? juce::jlimit (0.0f, 24.0f, 0.5f * (-thr) * (1.0f - 1.0f / ratio)) : 0.0f;
            const float makeup = juce::Decibels::decibelsToGain (autoMk);

            // gate
            const float gThr = get ("g_thr");
            const bool gateOn = gThr > -89.5f;
            const float gRange = get ("g_range");
            const int holdSamples = (int) (get ("g_hold") * 0.001 * sr);
            const float gOpenC = coef (1.5, sr);                                   // ~1.5 ms smooth open
            const float gCloseStep = gRange / (float) juce::jmax (1.0, get ("g_rel") * 0.001 * sr);   // dB per sample
            const float gEnvAtt = coef (0.3, sr), gEnvRel = coef (12.0, sr);
            const float gHpA = (float) std::exp (-juce::MathConstants<double>::twoPi * 80.0 / sr);
            constexpr float hysteresis = 6.0f;

            // sidechain
            const float* scL = nullptr;
            const float* scR = nullptr;
            const int scTrack = getInt ("sc_track");
            bool useSc = false;
            if (scTrack > 0 && sidechainSource())
                useSc = sidechainSource() (scTrack, n, scL, scR);
            sidechainActive.store (useSc);
            const float scHpf = get ("sc_hpf");
            const float hpA = scHpf > 1.0f ? (float) std::exp (-juce::MathConstants<double>::twoPi * scHpf / sr) : 0.0f;

            const bool multi = getInt ("bands") == 1;
            if (multi)
            {
                const float xl = get ("x_low"), xh = juce::jmax (xl * 1.5f, get ("x_high"));
                lowSplit.setCutoffFrequency (xl);   scLowSplit.setCutoffFrequency (xl);
                highSplit.setCutoffFrequency (xh);  scHighSplit.setCutoffFrequency (xh);
            }
            const float amt[3] = { get ("amt_low") * 0.01f, get ("amt_mid") * 0.01f, get ("amt_high") * 0.01f };

            // static curve with soft knee
            auto curve = [&] (float levelDb)
            {
                const float over = levelDb - thr;
                const float slope = 1.0f - 1.0f / ratio;
                if (knee < 0.01f)          return over > 0.0f ? over * slope : 0.0f;
                if (2.0f * over < -knee)   return 0.0f;
                if (2.0f * over > knee)    return over * slope;
                const float x = over + knee * 0.5f;
                return slope * x * x / (2.0f * knee);
            };

            // gain-reduction smoothing; Smooth / Vintage release slower after long compression
            auto smoothGr = [&] (float& state, float target)
            {
                if (target > state)
                    state = att * state + (1.0f - att) * target;
                else
                {
                    float r = rel;
                    if (character >= 1)
                    {
                        const float sustained = juce::jlimit (0.0f, 1.0f, grSlow / 6.0f);
                        r = rel + (relSlowC - rel) * sustained;
                    }
                    state = r * state + (1.0f - r) * target;
                }
                return state;
            };

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

                // ---- gate ----
                float gg = 1.0f;
                if (gateOn)
                {
                    // detector: own signal with the rumble filtered out
                    gateHp[0] = gHpA * gateHp[0] + (1.0f - gHpA) * l;
                    gateHp[1] = gHpA * gateHp[1] + (1.0f - gHpA) * r;
                    const float det = juce::jmax (std::abs (l - gateHp[0]), std::abs (r - gateHp[1]));
                    gateEnv = det > gateEnv ? gEnvAtt * gateEnv + (1.0f - gEnvAtt) * det : gEnvRel * gateEnv + (1.0f - gEnvRel) * det;
                    const float envDb = juce::Decibels::gainToDecibels (gateEnv, -120.0f);

                    if (envDb > gThr)                         { gateOpen = true; gateHold = holdSamples; }
                    else if (gateOpen && envDb > gThr - hysteresis) gateHold = holdSamples;   // still in the word: stay open
                    else if (gateHold > 0)                    --gateHold;
                    else                                      gateOpen = false;

                    if (gateOpen) gateGainDb = gOpenC * gateGainDb;                          // ease up to 0 dB
                    else          gateGainDb = juce::jmax (-gRange, gateGainDb - gCloseStep); // glide down in dB
                    gg = juce::Decibels::decibelsToGain (gateGainDb);
                }

                // ---- detector for the compressor ----
                float dl = l, dr = r;
                if (useSc)
                {
                    dl = scL[i]; dr = scR[i];
                    if (hpA > 0.0f)
                    {
                        scHp[0] = hpA * scHp[0] + (1.0f - hpA) * dl; dl -= scHp[0];
                        scHp[1] = hpA * scHp[1] + (1.0f - hpA) * dr; dr -= scHp[1];
                    }
                }

                if (! multi)
                {
                    float detLevel;
                    if (useRms)
                    {
                        const float sq = 0.5f * (dl * dl + dr * dr);
                        rms = rmsC * rms + (1.0f - rmsC) * sq;
                        detLevel = std::sqrt (rms) * 1.41f;   // sine RMS read as its peak, so thresholds feel the same
                    }
                    else
                        detLevel = juce::jmax (std::abs (dl), std::abs (dr));

                    const float target = curve (juce::Decibels::gainToDecibels (detLevel, -120.0f));
                    const float gr = smoothGr (grDb, target);
                    grSlow = grSlowC * grSlow + (1.0f - grSlowC) * gr;
                    maxGr = juce::jmax (maxGr, gr);
                    const float g = juce::Decibels::decibelsToGain (-gr) * makeup;
                    float cl = l * g, cr = r * g;
                    if (character == 2 && gr > 0.05f)   // vintage: a little warmth that grows with compression
                    {
                        const float warm = juce::jmin (0.35f, gr / 30.0f);
                        cl += warm * (std::tanh (1.6f * cl) / std::tanh (1.6f) - cl);
                        cr += warm * (std::tanh (1.6f * cr) / std::tanh (1.6f) - cr);
                    }
                    l = dryL + mix * (cl * gg - dryL);
                    r = dryR + mix * (cr * gg - dryR);
                }
                else
                {
                    float aB[2][3], dB[2][3];
                    const float ins[2] = { l, r }, dets[2] = { dl, dr };
                    for (int c = 0; c < 2; ++c)
                    {
                        float low, rest, mid, high;
                        lowSplit.processSample (c, ins[c], low, rest);
                        highSplit.processSample (c, rest, mid, high);
                        aB[c][0] = low; aB[c][1] = mid; aB[c][2] = high;
                        scLowSplit.processSample (c, dets[c], low, rest);
                        scHighSplit.processSample (c, rest, mid, high);
                        dB[c][0] = low; dB[c][1] = mid; dB[c][2] = high;
                    }
                    float outL = 0.0f, outR = 0.0f, grMax = 0.0f;
                    for (int k = 0; k < 3; ++k)
                    {
                        float detLevel;
                        if (useRms)
                        {
                            const float sq = 0.5f * (dB[0][k] * dB[0][k] + dB[1][k] * dB[1][k]);
                            bandRms[(size_t) k] = rmsC * bandRms[(size_t) k] + (1.0f - rmsC) * sq;
                            detLevel = std::sqrt (bandRms[(size_t) k]) * 1.41f;
                        }
                        else
                            detLevel = juce::jmax (std::abs (dB[0][k]), std::abs (dB[1][k]));
                        const float gr = smoothGr (bandGr[(size_t) k], curve (juce::Decibels::gainToDecibels (detLevel, -120.0f)) * amt[k]);
                        grMax = juce::jmax (grMax, gr);
                        const float g = juce::Decibels::decibelsToGain (-gr);
                        outL += aB[0][k] * g;
                        outR += aB[1][k] * g;
                    }
                    grSlow = grSlowC * grSlow + (1.0f - grSlowC) * grMax;
                    maxGr = juce::jmax (maxGr, grMax);
                    l = dryL + mix * (outL * makeup * gg - dryL);
                    r = dryR + mix * (outR * makeup * gg - dryR);
                }
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
        std::atomic<bool> sidechainActive { false };

    private:
        float grDb = 0.0f, grSlow = 0.0f, rms = 0.0f;
        std::array<float, 3> bandGr {}, bandRms {};
        bool gateOpen = false;
        int gateHold = 0;
        float gateGainDb = 0.0f, gateEnv = 0.0f;
        std::array<float, 2> gateHp {}, scHp {};
        juce::dsp::LinkwitzRileyFilter<float> lowSplit, highSplit, scLowSplit, scHighSplit;
    };

    //==========================================================================
    // Geode Reverb: an 8-line feedback delay network with input diffusion, early reflections,
    // per-line modulation, frequency-dependent decay (bass multiplier / crossover / high damping),
    // four space styles and ducking while the dry signal is loud.
    struct SpaceStyle
    {
        const char* name;
        float lengthScale, erLevel, erSpread, diffusionBoost, modScale, brightness;
    };
    const SpaceStyle kSpaces[] = {
        { "Room",    0.55f, 1.00f, 0.6f, 0.00f, 0.6f, 0.9f },
        { "Hall",    1.25f, 0.55f, 1.3f, 0.10f, 1.2f, 0.8f },
        { "Plate",   0.80f, 0.10f, 0.4f, 0.35f, 0.8f, 1.2f },
        { "Chamber", 0.90f, 0.75f, 0.9f, 0.15f, 0.9f, 1.0f },
    };

    struct ModDelay
    {
        std::vector<float> buf;
        int w = 0;
        void setMax (int n) { buf.assign ((size_t) juce::jmax (8, n), 0.0f); w = 0; }
        void clear() { std::fill (buf.begin(), buf.end(), 0.0f); }
        float read (double delay) const
        {
            const int len = (int) buf.size();
            delay = juce::jlimit (1.0, (double) len - 3.0, delay);
            double pos = w - delay;
            if (pos < 0) pos += len;
            const int i0 = (int) pos;
            const float f = (float) (pos - i0);
            const float a = buf[(size_t) i0], b = buf[(size_t) ((i0 + 1) % len)];
            return a + (b - a) * f;
        }
        void push (float x) { buf[(size_t) w] = x; if (++w >= (int) buf.size()) w = 0; }
    };

    struct Allpass
    {
        std::vector<float> buf;
        int w = 0, len = 1;
        void setMax (int n) { buf.assign ((size_t) juce::jmax (4, n), 0.0f); w = 0; }
        float process (float x, int delay, float g)
        {
            len = juce::jlimit (1, (int) buf.size() - 1, delay);
            int r = w - len;
            if (r < 0) r += (int) buf.size();
            const float d = buf[(size_t) r];
            const float v = x + g * d;
            buf[(size_t) w] = v;
            if (++w >= (int) buf.size()) w = 0;
            return d - g * v;
        }
    };

    class ReverbPlugin : public AmethystPlugin
    {
    public:
        ReverbPlugin() : AmethystPlugin ("amethyst.reverb", "Geode Reverb", "Reverb", "Lush 8-line algorithmic space", makeLayout())
        {
            tailSeconds = 20.0;
            sections = {
                { "Space", { "type", "predelay", "size", "diff" }, 0, 50 },
                { "Decay", { "time", "bass", "xover", "hidamp" }, 0, 50 },
                { "Motion", { "mod", "speed", "duck" }, 1, 50 },
                { "Tone", { "lowcut", "highcut", "width" }, 1, 50 },
                { "Mix", { "dry", "er", "wet" }, 1, 50 },
            };

            presets = {
                { "Default", {} },
                { "Vocal Plate",   { { "type", 2 }, { "predelay", 25 }, { "size", 45 }, { "time", 1.6 }, { "hidamp", 7000 }, { "diff", 85 }, { "lowcut", 200 }, { "highcut", 9000 }, { "wet", 26 }, { "er", 10 }, { "duck", 25 } } },
                { "Small Studio",  { { "type", 0 }, { "predelay", 5 }, { "size", 25 }, { "time", 0.5 }, { "hidamp", 5000 }, { "er", 45 }, { "wet", 20 } } },
                { "Large Hall",    { { "type", 1 }, { "predelay", 35 }, { "size", 85 }, { "time", 2.8 }, { "hidamp", 5500 }, { "wet", 30 } } },
                { "Cathedral",     { { "type", 1 }, { "predelay", 60 }, { "size", 100 }, { "time", 6.5 }, { "bass", 1.4 }, { "hidamp", 4000 }, { "mod", 35 }, { "wet", 36 }, { "dry", 85 } } },
                { "Drum Room",     { { "type", 0 }, { "predelay", 0 }, { "size", 35 }, { "time", 0.7 }, { "er", 60 }, { "wet", 24 } } },
                { "Chamber Vocal", { { "type", 3 }, { "predelay", 20 }, { "size", 55 }, { "time", 1.4 }, { "hidamp", 6500 }, { "wet", 24 }, { "duck", 35 } } },
                { "Ducked Vocal Hall", { { "type", 1 }, { "predelay", 40 }, { "size", 75 }, { "time", 2.4 }, { "wet", 34 }, { "duck", 70 } } },
                { "Ambient Wash",  { { "type", 1 }, { "predelay", 80 }, { "size", 100 }, { "time", 12.0 }, { "mod", 60 }, { "speed", 0.3f }, { "wet", 55 }, { "dry", 70 }, { "lowcut", 300 } } },
            };
        }

        static Layout makeLayout()
        {
            Layout l;
            l.add (AmParams::choice ("type", "Space", { "Room", "Hall", "Plate", "Chamber" }, 1));
            l.add (AmParams::num ("predelay", "Pre-delay", 0.0f, 250.0f, 20.0f, "ms", 0, 60.0f));
            l.add (AmParams::num ("size", "Size", 0.0f, 100.0f, 55.0f, "%", 0));
            l.add (AmParams::num ("diff", "Diffusion", 0.0f, 100.0f, 75.0f, "%", 0));
            l.add (AmParams::num ("time", "Decay", 0.1f, 20.0f, 1.8f, "s", 2, 2.0f));
            l.add (AmParams::num ("bass", "Bass x", 0.25f, 2.5f, 1.0f, "x", 2, 1.0f));
            l.add (AmParams::num ("xover", "Crossover", 100.0f, 2000.0f, 500.0f, "Hz", 0, 500.0f));
            l.add (AmParams::num ("hidamp", "High damp", 1000.0f, 20000.0f, 6000.0f, "Hz", 0, 5000.0f));
            l.add (AmParams::num ("mod", "Mod", 0.0f, 100.0f, 25.0f, "%", 0));
            l.add (AmParams::num ("speed", "Speed", 0.05f, 5.0f, 0.6f, "Hz", 2, 1.0f));
            l.add (AmParams::num ("duck", "Duck", 0.0f, 100.0f, 0.0f, "%", 0));
            l.add (AmParams::num ("lowcut", "Low cut", 20.0f, 1000.0f, 120.0f, "Hz", 0, 200.0f));
            l.add (AmParams::num ("highcut", "High cut", 1000.0f, 20000.0f, 12000.0f, "Hz", 0, 5000.0f));
            l.add (AmParams::num ("width", "Width", 0.0f, 100.0f, 100.0f, "%", 0));
            l.add (AmParams::num ("dry", "Dry", 0.0f, 100.0f, 100.0f, "%", 0));
            l.add (AmParams::num ("er", "Early", 0.0f, 100.0f, 25.0f, "%", 0));
            l.add (AmParams::num ("wet", "Wet", 0.0f, 100.0f, 28.0f, "%", 0));
            return l;
        }

        void prepareToPlay (double sr, int bs) override
        {
            sampleRate = sr;
            preBuf.setSize (2, (int) (sr * 0.3) + 2);
            preBuf.clear();
            erBuf.setSize (2, (int) (sr * 0.3) + 2);
            erBuf.clear();
            for (auto& d : lines) d.setMax ((int) (sr * 0.25));   // 8 lines, up to 250 ms each
            for (auto& a : diffusers) a.setMax ((int) (sr * 0.05));
            work.setSize (4, juce::jmax (bs, 512));
            preW = erW = 0;
            lowState.fill (0.0f);
            dampState.fill (0.0f);
            lastLow = lastHigh = -1.0f;
            lfo = 0.0;
            duckEnv = 0.0f;
            duckGain = 1.0f;
        }

        void process (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
        {
            juce::ScopedNoDenormals noDenormals;
            const int n = buffer.getNumSamples();
            const int nCh = juce::jmin (2, buffer.getNumChannels());
            if (work.getNumSamples() < n)
                work.setSize (4, n, false, false, true);
            updateFilters();

            const auto& space = kSpaces[juce::jlimit (0, 3, getInt ("type"))];
            const float size = get ("size") * 0.01f;
            const float diff = juce::jlimit (0.0f, 1.0f, get ("diff") * 0.01f + space.diffusionBoost);
            const double rt60 = juce::jmax (0.1f, get ("time")) * 1.22;   // calibrated: the damping filters shorten the measured tail by ~20%
            const float bassMult = get ("bass");
            const float xA = (float) std::exp (-juce::MathConstants<double>::twoPi * get ("xover") / sampleRate);
            const float dA = (float) std::exp (-juce::MathConstants<double>::twoPi * juce::jmin (get ("hidamp") * space.brightness, (float) (sampleRate * 0.45)) / sampleRate);
            const int pre = juce::jlimit (0, preBuf.getNumSamples() - 2, (int) (get ("predelay") * 0.001 * sampleRate));
            const float width = get ("width") * 0.01f;
            const float dry = get ("dry") * 0.01f, erLvl = get ("er") * 0.01f * space.erLevel, wet = get ("wet") * 0.01f;
            const float duck = get ("duck") * 0.01f;
            const double modDepth = get ("mod") * 0.01f * space.modScale * sampleRate * 0.0012;
            const double lfoInc = juce::MathConstants<double>::twoPi * get ("speed") / sampleRate;

            // delay lengths (mutually prime-ish), scaled by style and size
            static constexpr double baseMs[8] = { 31.3, 37.9, 41.1, 43.7, 47.9, 53.3, 59.9, 67.1 };
            const double scale = space.lengthScale * (0.35 + 1.4 * size);
            double len[8];
            float gLow[8], gHigh[8];
            for (int k = 0; k < 8; ++k)
            {
                len[k] = juce::jlimit (8.0, (double) lines[(size_t) k].buf.size() - 8.0, baseMs[k] * scale * 0.001 * sampleRate);
                // gain per pass so the tail falls 60 dB in rt60 seconds (bass decays bassMult times slower)
                gHigh[k] = (float) std::pow (10.0, -3.0 * len[k] / (rt60 * sampleRate));
                gLow[k]  = (float) std::pow (10.0, -3.0 * len[k] / (rt60 * bassMult * sampleRate));
            }

            static constexpr float apMs[4] = { 4.77f, 3.59f, 12.73f, 9.31f };
            int ap[4];
            for (int k = 0; k < 4; ++k)
                ap[k] = juce::jmax (1, (int) (apMs[k] * (0.5 + 0.8 * size) * 0.001 * sampleRate));
            const float apG = 0.45f + 0.3f * diff;

            static constexpr float tapsMs[2][8] = { { 5.1f, 9.7f, 14.3f, 21.1f, 27.9f, 36.7f, 47.3f, 61.9f }, { 6.3f, 11.9f, 16.7f, 23.9f, 31.1f, 40.3f, 52.7f, 67.3f } };
            int taps[2][8];
            float tapGain[8];
            for (int k = 0; k < 8; ++k)
            {
                tapGain[k] = std::pow (0.8f, (float) k) * 0.45f;
                for (int c = 0; c < 2; ++c)
                    taps[c][k] = juce::jlimit (1, erBuf.getNumSamples() - 2, (int) (tapsMs[c][k] * space.erSpread * (0.4f + 1.2f * size) * 0.001 * sampleRate));
            }

            const float duckAtt = coef (8.0, sampleRate), duckRel = coef (350.0, sampleRate);
            auto* lateL = work.getWritePointer (0);
            auto* lateR = work.getWritePointer (1);
            auto* earlyL = work.getWritePointer (2);
            auto* earlyR = work.getWritePointer (3);
            const int preLen = preBuf.getNumSamples(), erLen = erBuf.getNumSamples();
            float duckShown = 1.0f;

            for (int i = 0; i < n; ++i)
            {
                const float inL = buffer.getSample (0, i);
                const float inR = nCh > 1 ? buffer.getSample (1, i) : inL;

                // ducking follows the dry signal
                const float lvl = juce::jmax (std::abs (inL), std::abs (inR));
                duckEnv = lvl > duckEnv ? duckAtt * duckEnv + (1.0f - duckAtt) * lvl : duckRel * duckEnv + (1.0f - duckRel) * lvl;
                if (duck > 0.0f)
                {
                    const float amount = juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (duckEnv, -100.0f) + 45.0f) / 30.0f);
                    duckGain = juce::Decibels::decibelsToGain (-18.0f * duck * amount);
                    duckShown = juce::jmin (duckShown, duckGain);
                }
                else duckGain = 1.0f;

                // pre-delay
                preBuf.setSample (0, preW, inL);
                preBuf.setSample (1, preW, inR);
                const int rp = (preW - pre + preLen) % preLen;
                const float pl = preBuf.getSample (0, rp), pr = preBuf.getSample (1, rp);
                preW = (preW + 1) % preLen;

                // early reflections
                erBuf.setSample (0, erW, pl);
                erBuf.setSample (1, erW, pr);
                float el = 0.0f, er = 0.0f;
                for (int k = 0; k < 8; ++k)
                {
                    el += tapGain[k] * erBuf.getSample ((k & 1), (erW - taps[0][k] + erLen) % erLen);
                    er += tapGain[k] * erBuf.getSample (1 - (k & 1), (erW - taps[1][k] + erLen) % erLen);
                }
                erW = (erW + 1) % erLen;

                // input diffusion (mono sum through 4 allpasses, then spread)
                float x = 0.5f * (pl + pr) + 0.25f * (el + er);
                for (int k = 0; k < 4; ++k)
                    x = diffusers[(size_t) k].process (x, ap[k], apG);
                const float side = 0.5f * (pl - pr);

                // read the 8 modulated lines
                float y[8];
                for (int k = 0; k < 8; ++k)
                {
                    const double m = modDepth * (1.0 + std::sin (lfo * (1.0 + 0.13 * k) + k * 0.785));
                    y[k] = lines[(size_t) k].read (len[k] + m);
                }

                // frequency-dependent decay: low band and high band get their own gain, then high damping
                float sum = 0.0f;
                for (int k = 0; k < 8; ++k)
                {
                    lowState[(size_t) k] = xA * lowState[(size_t) k] + (1.0f - xA) * y[k];
                    const float lo = lowState[(size_t) k], hi = y[k] - lo;
                    float v = lo * gLow[k] + hi * gHigh[k];
                    dampState[(size_t) k] = dA * dampState[(size_t) k] + (1.0f - dA) * v;
                    y[k] = dampState[(size_t) k];
                    sum += y[k];
                }

                // Householder feedback matrix + injection (alternating signs keep it stereo)
                const float hh = sum * (2.0f / 8.0f);
                for (int k = 0; k < 8; ++k)
                {
                    const float inject = (k & 1) ? (x - side) : (x + side);
                    lines[(size_t) k].push (y[k] - hh + inject * 0.35f);
                }

                const float oL = (y[0] - y[2] + y[4] - y[6]) * 0.5f;
                const float oR = (y[1] - y[3] + y[5] - y[7]) * 0.5f;
                const float mid = 0.5f * (oL + oR), sd = 0.5f * (oL - oR) * width;
                lateL[i] = (mid + sd) * duckGain;
                lateR[i] = (mid - sd) * duckGain;
                earlyL[i] = el * duckGain;
                earlyR[i] = er * duckGain;

                lfo += lfoInc;
            }
            if (lfo > juce::MathConstants<double>::twoPi * 1000.0) lfo = std::fmod (lfo, juce::MathConstants<double>::twoPi);

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
            duckMeter.store (duckShown);
        }

        std::unique_ptr<juce::Component> createVisual() override;
        int getVisualHeight() const override { return 170; }

        std::atomic<float> activity { 0.0f }, duckMeter { 1.0f };

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

        juce::AudioBuffer<float> preBuf, erBuf, work;
        std::array<ModDelay, 8> lines;
        std::array<Allpass, 4> diffusers;
        std::array<float, 8> lowState {}, dampState {};
        int preW = 0, erW = 0;
        double lfo = 0.0;
        float duckEnv = 0.0f, duckGain = 1.0f;
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

            const float size = plugin.get ("size") * 0.01f;
            const float decay = juce::jlimit (0.0f, 1.0f, std::log10 (plugin.get ("time") / 0.1f) / std::log10 (200.0f));
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
            static const char* spaces[] = { "ROOM", "HALL", "PLATE", "CHAMBER" };
            const float duckNow = plugin.duckMeter.load();
            g.drawText (juce::String (spaces[juce::jlimit (0, 3, plugin.getInt ("type"))]) + "   SIZE " + juce::String ((int) (size * 100)) + "%   DECAY "
                            + juce::String (plugin.get ("time"), 1) + " s" + (duckNow < 0.97f ? "   DUCKING " + juce::String ((int) ((1.0f - duckNow) * 100)) + "%" : juce::String()),
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
