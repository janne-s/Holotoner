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
    sourceRelease_ = static_cast<float>(std::exp(-1.0 / (0.06 * sampleRate_)));
    dcPole_ = static_cast<float>(std::exp(-2.0 * std::numbers::pi * 5.0 / sampleRate_));
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
    analysis_ = {};
    inputAt_ = dryAt_ = outputAt_ = filled_ = sinceHop_ = 0;
    wetCurrent_ = parameters_.wet;
    safetyResets_ = 0;
    scanPhase_ = 0.0;
    scanPresence_ = sourceEnvelope_ = dcLeft_ = dcRight_ = 0.0f;
    previousPhaseLeft_ = previousPhaseRight_ = 0.0f;
    polarMix_ = parameters_.phaseProjection == PhaseProjection::Polar ? 1.0f : 0.0f;
    phasePlane_ = projectPlane(parameters_.plane);
    phasePlaneCurrent_ = phasePlane_;
    previousScan_.fill({});
    scanFrameBlend_ = 1.0f;
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
    parameters.target = static_cast<ImageTarget>(
        std::clamp(static_cast<int>(parameters.target), static_cast<int>(ImageTarget::Off),
                   static_cast<int>(ImageTarget::Phase)));
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
        const auto sourceMagnitude = std::max(std::abs(delayedLeft), std::abs(delayedRight));
        sourceEnvelope_ = std::max(sourceMagnitude, sourceEnvelope_ * sourceRelease_);
        // The source drives scan travel and polarity; silence cannot generate sound.
        scanPhase_ += 40.0 * static_cast<double>(sourceMagnitude) / sampleRate_;
        scanPhase_ -= std::floor(scanPhase_);
        const auto scanPosition = (scanPhase_ < 0.5 ? 2.0 * scanPhase_ : 2.0 * (1.0 - scanPhase_))
            * static_cast<double>(ImageMask::kScanSize - 1);
        const auto at = static_cast<std::size_t>(scanPosition);
        const auto next = std::min(at + 1, ImageMask::kScanSize - 1);
        const auto fraction = static_cast<float>(scanPosition - static_cast<double>(at));
        const auto& a = smoothedMask_.scan[at];
        const auto& b = smoothedMask_.scan[next];
        const auto& oldA = previousScan_[at];
        const auto& oldB = previousScan_[next];
        const auto oldX = oldA.x + fraction * (oldB.x - oldA.x);
        const auto oldY = oldA.y + fraction * (oldB.y - oldA.y);
        scanFrameBlend_ = std::min(1.0f, scanFrameBlend_ + 1.0f / static_cast<float>(kHopSize));
        const auto x = oldX + scanFrameBlend_ * (a.x + fraction * (b.x - a.x) - oldX);
        const auto y = oldY + scanFrameBlend_ * (a.y + fraction * (b.y - a.y) - oldY);
        for (std::size_t corner = 0; corner < phasePlane_.corners.size(); ++corner) {
            auto& current = phasePlaneCurrent_.corners[corner];
            const auto& target = phasePlane_.corners[corner];
            current.stereo += wetRate_ * (target.stereo - current.stereo);
            current.frequency += wetRate_ * (target.frequency - current.frequency);
        }
        polarMix_ += wetRate_ * ((parameters_.phaseProjection == PhaseProjection::Polar ? 1.0f : 0.0f) - polarMix_);
        const auto cartesian = scopePointToStereo(phaseMapPoint(phasePlaneCurrent_, x, y, PhaseProjection::Cartesian));
        const auto polar = scopePointToStereo(phaseMapPoint(phasePlaneCurrent_, x, y, PhaseProjection::Polar));
        const auto carrier = std::abs(delayedLeft) >= std::abs(delayedRight) ? delayedLeft : delayedRight;
        const auto gain = sourceMagnitude > 1.0e-7f
            ? std::min(sourceEnvelope_, 1.0f) * (carrier >= 0.0f ? 1.0f : -1.0f) * scanPresence_ : 0.0f;
        const auto phaseLeft = gain * (cartesian.left + polarMix_ * (polar.left - cartesian.left));
        const auto phaseRight = gain * (cartesian.right + polarMix_ * (polar.right - cartesian.right));
        dcLeft_ = phaseLeft - previousPhaseLeft_ + dcPole_ * dcLeft_;
        dcRight_ = phaseRight - previousPhaseRight_ + dcPole_ * dcRight_;
        previousPhaseLeft_ = phaseLeft;
        previousPhaseRight_ = phaseRight;
        const auto phaseWeight = targetWeights_[3] * maskPresence_ * smoothedParameters_.imageAmount;
        wetLeft += phaseWeight * (static_cast<double>(dcLeft_) - wetLeft);
        wetRight += phaseWeight * (static_cast<double>(dcRight_) - wetRight);
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
    auto phaseTransform = smoothedParameters_.plane;
    phaseTransform.scaleX *= targetMask_.aspectX;
    phaseTransform.scaleY *= targetMask_.aspectY;
    phasePlane_ = projectPlane(phaseTransform);
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
    std::array<double, kAnalysisBins> analysisLeft {};
    std::array<double, kAnalysisBins> analysisRight {};
    std::array<double, kAnalysisBins> analysisCross {};
    std::array<std::size_t, kAnalysisBins> analysisCount {};
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
        const auto analysisWet = static_cast<double>(wetCurrent_);
        const auto analysisOutLeft = inLeft + analysisWet * (outLeft - inLeft);
        const auto analysisOutRight = inRight + analysisWet * (outRight - inRight);
        const auto analysisAt = std::min(
            kAnalysisBins - 1,
            static_cast<std::size_t>(std::clamp(0.5 * (frequency + 1.0), 0.0, 1.0)
                                     * static_cast<double>(kAnalysisBins)));
        analysisLeft[analysisAt] += std::norm(analysisOutLeft);
        analysisRight[analysisAt] += std::norm(analysisOutRight);
        analysisCross[analysisAt] += std::real(analysisOutLeft * std::conj(analysisOutRight));
        ++analysisCount[analysisAt];
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
    constexpr double spectrumToAmplitude = 2.0 / static_cast<double>(kFftSize);
    constexpr float analysisDecay = 0.82f;
    constexpr float directionSmoothing = 0.35f;
    for (std::size_t i = 0; i < kAnalysisBins; ++i) {
        const auto count = static_cast<double>(std::max<std::size_t>(analysisCount[i], 1));
        const auto leftPower = analysisLeft[i] / count;
        const auto rightPower = analysisRight[i] / count;
        const auto leftLevel = std::sqrt(leftPower);
        const auto rightLevel = std::sqrt(rightPower);
        const auto measuredLevel = static_cast<float>(
            spectrumToAmplitude * std::sqrt(0.5 * (leftPower + rightPower)));
        analysis_.level[i] = std::max(measuredLevel, analysisDecay * analysis_.level[i]);
        if (leftLevel + rightLevel > kTiny) {
            const auto measuredBalance = static_cast<float>(
                std::clamp((rightLevel - leftLevel) / (rightLevel + leftLevel), -1.0, 1.0));
            const auto denominator = std::sqrt(leftPower * rightPower);
            const auto measuredCorrelation = static_cast<float>(
                denominator > kTiny ? std::clamp(analysisCross[i] / count / denominator, -1.0, 1.0) : 0.0);
            analysis_.balance[i] += directionSmoothing * (measuredBalance - analysis_.balance[i]);
            analysis_.correlation[i] += directionSmoothing
                * (measuredCorrelation - analysis_.correlation[i]);
        }
    }
    ++analysis_.revision;
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
    previousScan_ = smoothedMask_.scan;
    scanFrameBlend_ = 0.0f;
    if (targetMask_.hasScan)
        for (std::size_t i = 0; i < ImageMask::kScanSize; ++i) {
            smooth(smoothedMask_.scan[i].x, targetMask_.scan[i].x);
            smooth(smoothedMask_.scan[i].y, targetMask_.scan[i].y);
        }
    smooth(scanPresence_, targetMask_.hasScan ? 1.0f : 0.0f);
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
                                  const std::array<double, 4>& targetWeights, double amount,
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
