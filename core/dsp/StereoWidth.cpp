#include "StereoWidth.h"

#include <algorithm>
#include <cmath>

namespace holotoner {
namespace {

float finiteOr(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

float signedWidth(const StereoWidth::Parameters& parameters)
{
    const auto projection = projectPlane(parameters.plane);
    const auto magnitude = std::clamp(0.5f * projection.stereoSpan, 0.0f, 2.0f);
    return projection.signedFacing < 0.0f ? -magnitude : magnitude;
}

} // namespace

void StereoWidth::Ramp::prepare(int samples)
{
    length_ = std::max(0, samples);
    remaining_ = 0;
    step_ = 0.0f;
}

void StereoWidth::Ramp::reset(float value)
{
    current_ = value;
    target_ = value;
    step_ = 0.0f;
    remaining_ = 0;
}

void StereoWidth::Ramp::set(float target)
{
    if (target == target_)
        return;
    target_ = target;
    if (length_ == 0) {
        reset(target_);
        return;
    }
    remaining_ = length_;
    step_ = (target_ - current_) / static_cast<float>(remaining_);
}

float StereoWidth::Ramp::next()
{
    if (remaining_ > 0) {
        current_ += step_;
        --remaining_;
        if (remaining_ == 0)
            current_ = target_;
    }
    return current_;
}

void StereoWidth::prepare(double sampleRate, double smoothingMilliseconds)
{
    const auto seconds = std::max(0.0, smoothingMilliseconds) * 0.001;
    const auto smoothingSamples = static_cast<int>(std::ceil(std::max(1.0, sampleRate) * seconds));
    signedWidth_.prepare(smoothingSamples);
    positionX_.prepare(smoothingSamples);
    wet_.prepare(smoothingSamples);
    signedWidth_.reset(signedWidth(parameters_));
    positionX_.reset(std::clamp(parameters_.plane.positionX, -1.0f, 1.0f));
    wet_.reset(std::clamp(parameters_.wet, 0.0f, 1.0f));
    prepared_ = true;
}

void StereoWidth::reset()
{
    signedWidth_.reset(signedWidth(parameters_));
    positionX_.reset(std::clamp(parameters_.plane.positionX, -1.0f, 1.0f));
    wet_.reset(std::clamp(parameters_.wet, 0.0f, 1.0f));
    safetyResets_ = 0;
}

void StereoWidth::setParameters(Parameters parameters)
{
    auto& plane = parameters.plane;
    plane.positionX = std::clamp(finiteOr(plane.positionX, 0.0f), -1.0f, 1.0f);
    plane.positionY = std::clamp(finiteOr(plane.positionY, 0.0f), -1.0f, 1.0f);
    plane.scaleX = std::clamp(finiteOr(plane.scaleX, 1.0f), 0.0f, 2.0f);
    plane.scaleY = std::clamp(finiteOr(plane.scaleY, 1.0f), 0.0f, 2.0f);
    plane.pitchDegrees = std::clamp(finiteOr(plane.pitchDegrees, 0.0f), -180.0f, 180.0f);
    plane.yawDegrees = std::clamp(finiteOr(plane.yawDegrees, 0.0f), -180.0f, 180.0f);
    plane.rollDegrees = std::clamp(finiteOr(plane.rollDegrees, 0.0f), -180.0f, 180.0f);
    parameters.wet = finiteOr(parameters.wet, 1.0f);
    parameters.wet = std::clamp(parameters.wet, 0.0f, 1.0f);
    parameters_ = parameters;

    if (prepared_) {
        signedWidth_.set(signedWidth(parameters_));
        positionX_.set(parameters_.plane.positionX);
        wet_.set(parameters_.wet);
    }
}

void StereoWidth::process(float* left, float* right, int sampleCount)
{
    if (left == nullptr || right == nullptr || sampleCount <= 0)
        return;

    for (int i = 0; i < sampleCount; ++i) {
        const auto dryLeft = finiteOr(left[i], 0.0f);
        const auto dryRight = finiteOr(right[i], 0.0f);
        if (dryLeft != left[i] || dryRight != right[i])
            ++safetyResets_;
        const auto mid = 0.5f * (dryLeft + dryRight);
        const auto side = 0.5f * (dryLeft - dryRight);
        const auto width = signedWidth_.next();
        const auto position = positionX_.next();
        const auto wet = wet_.next();
        const auto leftBalance = 1.0f - std::max(0.0f, position);
        const auto rightBalance = 1.0f + std::min(0.0f, position);
        const auto wetLeft = (mid + width * side) * leftBalance;
        const auto wetRight = (mid - width * side) * rightBalance;
        const auto outputLeft = dryLeft + wet * (wetLeft - dryLeft);
        const auto outputRight = dryRight + wet * (wetRight - dryRight);
        if (std::isfinite(outputLeft) && std::isfinite(outputRight)) {
            left[i] = outputLeft;
            right[i] = outputRight;
        } else {
            left[i] = 0.0f;
            right[i] = 0.0f;
            ++safetyResets_;
        }
    }
}

std::uint32_t StereoWidth::takeSafetyResets()
{
    const auto resets = safetyResets_;
    safetyResets_ = 0;
    return resets;
}

} // namespace holotoner
