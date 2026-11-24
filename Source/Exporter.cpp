#include "Exporter.h"

bool amethystCanEncode (int formatId);
std::string amethystEncodeCompressed (const char* utf8Path, const float* interleaved, long long frames,
                                      int channels, int sampleRate, int formatId, int kbps);

namespace Exporter
{
    juce::String displayName (Format f)
    {
        switch (f)
        {
            case Format::Wav:  return "Wave file";
            case Format::Mp3:  return "MP3 file";
            case Format::Ogg:  return "OGG file";
            case Format::Flac: return "FLAC file";
            case Format::M4a:  return "M4A file";
        }
        return {};
    }

    juce::String extension (Format f)
    {
        switch (f)
        {
            case Format::Wav:  return ".wav";
            case Format::Mp3:  return ".mp3";
            case Format::Ogg:  return ".ogg";
            case Format::Flac: return ".flac";
            case Format::M4a:  return ".m4a";
        }
        return ".wav";
    }

    bool isAvailable (Format f)
    {
        if (f == Format::Mp3) return amethystCanEncode (0);
        if (f == Format::M4a) return amethystCanEncode (1);
        return true;
    }

    namespace
    {
        juce::Result writeWithJuce (juce::AudioFormat& format, const juce::AudioBuffer<float>& buffer, double sr,
                                    const juce::File& file, int bits, int quality)
        {
            file.deleteFile();
            std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
            if (stream == nullptr || ! stream->openedOk())
                return juce::Result::fail ("Couldn't create " + file.getFullPathName());

            std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (stream.get(), sr, (unsigned int) buffer.getNumChannels(),
                                                                                     bits, {}, quality));
            if (writer == nullptr)
                return juce::Result::fail ("This format doesn't support these settings.");
            stream.release(); // owned by the writer now

            if (! writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples()))
                return juce::Result::fail ("Writing the file failed.");
            return juce::Result::ok();
        }
    }

    juce::Result write (const juce::AudioBuffer<float>& buffer, double sr, Format format, const juce::File& file)
    {
        switch (format)
        {
            case Format::Wav:
            {
                juce::WavAudioFormat wav;
                return writeWithJuce (wav, buffer, sr, file, 24, 0);
            }
            case Format::Flac:
            {
                juce::FlacAudioFormat flac;
                return writeWithJuce (flac, buffer, sr, file, 24, 0);
            }
            case Format::Ogg:
            {
                juce::OggVorbisAudioFormat ogg;
                const int quality = juce::jmax (0, ogg.getQualityOptions().size() * 3 / 4); // high quality
                return writeWithJuce (ogg, buffer, sr, file, 16, quality);
            }
            case Format::Mp3:
            case Format::M4a:
            {
                // the system encoders want 44.1 or 48 kHz
                const juce::AudioBuffer<float>* source = &buffer;
                juce::AudioBuffer<float> resampled;
                int rate = (int) std::lround (sr);
                if (rate != 44100 && rate != 48000)
                {
                    const double ratio = sr / 48000.0;
                    const int outLen = juce::jmax (1, (int) ((buffer.getNumSamples() - 8) / ratio));
                    resampled.setSize (buffer.getNumChannels(), outLen);
                    for (int c = 0; c < buffer.getNumChannels(); ++c)
                    {
                        juce::LagrangeInterpolator li;
                        li.process (ratio, buffer.getReadPointer (c), resampled.getWritePointer (c), outLen);
                    }
                    source = &resampled;
                    rate = 48000;
                }

                const int ch = source->getNumChannels();
                const int frames = source->getNumSamples();
                std::vector<float> interleaved ((size_t) frames * (size_t) ch);
                for (int c = 0; c < ch; ++c)
                {
                    const float* src = source->getReadPointer (c);
                    for (int i = 0; i < frames; ++i)
                        interleaved[(size_t) i * (size_t) ch + (size_t) c] = juce::jlimit (-1.0f, 1.0f, src[i]);
                }

                const bool aac = format == Format::M4a;
                const auto err = amethystEncodeCompressed (file.getFullPathName().toRawUTF8(), interleaved.data(), frames, ch, rate,
                                                           aac ? 1 : 0, aac ? 192 : 320);
                return err.empty() ? juce::Result::ok() : juce::Result::fail (juce::String (err));
            }
        }
        return juce::Result::fail ("Unknown format.");
    }

    //==========================================================================
    namespace
    {
        class ExportJob : public juce::ThreadWithProgressWindow
        {
        public:
            ExportJob (AudioEngine& e, Format f, juce::File target, juce::Component* centre, std::function<void (bool)> done)
                : juce::ThreadWithProgressWindow ("Exporting " + target.getFileName(), true, true, 10000, "Cancel", centre),
                  engine (e), format (f), file (std::move (target)), onDone (std::move (done))
            {
            }

            void run() override
            {
                setStatusMessage ("Rendering the whole song...");
                juce::AudioBuffer<float> mix;
                auto r = engine.renderSong (mix, [this] (float p)
                {
                    setProgress (p * 0.85);
                    return ! threadShouldExit();
                });
                if (r.failed())
                {
                    result = r;
                    return;
                }

                setStatusMessage ("Saving " + displayName (format) + "...");
                setProgress (0.9);
                result = write (mix, engine.getSampleRate(), format, file);
                seconds = mix.getNumSamples() / juce::jmax (1.0, engine.getSampleRate());
                setProgress (1.0);
            }

            void threadComplete (bool userPressedCancel) override
            {
                const bool ok = ! userPressedCancel && result.wasOk();
                if (! ok)
                    file.deleteFile(); // never leave a half-written export behind

                if (userPressedCancel)
                {
                    // nothing to report
                }
                else if (ok)
                {
                    const auto f = file;
                    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::InfoIcon, "Export finished",
                                                        "Saved " + juce::String (seconds, 1) + " seconds to\n" + file.getFullPathName(),
                                                        "Show file", "Close", nullptr,
                                                        juce::ModalCallbackFunction::create ([f] (int r) { if (r == 1) f.revealToUser(); }));
                }
                else
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Export failed", result.getErrorMessage());
                }

                if (onDone)
                    onDone (ok);
                delete this;
            }

        private:
            AudioEngine& engine;
            Format format;
            juce::File file;
            std::function<void (bool)> onDone;
            juce::Result result { juce::Result::ok() };
            double seconds = 0.0;
        };
    }

    void exportSong (AudioEngine& engine, Format format, const juce::File& file, juce::Component* centreAround, std::function<void (bool)> onDone)
    {
        engine.stop();
        (new ExportJob (engine, format, file, centreAround, std::move (onDone)))->launchThread();
    }
}
