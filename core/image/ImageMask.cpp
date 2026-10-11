#include "core/image/ImageMask.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace holotoner {
namespace {

float finiteOr(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

struct Rgba {
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    float alpha = 0.0f;
};

Rgba pixel(const RgbaImage& image, int x, int y)
{
    x = std::clamp(x, 0, image.width - 1);
    y = std::clamp(y, 0, image.height - 1);
    const auto offset = static_cast<std::size_t>((y * image.width + x) * 4);
    constexpr float byteToUnit = 1.0f / 255.0f;
    return {static_cast<float>(image.pixels[offset]) * byteToUnit,
            static_cast<float>(image.pixels[offset + 1]) * byteToUnit,
            static_cast<float>(image.pixels[offset + 2]) * byteToUnit,
            static_cast<float>(image.pixels[offset + 3]) * byteToUnit};
}

Rgba bilinear(const RgbaImage& image, float x, float y)
{
    const auto x0 = static_cast<int>(std::floor(x));
    const auto y0 = static_cast<int>(std::floor(y));
    const auto x1 = std::min(x0 + 1, image.width - 1);
    const auto y1 = std::min(y0 + 1, image.height - 1);
    const auto tx = x - static_cast<float>(x0);
    const auto ty = y - static_cast<float>(y0);
    const auto a = pixel(image, x0, y0);
    const auto b = pixel(image, x1, y0);
    const auto c = pixel(image, x0, y1);
    const auto d = pixel(image, x1, y1);
    const auto mix = [tx, ty](float av, float bv, float cv, float dv) {
        const auto top = av + tx * (bv - av);
        const auto bottom = cv + tx * (dv - cv);
        return top + ty * (bottom - top);
    };
    return {mix(a.red, b.red, c.red, d.red), mix(a.green, b.green, c.green, d.green),
            mix(a.blue, b.blue, c.blue, d.blue), mix(a.alpha, b.alpha, c.alpha, d.alpha)};
}

float channelValue(const Rgba& rgba, PixelChannel channel)
{
    switch (channel) {
    case PixelChannel::Off:
        return 0.0f;
    case PixelChannel::Luma:
        return (0.2126f * rgba.red + 0.7152f * rgba.green + 0.0722f * rgba.blue) * rgba.alpha;
    case PixelChannel::Alpha:
        return rgba.alpha;
    case PixelChannel::Red:
        return rgba.red * rgba.alpha;
    case PixelChannel::Green:
        return rgba.green * rgba.alpha;
    case PixelChannel::Blue:
        return rgba.blue * rgba.alpha;
    }
    return 0.0f;
}

} // namespace

bool RgbaImage::isValid() const
{
    if (width <= 0 || height <= 0)
        return false;
    const auto expected = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    return pixels.size() == expected;
}

float ImageMask::at(int x, int y) const
{
    if (x < 0 || x >= kImageMaskWidth || y < 0 || y >= kImageMaskHeight)
        return 0.0f;
    return values[static_cast<std::size_t>(y * kImageMaskWidth + x)];
}

ImageMask buildImageMask(const RgbaImage& image, ImageMaskSettings settings)
{
    ImageMask result;
    if (!image.isValid() || settings.channel == PixelChannel::Off)
        return result;
    result.hasSource = true;
    const auto aspect = static_cast<float>(image.width) / static_cast<float>(image.height);
    result.aspectX = std::min(aspect, 1.0f);
    result.aspectY = std::min(1.0f / aspect, 1.0f);

    settings.gain = std::clamp(finiteOr(settings.gain, 1.0f), 0.0f, 8.0f);
    settings.fill = std::clamp(finiteOr(settings.fill, 0.0f), 0.0f, 1.0f);
    settings.amount = std::clamp(finiteOr(settings.amount, 1.0f), 0.0f, 1.0f);
    settings.threshold = std::clamp(finiteOr(settings.threshold, 0.0f), 0.0f, 1.0f);
    std::array<float, result.values.size()> alpha {};
    float minimum = std::numeric_limits<float>::max();
    float maximum = std::numeric_limits<float>::lowest();

    for (int y = 0; y < kImageMaskHeight; ++y) {
        const auto sourceY = static_cast<float>(y) * static_cast<float>(image.height - 1)
            / static_cast<float>(kImageMaskHeight - 1);
        for (int x = 0; x < kImageMaskWidth; ++x) {
            const auto sourceX = static_cast<float>(x) * static_cast<float>(image.width - 1)
                / static_cast<float>(kImageMaskWidth - 1);
            const auto rgba = bilinear(image, sourceX, sourceY);
            const auto index = static_cast<std::size_t>(y * kImageMaskWidth + x);
            alpha[index] = rgba.alpha;
            auto value = std::max(channelValue(rgba, settings.channel), settings.fill * rgba.alpha);
            value = std::clamp(value * settings.gain, 0.0f, 1.0f);
            result.values[index] = value;
            if (rgba.alpha > 0.0f) {
                minimum = std::min(minimum, value);
                maximum = std::max(maximum, value);
            }
        }
    }

    if (settings.normalize && maximum - minimum > 1.0e-6f) {
        const auto scale = 1.0f / (maximum - minimum);
        for (std::size_t i = 0; i < result.values.size(); ++i)
            result.values[i] = alpha[i] > 0.0f ? (result.values[i] - minimum) * scale : 0.0f;
    }
    for (std::size_t i = 0; i < result.values.size(); ++i) {
        if (settings.invert)
            result.values[i] = std::max(0.0f, alpha[i] - result.values[i]);
        const auto value = result.values[i];
        result.values[i] = value > settings.threshold
            ? (settings.binary ? alpha[i] : value) : 0.0f;
        result.values[i] = std::clamp(result.values[i] * settings.amount, 0.0f, 1.0f);
    }
    double total = 0.0;
    for (const auto value : result.values)
        total += value;
    if (total > 1.0e-9) {
        result.hasPhasePoints = true;
        std::size_t cursor = 0;
        double accumulated = 0.0;
        for (int y = 0; y < kImageMaskHeight; ++y)
            for (int column = 0; column < kImageMaskWidth; ++column) {
                const auto x = (y % 2 == 0) ? column : kImageMaskWidth - 1 - column;
                accumulated += result.at(x, y);
                while (cursor < result.phasePoints.size()
                       && total * (static_cast<double>(cursor) + 0.5)
                              / static_cast<double>(result.phasePoints.size()) <= accumulated) {
                    result.phasePoints[cursor++] = {
                        2.0f * static_cast<float>(x) / static_cast<float>(kImageMaskWidth - 1) - 1.0f,
                        1.0f - 2.0f * static_cast<float>(y) / static_cast<float>(kImageMaskHeight - 1)};
                }
            }
        while (cursor < result.phasePoints.size()) {
            result.phasePoints[cursor] = result.phasePoints[cursor - 1];
            ++cursor;
        }
    }
    return result;
}

} // namespace holotoner
