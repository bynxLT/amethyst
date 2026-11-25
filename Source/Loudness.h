#pragma once

#include <JuceHeader.h>

// Loudness measurement after ITU-R BS.1770-4 / EBU R128:
//  - K-weighting (high-shelf "head" filter + RLB high-pass), designed for any sample rate
//  - momentary (400 ms) and short-term (3 s) loudness, integrated loudness with the
//    absolute (-70 LUFS) and relative (-10 LU) gates, and a 4x oversampled true-peak estimate.
// Used by the Loudness Meter effect, loudness-normalised export and Smart Vocal Chain.
namespace Loudness
{
    // One biquad section, transposed direct form II.
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        inline double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void reset() noexcept { z1 = z2 = 0; }
    };

    // The two K-weighting stages, from the BS.1770 analog prototypes (exact at 48 kHz, correct at any rate).
    inline void designKWeighting (double sr, Biquad& shelf, Biquad& highPass)
    {
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (juce::MathConstants<double>::pi * f0 / sr);
            const double Vh = std::pow (10.0, G / 20.0), Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            shelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
            shelf.b1 = 2.0 * (K * K - Vh) / a0;
            shelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
            shelf.a1 = 2.0 * (K * K - 1.0) / a0;
            shelf.a2 = (1.0 - K / Q + K * K) / a0;
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (juce::MathConstants<double>::pi * f0 / sr);
            const double a0 = 1.0 + K / Q + K * K;
            highPass.b0 = 1.0;
            highPass.b1 = -2.0;
            highPass.b2 = 1.0;
            highPass.a1 = 2.0 * (K * K - 1.0) / a0;
            highPass.a2 = (1.0 - K / Q + K * K) / a0;
        }
    }

    inline double toLufs (double meanSquare) { return meanSquare > 1.0e-12 ? -0.691 + 10.0 * std::log10 (meanSquare) : -100.0; }

    // Streaming meter: feed blocks, read values. Not thread-safe on its own; the owner publishes atomics.
    class Meter
    {
    public:
        void prepare (double sampleRate)
        {
            sr = sampleRate;
            for (int c = 0; c < 2; ++c)
            {
                designKWeighting (sr, shelf[c], hp[c]);
                shelf[c].reset();
                hp[c].reset();
            }
            stepLen = juce::jmax (1, (int) std::lround (sr * 0.1));   // 100 ms sub-blocks
            stepAcc = 0.0;
            stepCount = 0;
            stepRing.fill (0.0);
            numSteps = 0;
            ringPos = 0;
            binSum.fill (0.0);
            binCount.fill (0);
            truePeak = 0.0f;
            for (auto& h : tpHist) h.fill (0.0f);
            tpIndex = 0;
        }

        void reset() { prepare (sr); }

        void process (const float* left, const float* right, int n)
        {
            for (int i = 0; i < n; ++i)
            {
                const double l = hp[0].process (shelf[0].process (left[i]));
                const double r = hp[1].process (shelf[1].process (right != nullptr ? right[i] : left[i]));
                stepAcc += l * l + r * r;
                if (++stepCount >= stepLen)
                {
                    pushStep (stepAcc / stepCount);
                    stepAcc = 0.0;
                    stepCount = 0;
                }
                trackTruePeak (left[i], 0);
                trackTruePeak (right != nullptr ? right[i] : left[i], 1);
            }
        }

        double momentary() const { return windowLufs (4); }    // last 400 ms
        double shortTerm() const { return windowLufs (30); }   // last 3 s

        // Gating blocks live in a 0.1 LU histogram from -70 to +5 LUFS, so this never allocates.
        double integrated() const
        {
            double sum = 0.0;
            long long count = 0;
            for (size_t b = 0; b < kBins; ++b) { sum += binSum[b]; count += binCount[b]; }
            if (count == 0)
                return -100.0;
            const double relGate = toLufs (sum / (double) count) - 10.0;
            sum = 0.0;
            count = 0;
            for (size_t b = 0; b < kBins; ++b)
                if (-70.0 + (double) b * 0.1 + 0.05 > relGate) { sum += binSum[b]; count += binCount[b]; }
            return count > 0 ? toLufs (sum / (double) count) : -100.0;
        }

        float truePeakDb() const { return juce::Decibels::gainToDecibels (truePeak, -100.0f); }

    private:
        void pushStep (double meanSquare)
        {
            stepRing[ringPos] = meanSquare;
            ringPos = (ringPos + 1) % stepRing.size();
            numSteps = juce::jmin (numSteps + 1, stepRing.size());
            // gating blocks: 400 ms windows with 75% overlap = one every 100 ms once 4 steps exist
            if (numSteps >= 4)
            {
                const double ms = sumLast (4) / 4.0;
                const double l = toLufs (ms);
                if (l > -70.0)
                {
                    const auto b = (size_t) juce::jlimit (0, (int) kBins - 1, (int) ((l + 70.0) * 10.0));
                    binSum[b] += ms;
                    ++binCount[b];
                }
            }
        }

        double sumLast (size_t k) const
        {
            double s = 0.0;
            for (size_t i = 0; i < k; ++i)
                s += stepRing[(ringPos + stepRing.size() - 1 - i) % stepRing.size()];
            return s;
        }

        double windowLufs (size_t count) const
        {
            if (numSteps == 0)
                return -100.0;
            const size_t k = juce::jmin (count, numSteps);
            return toLufs (sumLast (k) / (double) k);
        }

        // 4x oversampled peak with a short windowed-sinc interpolator (good to a few tenths of a dB).
        void trackTruePeak (float x, int ch)
        {
            auto& h = tpHist[(size_t) ch];
            h[(size_t) tpIndexFor (ch)] = x;
            const float ax = std::abs (x);
            if (ax > truePeak) truePeak = ax;
            static const float taps[3][8] = {
                { -0.0110f, 0.0390f, -0.1050f, 0.8810f, 0.2390f, -0.0730f, 0.0260f, -0.0060f },
                { -0.0170f, 0.0610f, -0.1650f, 0.6210f, 0.6210f, -0.1650f, 0.0610f, -0.0170f },
                { -0.0060f, 0.0260f, -0.0730f, 0.2390f, 0.8810f, -0.1050f, 0.0390f, -0.0110f },
            };
            for (auto& t : taps)
            {
                float y = 0.0f;
                for (int k = 0; k < 8; ++k)
                    y += t[k] * h[(size_t) ((tpPos[(size_t) ch] - 7 + k + 8) & 7)];
                if (std::abs (y) > truePeak) truePeak = std::abs (y);
            }
        }
        int tpIndexFor (int ch) { tpPos[(size_t) ch] = (tpPos[(size_t) ch] + 1) & 7; return tpPos[(size_t) ch]; }

        double sr = 48000.0;
        Biquad shelf[2], hp[2];
        int stepLen = 4800, stepCount = 0;
        double stepAcc = 0.0;
        static constexpr size_t kBins = 750;
        std::array<double, 30> stepRing {};
        size_t numSteps = 0, ringPos = 0;
        std::array<double, kBins> binSum {};
        std::array<long long, kBins> binCount {};
        float truePeak = 0.0f;
        std::array<std::array<float, 8>, 2> tpHist {};
        std::array<int, 2> tpPos { 0, 0 };
        int tpIndex = 0;
    };

    // Whole-buffer analysis (offline): integrated loudness and true peak.
    struct Result { double integrated = -100.0, maxShortTerm = -100.0; float truePeakDb = -100.0f; };

    inline Result analyse (const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        Meter m;
        m.prepare (sampleRate);
        const int n = buffer.getNumSamples();
        const float* l = buffer.getReadPointer (0);
        const float* r = buffer.getNumChannels() > 1 ? buffer.getReadPointer (1) : nullptr;
        Result res;
        constexpr int chunk = 4800;
        for (int i = 0; i < n; i += chunk)
        {
            const int k = juce::jmin (chunk, n - i);
            m.process (l + i, r != nullptr ? r + i : nullptr, k);
            res.maxShortTerm = juce::jmax (res.maxShortTerm, m.shortTerm());
        }
        res.integrated = m.integrated();
        res.truePeakDb = m.truePeakDb();
        return res;
    }

    // Normalises `buffer` to `targetLufs` with a true-peak ceiling: gain, then a 5 ms look-ahead
    // limiter for anything that would still pass the ceiling. Returns the gain applied (dB).
    inline double normalise (juce::AudioBuffer<float>& buffer, double sampleRate, double targetLufs, float ceilingDb = -1.0f)
    {
        const auto before = analyse (buffer, sampleRate);
        if (before.integrated <= -70.0)
            return 0.0;
        const double gainDb = juce::jlimit (-30.0, 30.0, targetLufs - before.integrated);
        buffer.applyGain ((float) juce::Decibels::decibelsToGain (gainDb));

        const float ceiling = juce::Decibels::decibelsToGain (ceilingDb - 0.3f);   // margin for inter-sample peaks
        const int n = buffer.getNumSamples(), nCh = buffer.getNumChannels();
        const int look = juce::jmax (1, (int) (sampleRate * 0.005));
        const float rel = (float) std::exp (-1.0 / (0.08 * sampleRate));
        // required gain per sample (min over channels), then a sliding minimum over the look-ahead window
        std::vector<float> need ((size_t) n, 1.0f);
        for (int c = 0; c < nCh; ++c)
        {
            const float* d = buffer.getReadPointer (c);
            for (int i = 0; i < n; ++i)
            {
                const float a = std::abs (d[i]);
                if (a > ceiling) need[(size_t) i] = juce::jmin (need[(size_t) i], ceiling / a);
            }
        }
        std::vector<float> g ((size_t) n, 1.0f);
        const float attackStep = 1.0f / (float) look;
        float env = 1.0f;
        for (int i = n - 1; i >= 0; --i)   // backwards pass: the reduction starts `look` samples early (attack ramp)
        {
            env = juce::jmin (need[(size_t) i], env + (1.0f - env) * attackStep * 4.0f);
            g[(size_t) i] = env;
        }
        env = 1.0f;
        for (int i = 0; i < n; ++i)        // forward pass: smooth release back towards unity
        {
            env = juce::jmin (g[(size_t) i], env + (1.0f - env) * (1.0f - rel));
            g[(size_t) i] = env;
        }
        for (int c = 0; c < nCh; ++c)
        {
            float* d = buffer.getWritePointer (c);
            for (int i = 0; i < n; ++i)
                d[i] = juce::jlimit (-ceiling, ceiling, d[i] * g[(size_t) i]);
        }
        return gainDb;
    }
}
