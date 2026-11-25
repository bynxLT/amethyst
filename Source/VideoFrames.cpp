// Pulls small square frames out of a video file (for animated profile pictures).
//   Windows: Media Foundation source reader (MP4 / MOV / WMV with the codecs Windows ships).
// Kept free of JUCE headers so the system headers can't clash with JUCE names.
#include <vector>
#include <cstdint>
#include <cstring>
#include <algorithm>

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
#endif

// Decodes up to maxFrames frames, one every intervalSeconds from the start, each centre-cropped to a
// square and box-filtered down to side x side. Output: frameCount frames of BGRA, back to back.
bool amethystDecodeVideoSquares (const wchar_t* path, int maxFrames, double intervalSeconds, int side,
                                 std::vector<std::uint8_t>& out, int& frameCount)
{
    out.clear();
    frameCount = 0;
   #if defined (_WIN32)
    using Microsoft::WRL::ComPtr;
    const bool comOk = SUCCEEDED (CoInitializeEx (nullptr, COINIT_MULTITHREADED));
    if (FAILED (MFStartup (MF_VERSION, MFSTARTUP_LITE)))
    {
        if (comOk) CoUninitialize();
        return false;
    }

    bool ok = false;
    {
        ComPtr<IMFAttributes> attrs;
        MFCreateAttributes (&attrs, 1);
        attrs->SetUINT32 (MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);   // lets us ask for RGB32

        ComPtr<IMFSourceReader> reader;
        if (SUCCEEDED (MFCreateSourceReaderFromURL (path, attrs.Get(), &reader)))
        {
            const DWORD stream = (DWORD) MF_SOURCE_READER_FIRST_VIDEO_STREAM;
            reader->SetStreamSelection ((DWORD) MF_SOURCE_READER_ALL_STREAMS, FALSE);
            reader->SetStreamSelection (stream, TRUE);

            ComPtr<IMFMediaType> want;
            MFCreateMediaType (&want);
            want->SetGUID (MF_MT_MAJOR_TYPE, MFMediaType_Video);
            want->SetGUID (MF_MT_SUBTYPE, MFVideoFormat_RGB32);
            if (SUCCEEDED (reader->SetCurrentMediaType (stream, nullptr, want.Get())))
            {
                ComPtr<IMFMediaType> actual;
                reader->GetCurrentMediaType (stream, &actual);
                UINT32 w = 0, h = 0;
                MFGetAttributeSize (actual.Get(), MF_MT_FRAME_SIZE, &w, &h);
                INT32 stride = (INT32) (w * 4);
                UINT32 strideAttr = 0;
                if (SUCCEEDED (actual->GetUINT32 (MF_MT_DEFAULT_STRIDE, &strideAttr)))
                    stride = (INT32) strideAttr;

                if (w > 0 && h > 0)
                {
                    const int sq = (int) std::min (w, h);
                    const int x0 = ((int) w - sq) / 2, y0 = ((int) h - sq) / 2;
                    const LONGLONG step = (LONGLONG) (intervalSeconds * 10000000.0);
                    LONGLONG nextWanted = 0;
                    std::vector<std::uint8_t> frame ((size_t) side * side * 4);

                    while (frameCount < maxFrames)
                    {
                        DWORD flags = 0;
                        LONGLONG ts = 0;
                        ComPtr<IMFSample> sample;
                        if (FAILED (reader->ReadSample (stream, 0, nullptr, &flags, &ts, &sample)))
                            break;
                        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
                            break;
                        if (sample == nullptr || ts < nextWanted)
                            continue;

                        ComPtr<IMFMediaBuffer> buffer;
                        if (FAILED (sample->ConvertToContiguousBuffer (&buffer)))
                            continue;
                        BYTE* data = nullptr;
                        DWORD len = 0;
                        if (FAILED (buffer->Lock (&data, nullptr, &len)))
                            continue;

                        const INT32 absStride = stride < 0 ? -stride : stride;
                        if ((DWORD) absStride * h <= len)
                        {
                            // box filter the centred square down to side x side
                            for (int oy = 0; oy < side; ++oy)
                            {
                                const int sy0 = y0 + oy * sq / side, sy1 = std::max (sy0 + 1, y0 + (oy + 1) * sq / side);
                                for (int ox = 0; ox < side; ++ox)
                                {
                                    const int sx0 = x0 + ox * sq / side, sx1 = std::max (sx0 + 1, x0 + (ox + 1) * sq / side);
                                    unsigned sum[3] = { 0, 0, 0 }, n = 0;
                                    for (int sy = sy0; sy < sy1; sy += std::max (1, (sy1 - sy0) / 4))
                                    {
                                        const int row = stride < 0 ? (int) h - 1 - sy : sy;   // bottom-up when the stride is negative
                                        const BYTE* p = data + (size_t) row * absStride;
                                        for (int sx = sx0; sx < sx1; sx += std::max (1, (sx1 - sx0) / 4))
                                        {
                                            sum[0] += p[sx * 4]; sum[1] += p[sx * 4 + 1]; sum[2] += p[sx * 4 + 2];
                                            ++n;
                                        }
                                    }
                                    auto* d = frame.data() + ((size_t) oy * side + ox) * 4;
                                    d[0] = (std::uint8_t) (sum[0] / n); d[1] = (std::uint8_t) (sum[1] / n); d[2] = (std::uint8_t) (sum[2] / n); d[3] = 255;
                                }
                            }
                            out.insert (out.end(), frame.begin(), frame.end());
                            ++frameCount;
                            nextWanted = (ts / step + 1) * step;   // stay on the 80 ms grid
                        }
                        buffer->Unlock();
                    }
                    ok = frameCount > 0;
                }
            }
        }
    }

    MFShutdown();
    if (comOk) CoUninitialize();
    return ok;
   #else
    (void) path; (void) maxFrames; (void) intervalSeconds; (void) side;
    return false;
   #endif
}
