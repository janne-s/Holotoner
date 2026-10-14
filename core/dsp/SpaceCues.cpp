#include "core/dsp/SpaceCues.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace holotoner {
namespace {

constexpr double kInterferePerUnit = 0.008; // seconds of path difference per plane unit
constexpr double kMaxInterfere = 0.010;
constexpr double kResonanceWidth = 1.0 / 24.0; // octaves: a quarter tone
constexpr double kOff = 1.0e-6;

double smoothstep(double edge0, double edge1, double x)
{
    const auto t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double reach(const ProjectedPoint& point)
{
    const auto dx = static_cast<double>(point.stereo);
    const auto dy = static_cast<double>(point.frequency);
    const auto dz = kListenerDistance - static_cast<double>(point.depth);
    return std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 0.05);
}

double hzOfHeight(double y)
{
    // The plane's vertical extent -1…+1 spans the same log range as the rows.
    return kSpaceLowHz * std::pow(kSpaceHighHz / kSpaceLowHz, std::clamp(0.5 * (y + 1.0), -0.5, 1.5));
}

} // namespace

bool spaceActive(const SpaceAmounts& amounts)
{
    return amounts.interfere > kOff || amounts.echo > kOff || amounts.resonate > kOff;
}

double spaceRowPosition(double hz)
{
    return std::clamp(std::log(std::max(hz, kSpaceLowHz) / kSpaceLowHz) / std::log(kSpaceHighHz / kSpaceLowHz), 0.0, 1.0);
}

std::array<double, kImageMaskHeight> spaceRowWeights(const ImageMask& mask)
{
    std::array<double, kImageMaskHeight> weights {};
    double brightest = 0.0;
    for (int y = 0; y < kImageMaskHeight; ++y) {
        double sum = 0.0;
        for (int x = 0; x < kImageMaskWidth; ++x)
            sum += static_cast<double>(mask.values[static_cast<std::size_t>(y * kImageMaskWidth + x)]);
        weights[static_cast<std::size_t>(y)] = sum;
        brightest = std::max(brightest, sum);
    }
    for (auto& weight : weights)
        weight = mask.hasSource && brightest > 1.0e-12 ? weight / brightest : 0.0;
    return weights;
}

double spaceRowWeight(const std::array<double, kImageMaskHeight>& weights, double hz)
{
    const auto row = (1.0 - spaceRowPosition(hz)) * static_cast<double>(kImageMaskHeight - 1);
    const auto r0 = std::min(static_cast<std::size_t>(row), static_cast<std::size_t>(kImageMaskHeight - 2));
    const auto t = row - static_cast<double>(r0);
    return weights[r0] + t * (weights[r0 + 1] - weights[r0]);
}

EarResponse interfereResponse(const ProjectedPlane& plane, const SpaceAmounts& amounts, double hz)
{
    EarResponse response;
    if (amounts.interfere <= kOff)
        return response;
    const auto v = static_cast<float>(spaceRowPosition(hz));
    const auto leftHalf = reach(planePoint(plane, 0.25f, v));
    const auto rightHalf = reach(planePoint(plane, 0.75f, v));
    const auto feedback = kMaxFeedback * amounts.feedback * amounts.interfere;
    const auto ear = [&](double near, double far) {
        const auto difference = std::clamp((far - near) * kInterferePerUnit, -kMaxInterfere, kMaxInterfere);
        const auto delayed = std::polar(1.0, -2.0 * std::numbers::pi * hz * difference);
        // Feed-forward comb, then the same delay fed back: peaks stay at unity, notches deepen.
        return (1.0 + amounts.interfere * delayed) / (1.0 + amounts.interfere)
             * (1.0 - feedback) / (1.0 - feedback * delayed);
    };
    response.left = ear(leftHalf, rightHalf);
    response.right = ear(rightHalf, leftHalf);
    return response;
}

double echoSeconds(const ProjectedPlane& plane, double hz, double base)
{
    const auto r = reach(planePoint(plane, 0.5f, static_cast<float>(spaceRowPosition(hz))));
    return std::clamp(base * std::pow(r / kListenerDistance, 2.0), kEchoMinSeconds, kEchoMaxSeconds);
}

Resonators resonators(const ProjectedPlane& plane, const std::array<double, kImageMaskHeight>& weights)
{
    Resonators result;
    for (int row = 0; row < kImageMaskHeight; ++row) {
        // Sharp: only clearly bright rows ring.
        const auto weight = smoothstep(0.2, 0.4, weights[static_cast<std::size_t>(row)]);
        if (weight <= 1.0e-3)
            continue;
        const auto v = 1.0f - static_cast<float>(row) / static_cast<float>(kImageMaskHeight - 1);
        const auto at = static_cast<std::size_t>(result.count++);
        result.hz[at] = hzOfHeight(static_cast<double>(planePoint(plane, 0.5f, v).frequency));
        result.weight[at] = weight;
    }
    return result;
}

double resonance(const Resonators& resonators, double hz)
{
    double strongest = 0.0;
    for (int i = 0; i < resonators.count; ++i) {
        const auto index = static_cast<std::size_t>(i);
        const auto octaves = std::log2(std::max(hz, 1.0) / resonators.hz[index]) / kResonanceWidth;
        strongest = std::max(strongest, resonators.weight[index] * std::exp(-octaves * octaves));
    }
    return strongest;
}

} // namespace holotoner
