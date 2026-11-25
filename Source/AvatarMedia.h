#pragma once

#include <JuceHeader.h>

// Profile pictures, still or animated.
// An animated avatar is stored as a horizontal strip of square frames (width = frames x height),
// played at a fixed 80 ms per frame. A still picture is just one square.
namespace AvatarMedia
{
    constexpr int kFrameMs = 80;
    constexpr int kMaxFrames = 48;     // ~3.8 s
    constexpr int kAnimSide = 96;      // animated frames are small; still pictures stay 256

    // Reads PNG / JPG / GIF (animated or not) / MP4 / MOV (Windows) into a still square or a strip.
    // Returns an invalid image and sets error on failure.
    juce::Image load (const juce::File&, juce::String& error);

    bool isAnimated (const juce::Image& img);
    int frameCount (const juce::Image& img);
    juce::Image currentFrame (const juce::Image& img);   // the frame to show right now (whole image if still)

    // Encodes for upload: PNG for stills, JPEG for strips (kept under maxBytes). mimeType is set.
    juce::MemoryBlock encode (const juce::Image& img, juce::String& mimeType, size_t maxBytes = 950 * 1024);

    // GIF decoding (exposed for tests): calls onFrame with the full composited canvas and its delay.
    // Return false from onFrame to stop. Returns false if the data isn't a readable GIF.
    bool decodeGif (const void* data, size_t size, const std::function<bool (const juce::Image& canvas, int delayMs)>& onFrame);
}
