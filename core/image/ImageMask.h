#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include "core/dsp/PhaseMap.h"

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
    float threshold = 0.0f;
    bool binary = false;
    PlaneTransform plane;
    PhaseProjection projection = PhaseProjection::Cartesian;
};

/** Fixed-size immutable-by-convention data suitable for audio-thread handoff. */
struct ImageMask {
    struct PhasePoint { float x = 0.0f; float y = 0.0f; };
    static constexpr std::size_t kPhasePointCount = 8192;
    std::array<float, static_cast<std::size_t>(kImageMaskWidth * kImageMaskHeight)> values {};
    // Brightness-weighted quantiles of the complete 2D mask, compiled OFF-thread.
    std::array<PhasePoint, kPhasePointCount> phasePoints {};
    bool hasPhasePoints = false;
    float aspectX = 1.0f, aspectY = 1.0f;
    bool hasSource = false;
    static constexpr int kFieldRows = 64;
    static constexpr int kFieldAngles = 129;
    // Conditional image support at each actual sample radius. Empty rows are identity.
    std::array<float, kFieldRows * kFieldAngles> fieldAngles {};
    PlaneTransform fieldPlane;
    PhaseProjection fieldProjection = PhaseProjection::Cartesian;
    bool fieldReady = false;

    float at(int x, int y) const;
};

/**
 * Builds the canonical modulation mask without graphics or wrapper code.
 * Transparent pixels stay empty; Fill only raises values inside alpha.
 */
ImageMask buildImageMask(const RgbaImage& image, ImageMaskSettings settings);
/** Off-thread rasterization: never call from the render callback. */
void buildImageField(ImageMask& mask, PlaneTransform plane, PhaseProjection projection);

} // namespace holotoner
