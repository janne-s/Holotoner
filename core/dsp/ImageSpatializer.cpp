#include "core/dsp/ImageSpatializer.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <type_traits>

namespace holotoner {
namespace {

constexpr double kLowHz = 40.0;
constexpr double kHighHz = 16000.0;
constexpr double kTiny = 1.0e-12;
constexpr double kRunaway = 16.0;

float finiteOr(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

ProjectedPoint interpolate(const ProjectedPoint& a, const ProjectedPoint& b, double amount)
{
    const auto t = static_cast<float>(amount);
    return {a.stereo + t * (b.stereo - a.stereo),
            a.frequency + t * (b.frequency - a.frequency),
            a.depth + t * (b.depth - a.depth)};
}

} // namespace

void ImageSpatializer::prepare(double sampleRate)
{
    sampleRate_ = std::isfinite(sampleRate) && sampleRate > 0.0 ? sampleRate : 48000.0;
    fft_.prepare(kFftSize);
    for (std::size_t i = 0; i < kFftSize; ++i)
        window_[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i)
                                        / static_cast<double>(kFftSize));
    double overlap = 0.0;
    for (std::size_t i = 0; i < kFftSize; i += kHopSize)
        overlap += window_[i] * window_[i];
    const auto scale = 1.0 / std::sqrt(overlap);
    for (auto& value : window_)
        value *= scale;
    frameSmoothing_ = 1.0 - std::exp(-static_cast<double>(kHopSize) / (0.02 * sampleRate_));
    wetRate_ = static_cast<float>(1.0 - std::exp(-1.0 / (0.02 * sampleRate_)));
    reset();
}

void ImageSpatializer::reset()
{
    inputLeft_.fill(0.0f);
    inputRight_.fill(0.0f);
    dryLeft_.fill(0.0f);
    dryRight_.fill(0.0f);
    outputLeft_.fill(0.0);
    outputRight_.fill(0.0);
    spectrum_.fill(std::complex<double> {});
    rows_.fill(RowTarget {});
    smoothedMask_ = {};
    targetMask_ = {};
    targetMaskIdentity_ = nullptr;
    maskPresence_ = targetMaskPresence_ = 0.0;
    smoothedParameters_ = parameters_;
    targetWeights_.fill(0.0);
    targetWeights_[static_cast<std::size_t>(parameters_.target)] = 1.0;
    inputAt_ = dryAt_ = outputAt_ = filled_ = sinceHop_ = 0;
    wetCurrent_ = parameters_.wet;
    safetyResets_ = 0;
}

void ImageSpatializer::setParameters(Parameters parameters)
{
    auto& plane = parameters.plane;
    plane.positionX = std::clamp(finiteOr(plane.positionX, 0.0f), -1.0f, 1.0f);
    plane.positionY = std::clamp(finiteOr(plane.positionY, 0.0f), -1.0f, 1.0f);
    plane.scaleX = std::clamp(finiteOr(plane.scaleX, 1.0f), 0.05f, 2.0f);
    plane.scaleY = std::clamp(finiteOr(plane.scaleY, 1.0f), 0.05f, 2.0f);
    plane.pitchDegrees = std::clamp(finiteOr(plane.pitchDegrees, 0.0f), -180.0f, 180.0f);
    plane.yawDegrees = std::clamp(finiteOr(plane.yawDegrees, 0.0f), -180.0f, 180.0f);
    plane.rollDegrees = std::clamp(finiteOr(plane.rollDegrees, 0.0f), -180.0f, 180.0f);
    parameters.imageAmount = std::clamp(finiteOr(parameters.imageAmount, 0.7f), 0.0f, 1.0f);
    parameters.wet = std::clamp(finiteOr(parameters.wet, 1.0f), 0.0f, 1.0f);
    parameters.target = static_cast<ImageTarget>(
        std::clamp(static_cast<int>(parameters.target), static_cast<int>(ImageTarget::Off),
                   static_cast<int>(ImageTarget::Level)));
    parameters_ = parameters;
}

void ImageSpatializer::process(float* left, float* right, int sampleCount, const ImageMask* mask)
{
    if (left == nullptr || right == nullptr || sampleCount <= 0)
        return;
    constexpr auto outputSize = 2 * kFftSize;
    for (int sample = 0; sample < sampleCount; ++sample) {
        const auto inLeft = finiteOr(left[sample], 0.0f);
        const auto inRight = finiteOr(right[sample], 0.0f);
        if (inLeft != left[sample] || inRight != right[sample])
            ++safetyResets_;

        inputLeft_[inputAt_] = inLeft;
        inputRight_[inputAt_] = inRight;
        inputAt_ = (inputAt_ + 1) % kFftSize;
        filled_ = std::min(kFftSize, filled_ + 1);

        dryLeft_[dryAt_] = inLeft;
        dryRight_[dryAt_] = inRight;
        dryAt_ = (dryAt_ + 1) % kFftSize;
        const auto delayedLeft = dryLeft_[dryAt_];
        const auto delayedRight = dryRight_[dryAt_];

        outputAt_ = (outputAt_ + 1) % outputSize;
        if (++sinceHop_ >= kHopSize && filled_ >= kFftSize) {
            sinceHop_ = 0;
            renderFrame(mask);
        }
        const auto read = (outputAt_ + outputSize - (kFftSize - 1)) % outputSize;
        const auto wetLeft = outputLeft_[read];
        const auto wetRight = outputRight_[read];
        outputLeft_[read] = 0.0;
        outputRight_[read] = 0.0;
        wetCurrent_ += (parameters_.wet - wetCurrent_) * wetRate_;
        const auto outLeft = static_cast<double>(delayedLeft) + static_cast<double>(wetCurrent_)
            * (wetLeft - static_cast<double>(delayedLeft));
        const auto outRight = static_cast<double>(delayedRight) + static_cast<double>(wetCurrent_)
            * (wetRight - static_cast<double>(delayedRight));
        if (std::isfinite(outLeft) && std::isfinite(outRight)
            && std::abs(outLeft) <= kRunaway && std::abs(outRight) <= kRunaway) {
            left[sample] = static_cast<float>(outLeft);
            right[sample] = static_cast<float>(outRight);
        } else {
            left[sample] = right[sample] = 0.0f;
            ++safetyResets_;
        }
    }
}

void ImageSpatializer::renderFrame(const ImageMask* mask)
{
    smoothFrameState(mask);
    const auto projection = projectPlane(smoothedParameters_.plane);
    buildRows(projection);
    for (std::size_t i = 0; i < kFftSize; ++i) {
        const auto at = (inputAt_ + i) % kFftSize;
        spectrum_[i] = {static_cast<double>(inputLeft_[at]) * window_[i],
                        static_cast<double>(inputRight_[at]) * window_[i]};
    }
    fft_.transform(spectrum_, false);

    const auto widthMagnitude = std::clamp(0.5 * static_cast<double>(projection.stereoSpan), 0.0, 2.0);
    const auto signedWidth = projection.signedFacing < 0.0f ? -widthMagnitude : widthMagnitude;
    const auto basePosition = static_cast<double>(smoothedParameters_.plane.positionX);
    const std::complex<double> imaginary {0.0, 1.0};
    const auto half = kFftSize / 2;
    for (std::size_t k = 1; k < half; ++k) {
        const auto packed = spectrum_[k];
        const auto mirrored = std::conj(spectrum_[kFftSize - k]);
        const auto inLeft = 0.5 * (packed + mirrored);
        const auto inRight = -0.5 * imaginary * (packed - mirrored);
        const auto hz = static_cast<double>(k) * sampleRate_ / static_cast<double>(kFftSize);
        const auto frequency = std::clamp(2.0 * std::log(std::max(hz, kLowHz) / kLowHz)
                                              / std::log(kHighHz / kLowHz) - 1.0,
                                          -1.0, 1.0);
        const auto row = rowForFrequency(frequency);
        std::complex<double> outLeft, outRight;
        spatialize(inLeft, inRight, signedWidth, basePosition, row, targetWeights_,
                   smoothedParameters_.imageAmount, outLeft, outRight);
        spectrum_[k] = outLeft + imaginary * outRight;
        spectrum_[kFftSize - k] = std::conj(outLeft) + imaginary * std::conj(outRight);
    }

    for (const auto k : {std::size_t {0}, half}) {
        std::complex<double> outLeft, outRight;
        spatialize({spectrum_[k].real(), 0.0}, {spectrum_[k].imag(), 0.0}, signedWidth,
                   basePosition, rowForFrequency(k == 0 ? -1.0 : 1.0), targetWeights_,
                   smoothedParameters_.imageAmount, outLeft, outRight);
        spectrum_[k] = {outLeft.real(), outRight.real()};
    }

    fft_.transform(spectrum_, true);
    constexpr auto outputSize = 2 * kFftSize;
    const auto inverseScale = 1.0 / static_cast<double>(kFftSize);
    for (std::size_t i = 0; i < kFftSize; ++i) {
        const auto at = (outputAt_ + outputSize - (kFftSize - 1) + i) % outputSize;
        outputLeft_[at] += spectrum_[i].real() * inverseScale * window_[i];
        outputRight_[at] += spectrum_[i].imag() * inverseScale * window_[i];
    }
}

void ImageSpatializer::smoothFrameState(const ImageMask* mask)
{
    if (mask != targetMaskIdentity_) {
        targetMaskIdentity_ = mask;
        targetMask_ = mask != nullptr ? *mask : ImageMask {};
        targetMaskPresence_ = targetMask_.hasSource ? 1.0 : 0.0;
    }
    const auto smooth = [this](auto& current, const auto target) {
        using Value = std::remove_reference_t<decltype(current)>;
        current += static_cast<Value>(frameSmoothing_ * (target - current));
    };
    auto& currentPlane = smoothedParameters_.plane;
    const auto& targetPlane = parameters_.plane;
    smooth(currentPlane.positionX, targetPlane.positionX);
    smooth(currentPlane.positionY, targetPlane.positionY);
    smooth(currentPlane.scaleX, targetPlane.scaleX);
    smooth(currentPlane.scaleY, targetPlane.scaleY);
    smooth(currentPlane.pitchDegrees, targetPlane.pitchDegrees);
    smooth(currentPlane.yawDegrees, targetPlane.yawDegrees);
    smooth(currentPlane.rollDegrees, targetPlane.rollDegrees);
    smooth(smoothedParameters_.imageAmount, parameters_.imageAmount);
    for (std::size_t i = 0; i < smoothedMask_.values.size(); ++i)
        smooth(smoothedMask_.values[i], targetMask_.values[i]);
    smooth(maskPresence_, targetMaskPresence_);
    smoothedMask_.hasSource = maskPresence_ > 1.0e-6;
    const auto selectedTarget = static_cast<std::size_t>(parameters_.target);
    for (std::size_t i = 0; i < targetWeights_.size(); ++i)
        smooth(targetWeights_[i], i == selectedTarget ? 1.0 : 0.0);
}

void ImageSpatializer::buildRows(const ProjectedPlane& projection)
{
    const auto* mask = &smoothedMask_;
    for (int y = 0; y < kImageMaskHeight; ++y) {
        double total = 0.0;
        double weightedX = 0.0;
        for (int x = 0; x < kImageMaskWidth; ++x) {
            const auto value = static_cast<double>(mask->at(x, y));
            const auto localX = 2.0 * static_cast<double>(x) / static_cast<double>(kImageMaskWidth - 1) - 1.0;
            total += value;
            weightedX += value * localX;
        }
        const auto centroid = total > kTiny ? weightedX / total : 0.0;
        double variance = 0.0;
        if (total > kTiny)
            for (int x = 0; x < kImageMaskWidth; ++x) {
                const auto value = static_cast<double>(mask->at(x, y));
                const auto localX = 2.0 * static_cast<double>(x) / static_cast<double>(kImageMaskWidth - 1) - 1.0;
                variance += value * (localX - centroid) * (localX - centroid);
            }
        const auto u = 0.5 * (centroid + 1.0);
        const auto vertical = 1.0 - static_cast<double>(y) / static_cast<double>(kImageMaskHeight - 1);
        const auto bottom = interpolate(projection.corners[0], projection.corners[1], u);
        const auto top = interpolate(projection.corners[3], projection.corners[2], u);
        const auto point = interpolate(bottom, top, vertical);
        const auto leftAtRow = interpolate(projection.corners[0], projection.corners[3], vertical);
        const auto rightAtRow = interpolate(projection.corners[1], projection.corners[2], vertical);
        const auto stereoPerLocalX = 0.5 * static_cast<double>(rightAtRow.stereo - leftAtRow.stereo);
        const auto orientation = projection.signedFacing < 0.0f ? -1.0 : 1.0;
        auto& row = rows_[static_cast<std::size_t>(y)];
        row.frequency = point.frequency;
        row.lateral = point.stereo;
        row.signedSpread = orientation * std::sqrt(total > kTiny ? variance / total : 0.0)
            * std::abs(stereoPerLocalX);
        row.level = total / static_cast<double>(kImageMaskWidth);
        row.presence = maskPresence_;
    }
}

ImageSpatializer::RowTarget ImageSpatializer::rowForFrequency(double normalizedFrequency) const
{
    const RowTarget* nearest = &rows_[0];
    auto distance = std::abs(nearest->frequency - normalizedFrequency);
    for (const auto& row : rows_) {
        const auto candidate = std::abs(row.frequency - normalizedFrequency);
        if (candidate < distance) {
            distance = candidate;
            nearest = &row;
        }
    }
    return *nearest;
}

void ImageSpatializer::spatialize(std::complex<double> left, std::complex<double> right,
                                  double signedWidth, double basePosition, const RowTarget& row,
                                  const std::array<double, 3>& targetWeights, double amount,
                                  std::complex<double>& outputLeft,
                                  std::complex<double>& outputRight)
{
    const auto mid = 0.5 * (left + right);
    const auto side = 0.5 * (left - right);
    const auto brightness = std::clamp(2.0 * row.level, 0.0, 1.0);
    const auto image = std::clamp(amount * row.presence, 0.0, 1.0);
    const auto placeAmount = targetWeights[static_cast<std::size_t>(ImageTarget::Place)]
        * image * brightness;
    const auto lateral = std::clamp(basePosition + placeAmount * (row.lateral - basePosition), -1.0, 1.0);
    auto gainLeft = 1.0 - std::max(0.0, lateral);
    auto gainRight = 1.0 + std::min(0.0, lateral);
    const auto normalization = std::sqrt(std::max(0.5 * (gainLeft * gainLeft + gainRight * gainRight), kTiny));
    gainLeft /= normalization;
    gainRight /= normalization;
    const auto phase = std::clamp(std::numbers::pi * 0.8 * placeAmount * row.signedSpread,
                                  -0.8 * std::numbers::pi, 0.8 * std::numbers::pi);
    const auto placedLeft = mid * std::polar(gainLeft, -0.5 * phase);
    const auto placedRight = mid * std::polar(gainRight, 0.5 * phase);
    const auto levelAmount = targetWeights[static_cast<std::size_t>(ImageTarget::Level)] * image;
    const auto level = 1.0 - levelAmount + levelAmount * brightness;
    outputLeft = level * (placedLeft + signedWidth * side);
    outputRight = level * (placedRight - signedWidth * side);
}

std::uint32_t ImageSpatializer::takeSafetyResets()
{
    const auto resets = safetyResets_;
    safetyResets_ = 0;
    return resets;
}

} // namespace holotoner
