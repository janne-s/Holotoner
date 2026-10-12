#include "core/dsp/ImageSpatializer.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <type_traits>

namespace holotoner {
namespace {

constexpr double kHighHz = 16000.0;
constexpr double kTiny = 1.0e-12;
constexpr double kRunaway = 16.0;

float finiteOr(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

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
    targetMask_ = {};
    targetMaskIdentity_ = nullptr;
    maskPresence_ = targetMaskPresence_ = 0.0;
    smoothedParameters_ = parameters_;
    inputAt_ = dryAt_ = outputAt_ = filled_ = sinceHop_ = 0;
    wetCurrent_ = parameters_.wet;
    safetyResets_ = 0;
    phaseControls_.fill({});
    phaseInitialized_ = false;
    fieldTime_ = 0.0;
    steeringBlend_ = 1.0f;
    for (int row = 0; row < ImageMask::kFieldRows; ++row)
        for (int column = 0; column < ImageMask::kFieldAngles; ++column) {
            const auto at = static_cast<std::size_t>(row*ImageMask::kFieldAngles+column);
            steering_[at] = -0.5f*std::numbers::pi_v<float> + std::numbers::pi_v<float>
                *static_cast<float>(column)/static_cast<float>(ImageMask::kFieldAngles-1);
        }
    previousSteering_ = steering_;

}

void ImageSpatializer::setParameters(Parameters parameters)
{
    auto& plane = parameters.plane;
    plane.positionX = std::clamp(finiteOr(plane.positionX, 0.0f), -1.0f, 1.0f);
    plane.positionY = std::clamp(finiteOr(plane.positionY, 0.0f), -1.0f, 1.0f);
    plane.scaleX = std::clamp(finiteOr(plane.scaleX, 1.0f), 0.05f, 4.0f);
    plane.scaleY = std::clamp(finiteOr(plane.scaleY, 1.0f), 0.05f, 4.0f);
    plane.pitchDegrees = std::clamp(finiteOr(plane.pitchDegrees, 0.0f), -180.0f, 180.0f);
    plane.yawDegrees = std::clamp(finiteOr(plane.yawDegrees, 0.0f), -180.0f, 180.0f);
    plane.rollDegrees = std::clamp(finiteOr(plane.rollDegrees, 0.0f), -180.0f, 180.0f);
    parameters.imageAmount = std::clamp(finiteOr(parameters.imageAmount, 1.0f), 0.0f, 1.0f);
    parameters.wet = std::clamp(finiteOr(parameters.wet, 1.0f), 0.0f, 1.0f);
    parameters.phaseProjection = parameters.phaseProjection == PhaseProjection::Polar
        ? PhaseProjection::Polar : PhaseProjection::Cartesian;
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
        auto wetLeft = outputLeft_[read];
        auto wetRight = outputRight_[read];
        outputLeft_[read] = 0.0;
        outputRight_[read] = 0.0;
        constrainField(wetLeft, wetRight);
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
    fieldTime_ += static_cast<double>(kHopSize) / sampleRate_;
    smoothFrameState(mask);
    const auto projection = projectPlane(smoothedParameters_.plane);
    auto phaseTransform = smoothedParameters_.plane;
    phaseTransform.scaleX *= targetMask_.aspectX;
    phaseTransform.scaleY *= targetMask_.aspectY;
    buildPhaseControls(projectPlane(phaseTransform));
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
        std::complex<double> outLeft, outRight;
        spatialize(inLeft, inRight, signedWidth, basePosition, outLeft, outRight);
        applyPhase(k, inLeft, inRight, signedWidth, outLeft, outRight);
        spectrum_[k] = outLeft + imaginary * outRight;
        spectrum_[kFftSize - k] = std::conj(outLeft) + imaginary * std::conj(outRight);
    }

