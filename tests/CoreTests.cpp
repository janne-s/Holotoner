#include "core/audio/SharedExchange.h"
#include "core/audio/SourcePlayer.h"
#include "core/dsp/StereoWidth.h"
#include "core/geometry/PlaneProjection.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>

namespace {

int failures = 0;

void expectNear(float actual, float expected, float tolerance, std::string_view name)
{
    if (std::abs(actual - expected) <= tolerance)
        return;
    std::cerr << name << ": expected " << expected << ", got " << actual << '\n';
    ++failures;
}

void projectionEndpoints()
{
    holotoner::PlaneTransform transform;
    auto plane = holotoner::projectPlane(transform);
    expectNear(plane.signedFacing, 1.0f, 1.0e-6f, "front facing");
    expectNear(plane.stereoSpan, 2.0f, 1.0e-6f, "front stereo span");
    expectNear(plane.frequencySpan, 2.0f, 1.0e-6f, "front frequency span");

    transform.yawDegrees = 90.0f;
    plane = holotoner::projectPlane(transform);
    expectNear(plane.signedFacing, 0.0f, 1.0e-5f, "edge facing");
    expectNear(plane.stereoSpan, 0.0f, 1.0e-5f, "edge stereo span");

    transform.yawDegrees = 180.0f;
    plane = holotoner::projectPlane(transform);
    expectNear(plane.signedFacing, -1.0f, 1.0e-6f, "back facing");
    expectNear(plane.corners[0].stereo, 1.0f, 1.0e-5f, "back reverses left corner");
    expectNear(plane.corners[1].stereo, -1.0f, 1.0e-5f, "back reverses right corner");

    transform = {};
    transform.pitchDegrees = 90.0f;
    plane = holotoner::projectPlane(transform);
    expectNear(plane.frequencySpan, 0.0f, 1.0e-5f, "pitch edge frequency span");

    transform = {};
    transform.scaleX = 2.0f;
    transform.scaleY = 0.5f;
    transform.rollDegrees = 90.0f;
    plane = holotoner::projectPlane(transform);
    expectNear(plane.stereoSpan, 1.0f, 1.0e-5f, "roll maps height to stereo");
    expectNear(plane.frequencySpan, 4.0f, 1.0e-5f, "roll maps width to frequency");
}

void stereoEndpoints(double sampleRate)
{
    holotoner::StereoWidth engine;
    engine.prepare(sampleRate, 0.0);

    std::array<float, 4> left {1.0f, 0.5f, -0.25f, -1.0f};
    std::array<float, 4> right {-1.0f, -0.5f, 0.25f, 1.0f};
    holotoner::StereoWidth::Parameters parameters;
    parameters.plane.yawDegrees = 90.0f;
    engine.setParameters(parameters);
    engine.process(left.data(), right.data(), static_cast<int>(left.size()));
    for (std::size_t i = 0; i < left.size(); ++i) {
        expectNear(left[i], 0.0f, 1.0e-5f, "edge left is mono mid");
        expectNear(right[i], 0.0f, 1.0e-5f, "edge right is mono mid");
    }

    left = {1.0f, 0.5f, -0.25f, -1.0f};
    right = {-1.0f, -0.5f, 0.25f, 1.0f};
    parameters.plane.yawDegrees = 180.0f;
    engine.setParameters(parameters);
    engine.process(left.data(), right.data(), static_cast<int>(left.size()));
    expectNear(left[0], -1.0f, 1.0e-6f, "back left");
    expectNear(right[0], 1.0f, 1.0e-6f, "back right");

    parameters = {};
    parameters.plane.positionX = 1.0f;
    engine.setParameters(parameters);
    left = {0.5f, 0.5f, 0.5f, 0.5f};
    right = left;
    engine.process(left.data(), right.data(), static_cast<int>(left.size()));
    expectNear(left[0], 0.0f, 1.0e-6f, "right position removes left wet channel");
    expectNear(right[0], 0.5f, 1.0e-6f, "right position keeps right wet channel");
}

void blockPartitionIsIndependent()
{
    constexpr std::size_t count = 2048;
    std::array<float, count> wholeLeft {};
    std::array<float, count> wholeRight {};
    for (std::size_t i = 0; i < count; ++i) {
        wholeLeft[i] = static_cast<float>(std::sin(static_cast<double>(i) * 0.071));
        wholeRight[i] = static_cast<float>(std::cos(static_cast<double>(i) * 0.053));
    }
    auto splitLeft = wholeLeft;
    auto splitRight = wholeRight;

    holotoner::StereoWidth whole;
    holotoner::StereoWidth split;
    whole.prepare(48000.0, 20.0);
    split.prepare(48000.0, 20.0);
    holotoner::StereoWidth::Parameters target;
    target.plane.positionX = 0.35f;
    target.plane.scaleX = 1.7f;
    target.plane.yawDegrees = 173.0f;
    target.wet = 0.83f;
    whole.setParameters(target);
    split.setParameters(target);
    whole.process(wholeLeft.data(), wholeRight.data(), static_cast<int>(count));

    constexpr std::array<int, 3> blocks {1, 97, 512};
    std::size_t at = 0;
    std::size_t block = 0;
    while (at < count) {
        const auto available = count - at;
        const auto wanted = static_cast<std::size_t>(blocks[block % blocks.size()]);
        const auto samples = std::min(available, wanted);
        split.process(splitLeft.data() + at, splitRight.data() + at, static_cast<int>(samples));
        at += samples;
        ++block;
    }

    for (std::size_t i = 0; i < count; ++i) {
        expectNear(splitLeft[i], wholeLeft[i], 0.0f, "block-independent left");
        expectNear(splitRight[i], wholeRight[i], 0.0f, "block-independent right");
    }
}

void dryIsExact()
{
    holotoner::StereoWidth engine;
    engine.prepare(44100.0, 0.0);
    holotoner::StereoWidth::Parameters parameters;
    parameters.plane.yawDegrees = 137.0f;
    parameters.plane.scaleX = 2.0f;
    parameters.wet = 0.0f;
    engine.setParameters(parameters);
    std::array<float, 3> left {0.2f, -0.4f, 0.9f};
    std::array<float, 3> right {-0.7f, 0.3f, 0.1f};
    const auto originalLeft = left;
    const auto originalRight = right;
    engine.process(left.data(), right.data(), static_cast<int>(left.size()));
    for (std::size_t i = 0; i < left.size(); ++i) {
        expectNear(left[i], originalLeft[i], 0.0f, "dry left exact");
        expectNear(right[i], originalRight[i], 0.0f, "dry right exact");
    }
}

void invalidValuesAreContained()
{
    holotoner::StereoWidth engine;
    engine.prepare(48000.0, 0.0);
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    holotoner::StereoWidth::Parameters parameters;
    parameters.plane.positionX = nan;
    parameters.plane.scaleX = nan;
    parameters.plane.yawDegrees = nan;
    parameters.wet = nan;
    engine.setParameters(parameters);
    std::array<float, 2> left {nan, std::numeric_limits<float>::max()};
    std::array<float, 2> right {0.25f, -std::numeric_limits<float>::max()};
    engine.process(left.data(), right.data(), static_cast<int>(left.size()));
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (!std::isfinite(left[i]) || !std::isfinite(right[i])) {
            std::cerr << "invalid sample escaped safety at " << i << '\n';
            ++failures;
        }
    }
    if (engine.takeSafetyResets() == 0) {
        std::cerr << "invalid samples did not register a safety reset\n";
        ++failures;
    }

