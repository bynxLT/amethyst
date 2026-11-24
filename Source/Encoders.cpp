// MP3 / M4A (AAC) encoding with the operating system's own encoders.
//   Windows: Media Foundation (MP3 and AAC encoders ship with Windows 10/11).
//   macOS:   AudioToolbox ExtAudioFile (AAC; macOS has no built-in MP3 encoder).
// Kept free of JUCE headers so the system headers can't clash with JUCE names.
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <cmath>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <mfapi.h>
 #include <mfidl.h>
 #include <mfreadwrite.h>
 #include <mferror.h>
 #include <wrl/client.h>
 #pragma comment (lib, "mfplat.lib")
 #pragma comment (lib, "mfreadwrite.lib")
 #pragma comment (lib, "mfuuid.lib")
 #pragma comment (lib, "ole32.lib")
#elif defined (__APPLE__)
 #include <AudioToolbox/AudioToolbox.h>
#endif

// formatId: 0 = MP3, 1 = AAC in .m4a
bool amethystCanEncode (int formatId)
{
   #if defined (_WIN32)
    (void) formatId;
    return true;
   #elif defined (__APPLE__)
    return formatId == 1;
   #else
    (void) formatId;
    return false;
   #endif
}

// Returns an empty string on success, otherwise a readable error.
std::string amethystEncodeCompressed (const char* utf8Path, const float* interleaved, long long frames,
                                      int channels, int sampleRate, int formatId, int kbps)
{
   #if defined (_WIN32)
    using Microsoft::WRL::ComPtr;
    const bool aac = formatId == 1;

    const HRESULT co = CoInitializeEx (nullptr, COINIT_MULTITHREADED);
    const bool coInitialised = SUCCEEDED (co);
    if (FAILED (MFStartup (MF_VERSION)))
    {
        if (coInitialised) CoUninitialize();
        return "Windows Media Foundation isn't available on this PC.";
    }

    struct Cleanup
    {
        bool co;
        ~Cleanup() { MFShutdown(); if (co) CoUninitialize(); }
    } cleanup { coInitialised };

    const int wlen = MultiByteToWideChar (CP_UTF8, 0, utf8Path, -1, nullptr, 0);
    std::wstring path ((size_t) std::max (0, wlen), L'\0');
    MultiByteToWideChar (CP_UTF8, 0, utf8Path, -1, path.data(), wlen);
    DeleteFileW (path.c_str());

    ComPtr<IMFSinkWriter> writer;
    if (FAILED (MFCreateSinkWriterFromURL (path.c_str(), nullptr, nullptr, &writer)))
        return "Couldn't create the output file.";

    // compressed output stream
    ComPtr<IMFMediaType> outType;
    MFCreateMediaType (&outType);
    outType->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID (MF_MT_SUBTYPE, aac ? MFAudioFormat_AAC : MFAudioFormat_MP3);
    outType->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) sampleRate);
    outType->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, (UINT32) channels);
    outType->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32) (kbps * 1000 / 8));
    if (aac)
        outType->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    else
        outType->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, 1);

    DWORD stream = 0;
    if (FAILED (writer->AddStream (outType.Get(), &stream)))
        return aac ? "This PC's AAC encoder doesn't support these settings." : "This PC's MP3 encoder doesn't support these settings.";

    // what we feed it: 16-bit PCM
    ComPtr<IMFMediaType> inType;
    MFCreateMediaType (&inType);
    inType->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID (MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inType->SetUINT32 (MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    inType->SetUINT32 (MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32) sampleRate);
    inType->SetUINT32 (MF_MT_AUDIO_NUM_CHANNELS, (UINT32) channels);
    inType->SetUINT32 (MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32) (channels * 2));
    inType->SetUINT32 (MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32) (sampleRate * channels * 2));
    if (FAILED (writer->SetInputMediaType (stream, inType.Get(), nullptr)))
        return "The encoder rejected the audio format.";

    if (FAILED (writer->BeginWriting()))
        return "The encoder couldn't start.";

    const long long chunk = 4096;
    LONGLONG time = 0;
    for (long long f = 0; f < frames; f += chunk)
    {
        const long long n = std::min (chunk, frames - f);
        const DWORD bytes = (DWORD) (n * channels * 2);

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED (MFCreateMemoryBuffer (bytes, &buffer)))
            return "Out of memory while encoding.";
        BYTE* data = nullptr;
        buffer->Lock (&data, nullptr, nullptr);
        auto* out = reinterpret_cast<int16_t*> (data);
        const float* in = interleaved + f * channels;
        for (long long i = 0; i < n * channels; ++i)
            out[i] = (int16_t) std::clamp ((int) std::lround (in[i] * 32767.0f), -32768, 32767);
        buffer->Unlock();
        buffer->SetCurrentLength (bytes);

        ComPtr<IMFSample> sample;
        MFCreateSample (&sample);
        sample->AddBuffer (buffer.Get());
        const LONGLONG duration = (LONGLONG) (n * 10000000LL / sampleRate);
        sample->SetSampleTime (time);
        sample->SetSampleDuration (duration);
        time += duration;

        if (FAILED (writer->WriteSample (stream, sample.Get())))
            return "Encoding failed part-way through.";
    }

    if (FAILED (writer->Finalize()))
        return "Couldn't finish writing the file.";
    return {};

   #elif defined (__APPLE__)
    if (formatId != 1)
        return "MP3 export isn't available on macOS yet - choose M4A, WAV, FLAC or OGG.";

    CFURLRef url = CFURLCreateFromFileSystemRepresentation (nullptr, (const UInt8*) utf8Path, (CFIndex) std::strlen (utf8Path), false);
    if (url == nullptr)
        return "Invalid file path.";

    AudioStreamBasicDescription dst {};
    dst.mSampleRate = sampleRate;
    dst.mFormatID = kAudioFormatMPEG4AAC;
    dst.mChannelsPerFrame = (UInt32) channels;

    ExtAudioFileRef file = nullptr;
    OSStatus err = ExtAudioFileCreateWithURL (url, kAudioFileM4AType, &dst, nullptr, kAudioFileFlags_EraseFile, &file);
    CFRelease (url);
    if (err != noErr || file == nullptr)
        return "Couldn't create the M4A file.";

    AudioStreamBasicDescription client {};
    client.mSampleRate = sampleRate;
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    client.mBitsPerChannel = 32;
    client.mChannelsPerFrame = (UInt32) channels;
    client.mFramesPerPacket = 1;
    client.mBytesPerFrame = (UInt32) (4 * channels);
    client.mBytesPerPacket = (UInt32) (4 * channels);
    ExtAudioFileSetProperty (file, kExtAudioFileProperty_ClientDataFormat, sizeof (client), &client);

    UInt32 bitrate = (UInt32) (kbps * 1000);
    AudioConverterRef converter = nullptr;
    UInt32 size = sizeof (converter);
    if (ExtAudioFileGetProperty (file, kExtAudioFileProperty_AudioConverter, &size, &converter) == noErr && converter != nullptr)
    {
        AudioConverterSetProperty (converter, kAudioConverterEncodeBitRate, sizeof (bitrate), &bitrate);
        CFArrayRef config = nullptr;
        ExtAudioFileSetProperty (file, kExtAudioFileProperty_ConverterConfig, sizeof (config), &config);
    }

    const long long chunk = 4096;
    for (long long f = 0; f < frames; f += chunk)
    {
        const long long n = std::min (chunk, frames - f);
        AudioBufferList abl;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = (UInt32) channels;
        abl.mBuffers[0].mDataByteSize = (UInt32) (n * 4 * channels);
        abl.mBuffers[0].mData = (void*) (interleaved + f * channels);
        if (ExtAudioFileWrite (file, (UInt32) n, &abl) != noErr)
        {
            ExtAudioFileDispose (file);
            return "Encoding failed part-way through.";
        }
    }
    ExtAudioFileDispose (file);
    return {};

   #else
    (void) utf8Path; (void) interleaved; (void) frames; (void) channels; (void) sampleRate; (void) formatId; (void) kbps;
    return "Compressed export isn't available on this platform.";
   #endif
}