    for (const auto k : {std::size_t {0}, half}) {
        std::complex<double> outLeft, outRight;
        spatialize({spectrum_[k].real(), 0.0}, {spectrum_[k].imag(), 0.0}, signedWidth,
                   basePosition, outLeft, outRight);
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
    previousSteering_ = steering_;
    steeringBlend_ = 0.0f;
    if (targetMask_.fieldReady)
        for (std::size_t i = 0; i < steering_.size(); ++i)
            smooth(steering_[i], targetMask_.fieldAngles[i]);
    smooth(maskPresence_, targetMaskPresence_);
}

void ImageSpatializer::constrainField(double& left, double& right)
{
    const auto mid = 0.5*(left+right), side = 0.5*(right-left);
    const auto level = std::hypot(mid, side);
    const auto amount = maskPresence_*smoothedParameters_.imageAmount;
    steeringBlend_ = std::min(1.0f, steeringBlend_ + 1.0f/static_cast<float>(kHopSize));
    if (level < 1.0e-9 || amount < 1.0e-6 || !targetMask_.fieldReady)
        return;
    const auto polarity = mid >= 0.0 ? 1.0 : -1.0;
    const auto angle = std::atan2(polarity*side, std::abs(mid));
    // Same calibrated level axis as the measured +14 dB scope. No audio gain is applied.
    const auto radius = std::clamp(1.0 + std::log10(std::max(level*5.0, 1.0e-6))/3.0, 0.0, 1.0);
    const auto rowPosition = std::clamp(radius*ImageMask::kFieldRows-0.5, 0.0,
                                      static_cast<double>(ImageMask::kFieldRows-1));
    const auto columnPosition = std::clamp((angle/std::numbers::pi+0.5)
        *static_cast<double>(ImageMask::kFieldAngles-1), 0.0, static_cast<double>(ImageMask::kFieldAngles-1));
    const auto row = static_cast<int>(rowPosition), column = static_cast<int>(columnPosition);
    const auto nextRow = std::min(row+1, ImageMask::kFieldRows-1);
    const auto nextColumn = std::min(column+1, ImageMask::kFieldAngles-1);
    const auto readAngle = [this](int r, int c) {
        const auto at = static_cast<std::size_t>(r*ImageMask::kFieldAngles+c);
        return static_cast<double>(previousSteering_[at] + steeringBlend_*(steering_[at]-previousSteering_[at]));
    };
    const auto tx = columnPosition-column, ty = rowPosition-row;
    const auto top = readAngle(row,column)*(1-tx)+readAngle(row,nextColumn)*tx;
    const auto bottom = readAngle(nextRow,column)*(1-tx)+readAngle(nextRow,nextColumn)*tx;
    // Avoid a waveform discontinuity at Polar Sample's polarity-fold seam.
    const auto seam = std::min(1.0, std::abs(mid)/(0.05*level));
    const auto rotation = amount*seam*(top*(1-ty)+bottom*ty-angle);
    const auto c = std::cos(rotation), s = std::sin(rotation);
    // Rotate the EXISTING sample vector: L²+R² is unchanged at every sample.
    // No pixel-derived amplitude, envelope, sign-only carrier or silent-row gating.
    const auto rotatedMid = c*mid-s*side;
    const auto rotatedSide = s*mid+c*side;
    left = rotatedMid-rotatedSide;
    right = rotatedMid+rotatedSide;
}

void ImageSpatializer::buildPhaseControls(const ProjectedPlane& projection)
{
    const auto smooth = phaseInitialized_ ? frameSmoothing_ : 1.0;
    for (std::size_t i = 0; i < kPhaseAnchors; ++i) {
        // Fixed frequency assignment, never advanced by time, pitch or source level.
        const auto index = (i * 4051 + 2053) % ImageMask::kPhasePointCount;
        const auto& pixel = targetMask_.phasePoints[index];
        const auto point = phaseMapPoint(projection, pixel.x, pixel.y, parameters_.phaseProjection);
        const auto radius = std::hypot(point.x, point.y);
        PhaseControls target;
        if (targetMask_.hasPhasePoints && point.y >= 0.0f && radius <= 1.0f) {
            target.angle = std::clamp(static_cast<double>(std::atan2(point.x, std::max(point.y, 0.08f))),
                                      -1.25, 1.25);
            target.relative = std::numbers::pi * 0.75 * std::clamp(1.0 - radius, 0.0, 1.0);
            target.common = std::numbers::pi * 0.6 * (radius - 0.5);
            // Spectral Space's per-bin phase drift, bounded and independent of pitch/level.
            const auto offset = static_cast<double>(i) * 2.399963229728653;
            target.relative += 0.3 * std::sin(2.0*std::numbers::pi*0.17*fieldTime_ + offset);
            target.common += 0.15 * std::sin(2.0*std::numbers::pi*0.11*fieldTime_ + offset);
        }
        auto& current = phaseControls_[i];
        current.angle += smooth * (target.angle - current.angle);
        current.relative += smooth * (target.relative - current.relative);
        current.common += smooth * (target.common - current.common);
    }
    phaseInitialized_ = true;
}

void ImageSpatializer::applyPhase(std::size_t bin, std::complex<double> left, std::complex<double> right,
                                  double signedWidth, std::complex<double>& outLeft,
                                  std::complex<double>& outRight) const
{
    const auto amount = maskPresence_ * smoothedParameters_.imageAmount;
    if (amount < 1.0e-9)
        return;
    const auto hz = static_cast<double>(bin) * sampleRate_ / static_cast<double>(kFftSize);
    const auto position = std::min(1.0, hz / kHighHz) * static_cast<double>(kPhaseAnchors - 1);
    const auto at = std::min(static_cast<std::size_t>(position), kPhaseAnchors - 2);
    const auto t = position - static_cast<double>(at);
    const auto& a = phaseControls_[at];
    const auto& b = phaseControls_[at + 1];
    const auto baseAngle = 0.5 * std::asin(std::clamp(
        static_cast<double>(smoothedParameters_.plane.positionX), -1.0, 1.0));
    const auto angle = baseAngle + amount * (a.angle + t * (b.angle - a.angle) - baseAngle);
    const auto relative = amount * (a.relative + t * (b.relative - a.relative));
    const auto common = amount * (a.common + t * (b.common - a.common));
    const auto c = std::cos(angle);
    const auto s = std::sin(angle);
    const auto phase = std::polar(1.0, relative);
    const auto mid = 0.5 * (left + right);
    const auto side = 0.5 * signedWidth * (right - left);
    // Unitary rotation: no pixel-derived gain envelope and no replacement carrier.
    const auto mappedMid = c * mid - s * std::conj(phase) * side;
    const auto mappedSide = s * phase * mid + c * side;
    const auto sharedPhase = std::polar(1.0, common);
    const auto mappedLeft = sharedPhase * (mappedMid - mappedSide);
    const auto mappedRight = sharedPhase * (mappedMid + mappedSide);
    outLeft = mappedLeft;
    outRight = mappedRight;
}

void ImageSpatializer::spatialize(std::complex<double> left, std::complex<double> right,
                                  double signedWidth, double basePosition,
                                  std::complex<double>& outputLeft, std::complex<double>& outputRight)
{
    const auto mid = 0.5 * (left + right);
    const auto side = 0.5 * (left - right);
    const auto lateral = std::clamp(basePosition, -1.0, 1.0);
    auto gainLeft = 1.0 - std::max(0.0, lateral);
    auto gainRight = 1.0 + std::min(0.0, lateral);
    const auto normalization = std::sqrt(std::max(0.5 * (gainLeft*gainLeft + gainRight*gainRight), kTiny));
    gainLeft /= normalization;
    gainRight /= normalization;
    outputLeft = mid*gainLeft + signedWidth*side;
    outputRight = mid*gainRight - signedWidth*side;
}

std::uint32_t ImageSpatializer::takeSafetyResets()
{
    const auto resets = safetyResets_;
    safetyResets_ = 0;
    return resets;
}

} // namespace holotoner
