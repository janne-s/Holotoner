#include "TextSource.h"
#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif

namespace textSource {
juce::StringArray segments(const juce::String& text, int layout, juce::String& error)
{
    juce::StringArray result;
    if (text.length() > 256) { error = "Text limit: 256 characters"; return {}; }
    if (layout == 0) result.add(text.trim());
    else if (layout == 2) result.addTokens(text, " \t\r\n", "");
    else {
#if JUCE_MAC
        const auto utf8 = text.toUTF8();
        auto string = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(utf8.getAddress()),
                                             static_cast<CFIndex>(utf8.sizeInBytes()-1), kCFStringEncodingUTF8, false);
        if (string == nullptr) { error = "Cannot read text"; return {}; }
        for (CFIndex i = 0; i < CFStringGetLength(string);) {
            const auto range = CFStringGetRangeOfComposedCharactersAtIndex(string, i);
            auto cluster = CFStringCreateWithSubstring(nullptr, string, range);
            char buffer[2048] {};
            if (!CFStringGetCString(cluster, buffer, sizeof(buffer), kCFStringEncodingUTF8))
                error = "Text character is too large";
            else result.add(juce::String::fromUTF8(buffer));
            CFRelease(cluster);
            i = range.location + range.length;
        }
        CFRelease(string);
#else
        // Never split an emoji or combining sequence with a code-point-only approximation.
        error = "Character playback currently requires macOS; Whole and Words are available";
#endif
    }
    result.trim();
    result.removeEmptyStrings();
    if (result.size() > kMaximumFrames) error = "Playback limit: 32 visible characters or words";
    if (result.isEmpty() && error.isEmpty()) error = "Enter text or an emoji";
    return error.isNotEmpty() ? juce::StringArray {} : result;
}

juce::Image render(const juce::String& text)
{
    juce::Image image(juce::Image::ARGB, 512, 512, true);
    juce::Graphics graphics(image);
    graphics.setColour(juce::Colours::white);
    // JUCE's shaping/fallback path supports system colour emoji as well as ordinary text.
    // Fit the entire source into a square; the same bitmap feeds preview, mask and state.
    graphics.setFont(juce::FontOptions(180.0f));
    graphics.drawFittedText(text, image.getBounds().reduced(28), juce::Justification::centred,
                            8, 0.05f);
    return fitInk(image);
}

juce::Image fitInk(const juce::Image& image)
{
    // Font point size is not the visible glyph size. The previous fixed square
    // left a small glyph in its centre; a 64px mask + high threshold reduced A to dots.
    // Fit actual ink bounds, not the font's line box. Preserve the glyph's aspect.
    int left = image.getWidth(), top = image.getHeight(), right = 0, bottom = 0;
    const juce::Image::BitmapData pixels(image,juce::Image::BitmapData::readOnly);
    for (int y = 0; y < image.getHeight(); ++y)
        for (int x = 0; x < image.getWidth(); ++x)
            if (pixels.getPixelColour(x,y).getAlpha() > 8) {
                left = std::min(left,x); top = std::min(top,y);
                right = std::max(right,x+1); bottom = std::max(bottom,y+1);
            }
    if (right <= left || bottom <= top) return image;
    const auto ink = image.getClippedImage({left,top,right-left,bottom-top});
    juce::Image fitted(juce::Image::ARGB,512,512,true);
    juce::Graphics target(fitted);
    target.setImageResamplingQuality(juce::Graphics::highResamplingQuality);
    target.drawImageWithin(ink,24,24,464,464,juce::RectanglePlacement::centred);
    return fitted;
}
} // namespace textSource