    holotoner::PlaneTransform transform;
    transform.positionX = nan;
    transform.scaleY = nan;
    transform.yawDegrees = nan;
    const auto plane = holotoner::projectPlane(transform);
    for (const auto& corner : plane.corners) {
        if (!std::isfinite(corner.stereo) || !std::isfinite(corner.frequency) || !std::isfinite(corner.depth)) {
            std::cerr << "invalid transform escaped projection safety\n";
            ++failures;
            break;
        }
    }
}

void sourcePlayerIsDeterministic()
{
    holotoner::SourcePlayer player;
    player.prepare(8.0);
    std::array<float, 8> left {};
    std::array<float, 8> right {};
    player.render(holotoner::SourceKind::Clicks, nullptr, left.data(), right.data(), 4);
    player.render(holotoner::SourceKind::Clicks, nullptr, left.data() + 4, right.data() + 4, 4);
    expectNear(left[0], 1.0f, 0.0f, "first click");
    expectNear(left[6], 1.0f, 0.0f, "second click across blocks");
    for (std::size_t i = 0; i < left.size(); ++i)
        expectNear(right[i], left[i], 0.0f, "clicks are stereo-identical");

    holotoner::SourceAudio file;
    file.sampleRate = 4.0;
    file.left = {0.0f, 1.0f, 0.0f, -1.0f};
    file.right = {1.0f, 0.0f, -1.0f, 0.0f};
    player.prepare(8.0);
    player.render(holotoner::SourceKind::File, &file, left.data(), right.data(), 4);
    expectNear(left[0], 0.0f, 0.0f, "file starts at beginning");
    expectNear(left[1], 0.5f, 1.0e-6f, "file resamples linearly");
    expectNear(left[2], 1.0f, 1.0e-6f, "file reaches source sample");

    left.fill(0.25f);
    right.fill(-0.25f);
    player.render(holotoner::SourceKind::Input, nullptr, left.data(), right.data(), 8);
    expectNear(left[7], 0.25f, 0.0f, "input remains untouched");
    expectNear(right[7], -0.25f, 0.0f, "input right remains untouched");
}

void sharedExchangeRetiresOffAudioPath()
{
    holotoner::SharedExchange<int> exchange;
    auto first = std::make_shared<const int>(7);
    exchange.publish(first);
    if (exchange.acquire() == nullptr || *exchange.acquire() != 7) {
        std::cerr << "shared exchange did not publish first value\n";
        ++failures;
    }
    exchange.publish(std::make_shared<const int>(9));
    if (exchange.acquire() == nullptr || *exchange.acquire() != 9) {
        std::cerr << "shared exchange did not publish replacement\n";
        ++failures;
    }
    exchange.collectGarbage();
    if (exchange.currentShared() == nullptr || *exchange.currentShared() != 9) {
        std::cerr << "shared exchange collected current value\n";
        ++failures;
    }
}

} // namespace

int main()
{
    projectionEndpoints();
    stereoEndpoints(44100.0);
    stereoEndpoints(48000.0);
    stereoEndpoints(96000.0);
    blockPartitionIsIndependent();
    dryIsExact();
    invalidValuesAreContained();
    sourcePlayerIsDeterministic();
    sharedExchangeRetiresOffAudioPath();
    if (failures != 0)
        std::cerr << failures << " test assertion(s) failed\n";
    return failures == 0 ? 0 : 1;
}
