#include "core/image/ImageMask.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

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
    buildImageField(result, settings.plane, settings.projection);
    return result;
}

void buildImageField(ImageMask& mask, PlaneTransform plane, PhaseProjection projection)
{
    mask.fieldPlane = plane;
    mask.fieldProjection = projection;
    mask.fieldReady = true;
    plane.scaleX *= mask.aspectX;
    plane.scaleY *= mask.aspectY;
    const auto projected = projectPlane(plane);
    const auto& a = projected.corners[0];
    const auto& b = projected.corners[1];
    const auto& d = projected.corners[3];
    const auto bx = b.stereo - a.stereo, by = b.frequency - a.frequency;
    const auto dx = d.stereo - a.stereo, dy = d.frequency - a.frequency;
    const auto determinant = bx * dy - by * dx;
    for (int row = 0; row < ImageMask::kFieldRows; ++row) {
        const auto radius = (static_cast<float>(row) + 0.5f) / static_cast<float>(ImageMask::kFieldRows);
        std::array<float, ImageMask::kFieldAngles> support {};
        float maximum = 0.0f;
        for (int column = 0; column < ImageMask::kFieldAngles; ++column) {
            const auto angle = -0.5f * std::numbers::pi_v<float>
                + std::numbers::pi_v<float> * static_cast<float>(column)
                    / static_cast<float>(ImageMask::kFieldAngles - 1);
            const auto x = projection == PhaseProjection::Cartesian
                ? 2.0f * radius * std::sin(angle) : angle * 4.0f / std::numbers::pi_v<float>;
            const auto y = projection == PhaseProjection::Cartesian
                ? 2.0f * (radius * std::cos(angle) - 0.5f) : 4.0f * (radius - 0.5f);
            if (std::abs(determinant) > 1.0e-6f) {
                const auto u = ((x-a.stereo)*dy - (y-a.frequency)*dx) / determinant;
                const auto v = (bx*(y-a.frequency) - by*(x-a.stereo)) / determinant;
                if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) {
                    const auto px = u * static_cast<float>(kImageMaskWidth - 1);
                    const auto py = (1.0f-v) * static_cast<float>(kImageMaskHeight - 1);
                    const auto ix = static_cast<int>(px), iy = static_cast<int>(py);
                    const auto tx = px-static_cast<float>(ix), ty = py-static_cast<float>(iy);
                    const auto top = mask.at(ix,iy)*(1-tx) + mask.at(std::min(ix+1,63),iy)*tx;
                    const auto bottom = mask.at(ix,std::min(iy+1,63))*(1-tx)
                        + mask.at(std::min(ix+1,63),std::min(iy+1,63))*tx;
                    support[static_cast<std::size_t>(column)] = top*(1-ty)+bottom*ty;
                    maximum = std::max(maximum, support[static_cast<std::size_t>(column)]);
                }
            }
        }
        for (int column = 0; column < ImageMask::kFieldAngles; ++column) {
            int nearest = column;
            int distance = ImageMask::kFieldAngles;
            if (maximum > 0.001f)
                for (int candidate = 0; candidate < ImageMask::kFieldAngles; ++candidate)
                    if (support[static_cast<std::size_t>(candidate)] >= 0.1f*maximum
                        && std::abs(candidate-column) < distance) {
                        nearest = candidate;
                        distance = std::abs(candidate-column);
                    }
            mask.fieldAngles[static_cast<std::size_t>(row*ImageMask::kFieldAngles+column)]
                = -0.5f * std::numbers::pi_v<float> + std::numbers::pi_v<float>
                    * static_cast<float>(nearest) / static_cast<float>(ImageMask::kFieldAngles-1);
        }
    }
}

} // namespace holotoner
