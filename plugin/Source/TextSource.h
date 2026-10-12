#pragma once
#include <juce_graphics/juce_graphics.h>

namespace textSource {
inline constexpr int kMaximumFrames = 32;
// Whole text, visible grapheme clusters, whitespace-delimited words. Blank clusters are skipped.
juce::StringArray segments(const juce::String& text, int layout, juce::String& error);
juce::Image render(const juce::String& text);
juce::Image fitInk(const juce::Image& image);
}
