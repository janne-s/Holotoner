#include "core/dsp/PhaseMap.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace holotoner {
ScopePoint phaseMapPoint(const ProjectedPlane& plane, float x, float y, PhaseProjection mode)
{
    const auto u = 0.5f * (x + 1.0f);
    const auto v = 0.5f * (y + 1.0f);
    const auto& a = plane.corners[0];
    const auto& b = plane.corners[1];
    const auto& d = plane.corners[3];
    const auto horizontal = a.stereo + u * (b.stereo - a.stereo) + v * (d.stereo - a.stereo);
    const auto vertical = a.frequency + u * (b.frequency - a.frequency) + v * (d.frequency - a.frequency);
    if (mode == PhaseProjection::Polar) {
        const auto angle = std::clamp(horizontal * 0.5f, -1.0f, 1.0f) * std::numbers::pi_v<float> * 0.5f;
        const auto radius = std::clamp(0.5f + vertical * 0.25f, 0.0f, 1.0f);
        return {radius * std::sin(angle), radius * std::cos(angle)};
    }
    return {horizontal * 0.5f, 0.5f + vertical * 0.5f};
}

ScopePoint stereoToScopePoint(float left, float right)
{
    if (!std::isfinite(left) || !std::isfinite(right))
        return {};
    const auto sum = left + right;
    const auto difference = (right - left) * (sum >= 0.0f ? 1.0f : -1.0f);
    const auto angle = std::atan2(difference, std::abs(sum));
    const auto level = std::hypot(left, right) * std::numbers::sqrt2_v<float> * 0.5f;
    const auto radius = std::clamp(1.0f + std::log10(std::max(level, 1.0e-6f)) / 3.0f, 0.0f, 1.0f);
    return {radius * std::sin(angle), radius * std::cos(angle)};
}
} // namespace holotoner
