#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace holotoner {

inline constexpr int kImageMaskWidth = 64;
inline constexpr int kImageMaskHeight = 64;

enum class PixelChannel { Off, Luma, Alpha, Red, Green, Blue };

/** Straight-alpha RGBA8 pixels in top-to-bottom row order. */
struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;

    bool isValid() const;
};

struct ImageMaskSettings {
    PixelChannel channel = PixelChannel::Off;
    float gain = 1.0f;
    float fill = 0.0f;
    bool normalize = false;
    bool invert = false;
    float amount = 1.0f;
};

/** Fixed-size immutable-by-convention data suitable for audio-thread handoff. */
struct ImageMask {
    std::array<float, static_cast<std::size_t>(kImageMaskWidth * kImageMaskHeight)> values {};
    bool hasSource = false;

    float at(int x, int y) const;
};

/**
 * Builds the canonical modulation mask without graphics or wrapper code.
 * Transparent pixels stay empty; Fill only raises values inside alpha.
 */
ImageMask buildImageMask(const RgbaImage& image, ImageMaskSettings settings);

} // namespace holotoner
