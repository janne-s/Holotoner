#include "StereoWidth.h"

#include <algorithm>
#include <cmath>

namespace holotoner {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float finiteOr(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

float signedWidth(const StereoWidth::Parameters& parameters)
{
    const auto yawRadians = std::clamp(parameters.yawDegrees, -180.0f, 180.0f) * kPi / 180.0f;
    return std::clamp(parameters.width, 0.0f, 2.0f) * std::cos(yawRadians);
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
    wet_.prepare(smoothingSamples);
    signedWidth_.reset(signedWidth(parameters_));
    wet_.reset(std::clamp(parameters_.wet, 0.0f, 1.0f));
    prepared_ = true;
}

void StereoWidth::reset()
{
    signedWidth_.reset(signedWidth(parameters_));
    wet_.reset(std::clamp(parameters_.wet, 0.0f, 1.0f));
    safetyResets_ = 0;
}

void StereoWidth::setParameters(Parameters parameters)
{
    parameters.yawDegrees = finiteOr(parameters.yawDegrees, 0.0f);
    parameters.width = finiteOr(parameters.width, 1.0f);
    parameters.wet = finiteOr(parameters.wet, 1.0f);
    parameters.yawDegrees = std::clamp(parameters.yawDegrees, -180.0f, 180.0f);
    parameters.width = std::clamp(parameters.width, 0.0f, 2.0f);
    parameters.wet = std::clamp(parameters.wet, 0.0f, 1.0f);
    parameters_ = parameters;

    if (prepared_) {
        signedWidth_.set(signedWidth(parameters_));
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
        const auto wet = wet_.next();
        const auto wetLeft = mid + width * side;
        const auto wetRight = mid - width * side;
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
