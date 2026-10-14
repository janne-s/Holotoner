#include "core/audio/SharedExchange.h"
#include "core/audio/SourcePlayer.h"
#include "core/dsp/Fft.h"
#include "core/dsp/ImageSpatializer.h"
#include "core/dsp/StereoWidth.h"
#include "core/geometry/PlaneProjection.h"
#include "core/geometry/Motion.h"
#include "core/image/ImageMask.h"
#include "core/image/ShapeGenerator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

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

void imageMaskIsDeterministic()
{
    holotoner::ImageMaskSettings settings;
    settings.channel = holotoner::PixelChannel::Luma;
    const auto empty = holotoner::buildImageMask({}, settings);
    if (empty.hasSource) {
        std::cerr << "invalid image mask reports a source\n";
        ++failures;
    }
    for (const auto value : empty.values)
        expectNear(value, 0.0f, 0.0f, "invalid image produces an empty mask");

    holotoner::RgbaImage image;
    image.width = 2;
    image.height = 2;
    image.pixels = {
        255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 255, 255,   255, 255, 255, 0,
    };

    settings.channel = holotoner::PixelChannel::Red;
    auto mask = holotoner::buildImageMask(image, settings);
    if (!mask.hasSource) {
        std::cerr << "valid image mask lost its source marker\n";
        ++failures;
    }
    expectNear(mask.at(0, 0), 1.0f, 1.0e-6f, "red mask reads red");
    expectNear(mask.at(63, 0), 0.0f, 1.0e-6f, "red mask rejects green");
    expectNear(mask.at(63, 63), 0.0f, 1.0e-6f, "transparent pixel stays empty");

    settings.fill = 0.4f;
    mask = holotoner::buildImageMask(image, settings);
    expectNear(mask.at(63, 0), 0.4f, 1.0e-6f, "fill raises opaque pixel");
    expectNear(mask.at(63, 63), 0.0f, 1.0e-6f, "fill never fills transparency");

    settings = {};
    settings.channel = holotoner::PixelChannel::Luma;
    settings.normalize = true;
    settings.invert = true;
    settings.amount = 0.5f;
    mask = holotoner::buildImageMask(image, settings);
    expectNear(mask.at(63, 63), 0.0f, 1.0e-6f, "normalized invert preserves transparency");
    for (const auto value : mask.values)
        if (!std::isfinite(value) || value < 0.0f || value > 0.5f) {
            std::cerr << "image mask escaped bounds\n";
            ++failures;
            break;
        }
}

void imageSpatializerIsBlockIndependent()
{
    constexpr std::size_t count = 8192;
    std::array<float, count> wholeLeft {};
    std::array<float, count> wholeRight {};
    for (std::size_t i = 0; i < count; ++i) {
        wholeLeft[i] = static_cast<float>(0.3 * std::sin(static_cast<double>(i) * 0.071));
        wholeRight[i] = static_cast<float>(0.2 * std::cos(static_cast<double>(i) * 0.053));
    }
    auto splitLeft = wholeLeft;
    auto splitRight = wholeRight;

    holotoner::ImageMask mask;
    mask.hasSource = true;
    for (int y = 0; y < holotoner::kImageMaskHeight; ++y)
        for (int x = 0; x < holotoner::kImageMaskWidth; ++x)
            mask.values[static_cast<std::size_t>(y * holotoner::kImageMaskWidth + x)]
                = x > y / 2 ? 0.75f : 0.1f;

    holotoner::ImageSpatializer::Parameters parameters;
    parameters.plane.positionX = 0.2f;
    parameters.plane.yawDegrees = 32.0f;
    parameters.imageAmount = 0.85f;
    parameters.wet = 0.9f;
    holotoner::ImageSpatializer whole;
    holotoner::ImageSpatializer split;
    whole.setParameters(parameters);
    split.setParameters(parameters);
    whole.prepare(48000.0);
    split.prepare(48000.0);
    whole.process(wholeLeft.data(), wholeRight.data(), static_cast<int>(count), &mask);

    constexpr std::array<int, 4> blocks {1, 31, 97, 512};
    std::size_t at = 0;
    std::size_t block = 0;
    while (at < count) {
        const auto samples = std::min(count - at, static_cast<std::size_t>(blocks[block % blocks.size()]));
        split.process(splitLeft.data() + at, splitRight.data() + at, static_cast<int>(samples), &mask);
        at += samples;
        ++block;
    }

    for (std::size_t i = 0; i < count; ++i) {
        expectNear(splitLeft[i], wholeLeft[i], 0.0f, "image spatializer block-independent left");
        expectNear(splitRight[i], wholeRight[i], 0.0f, "image spatializer block-independent right");
    }
}

void imageSpatializerEndpoints(double sampleRate)
{
    constexpr std::size_t count = 12288;
    constexpr auto latency = holotoner::ImageSpatializer::kFftSize - 1;
    std::array<float, count> inputLeft {};
    std::array<float, count> inputRight {};
    for (std::size_t i = 0; i < count; ++i) {
        inputLeft[i] = static_cast<float>(0.2 * std::sin(static_cast<double>(i) * 0.12));
        inputRight[i] = static_cast<float>(0.15 * std::cos(static_cast<double>(i) * 0.077));
    }

    holotoner::ImageSpatializer::Parameters parameters;
    parameters.wet = 0.0f;
    holotoner::ImageSpatializer dry;
    dry.setParameters(parameters);
    dry.prepare(sampleRate);
    auto dryLeft = inputLeft;
    auto dryRight = inputRight;
    dry.process(dryLeft.data(), dryRight.data(), static_cast<int>(count), nullptr);
    for (std::size_t i = latency; i < count; ++i) {
        expectNear(dryLeft[i], inputLeft[i - latency], 0.0f, "spatializer dry delay left");
        expectNear(dryRight[i], inputRight[i - latency], 0.0f, "spatializer dry delay right");
    }

    parameters.wet = 1.0f;
    parameters.plane.yawDegrees = 90.0f;
    holotoner::ImageSpatializer edge;
    edge.setParameters(parameters);
    edge.prepare(sampleRate);
    auto edgeLeft = inputLeft;
    auto edgeRight = inputRight;
    edge.process(edgeLeft.data(), edgeRight.data(), static_cast<int>(count), nullptr);
    for (std::size_t i = 2 * holotoner::ImageSpatializer::kFftSize; i < count; ++i)
        expectNear(edgeLeft[i], edgeRight[i], 2.0e-5f, "edge-on plane is mono");

    parameters.plane.yawDegrees = 180.0f;
    holotoner::ImageSpatializer back;
    back.setParameters(parameters);
    back.prepare(sampleRate);
    auto backLeft = inputLeft;
    auto backRight = inputRight;
    back.process(backLeft.data(), backRight.data(), static_cast<int>(count), nullptr);
    for (std::size_t i = 2 * holotoner::ImageSpatializer::kFftSize; i < count; ++i) {
        expectNear(backLeft[i], inputRight[i - latency], 2.0e-5f, "back plane swaps left");
        expectNear(backRight[i], inputLeft[i - latency], 2.0e-5f, "back plane swaps right");
    }
}

holotoner::ImageMask phaseFixture(bool mirror)
{
    holotoner::RgbaImage image;
    image.width = image.height = 64;
    image.pixels.resize(64 * 64 * 4);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x) {
            const auto px = mirror ? 63 - x : x;
            const bool on = (px > 10 && px < 22 && y > 12 && y < 47)
                || (px > 20 && px < 48 && y > 40 && y < 48);
            const auto at = static_cast<std::size_t>((y * 64 + x) * 4);
            for (std::size_t c = 0; c < 3; ++c)
                image.pixels[at + c] = on ? 255 : 0;
            image.pixels[at + 3] = 255;
        }
    holotoner::ImageMaskSettings settings;
    settings.channel = holotoner::PixelChannel::Luma;
    settings.binary = true;
    settings.threshold = 0.5f;
    return holotoner::buildImageMask(image, settings);
}

void phaseMapCoordinatesAreActualAudio()
{
    const auto mono = holotoner::stereoToScopePoint(1.0f, 1.0f);
    expectNear(mono.x, 0.0f, 1.0e-6f, "actual mono direction");
    expectNear(mono.y, 1.0f, 1.0e-6f, "actual full-scale radius");
    holotoner::PlaneTransform transform;
    transform.scaleX = transform.scaleY = 2.0f;
    const auto plane = holotoner::projectPlane(transform);
    const auto outer = holotoner::phaseMapPoint(plane, 1.0f, 1.0f, holotoner::PhaseProjection::Polar);
    expectNear(outer.x, 1.0f, 1.0e-6f, "polar can span entire semicircle");
    expectNear(outer.y, 0.0f, 1.0e-6f, "polar full-span edge");
    const auto top = holotoner::phaseMapPoint(plane, 0.0f, 0.5f, holotoner::PhaseProjection::Cartesian);
    expectNear(top.y, 1.0f, 0.0f, "cartesian can cover entire semicircle height");
    const auto a = holotoner::phaseMapPoint(plane, -0.2f, 0.0f, holotoner::PhaseProjection::Cartesian);
    const auto b = holotoner::phaseMapPoint(plane, 0.2f, 0.0f, holotoner::PhaseProjection::Cartesian);
    expectNear(a.y, b.y, 0.0f, "cartesian edges stay straight");
    const auto mask = phaseFixture(false);
    for (const auto point : mask.phasePoints) {
        const auto x = static_cast<int>(std::lround((point.x + 1.0f) * 31.5f));
        const auto y = static_cast<int>(std::lround((1.0f - point.y) * 31.5f));
        expectNear(mask.at(x, y), 1.0f, 0.0f, "binary distribution visits only selected pixels");
    }
}

void phaseImageProcessing(double sampleRate)
{
    auto mask = phaseFixture(false);
    auto other = phaseFixture(true);
    if (!mask.hasPhasePoints || !other.hasPhasePoints) {
        std::cerr << "phase map distribution missing\n";
        ++failures;
    }
    constexpr std::size_t count = 65536;
    std::vector<float> input(count);
    for (std::size_t i = 0; i < count; ++i)
        input[i] = static_cast<float>(0.2 * std::sin(2.0 * 3.141592653589793 * 440.0
                                                   * static_cast<double>(i) / sampleRate));
    auto wholeL = input, wholeR = input, splitL = input, splitR = input;
    auto otherL = input, otherR = input;
    holotoner::ImageSpatializer whole, split, mirrored;
    holotoner::ImageSpatializer::Parameters parameters;
    parameters.imageAmount = 1.0f;
    parameters.plane.scaleX = parameters.plane.scaleY = 0.8f;
    for (auto* engine : {&whole, &split, &mirrored}) {
        engine->setParameters(parameters);
        engine->prepare(sampleRate);
    }
    whole.process(wholeL.data(), wholeR.data(), static_cast<int>(count), &mask);
    mirrored.process(otherL.data(), otherR.data(), static_cast<int>(count), &other);
    constexpr std::array<std::size_t, 3> blocks {1, 31, 512};
    std::size_t at = 0, block = 0;
    while (at < count) {
        const auto length = std::min(count - at, blocks[block++ % blocks.size()]);
        split.process(splitL.data() + at, splitR.data() + at, static_cast<int>(length), &mask);
        at += length;
    }
    double difference = 0.0, stereo = 0.0;
    for (std::size_t i = 0; i < count; ++i) {
        expectNear(splitL[i], wholeL[i], 0.0f, "phase block independence left");
        expectNear(splitR[i], wholeR[i], 0.0f, "phase block independence right");
        if (!std::isfinite(wholeL[i]) || !std::isfinite(wholeR[i])
            || std::abs(wholeL[i]) > 1.0f || std::abs(wholeR[i]) > 1.0f) {
            std::cerr << "phase output not finite/bounded\n";
            ++failures;
            break;
        }
        if (i < 16384)
            continue;
        difference += std::pow(wholeL[i] - otherL[i], 2);
        stereo += std::pow(wholeL[i] - wholeR[i], 2);

    }
    if (difference < 0.01 || stereo < 0.01) {
        std::cerr << "phase image changes did not affect audio/stereo\n";
        ++failures;
    }
    parameters.plane.yawDegrees = 180.0f;
    mirrored.setParameters(parameters);
    mirrored.prepare(sampleRate);
    otherL = otherR = input;
    mirrored.process(otherL.data(), otherR.data(), static_cast<int>(count), &mask);
    for (std::size_t i = count - 512; i < count; ++i) {
        expectNear(otherL[i], wholeR[i], 2.0e-5f, "phase backside swaps left");
        expectNear(otherR[i], wholeL[i], 2.0e-5f, "phase backside swaps right");
    }
    parameters.plane.yawDegrees = 90.0f;
    auto edgeMask = mask;
    whole.setParameters(parameters);
    wholeL = wholeR = input;
    whole.process(wholeL.data(), wholeR.data(), static_cast<int>(count), &edgeMask);
    for (std::size_t i = count - 512; i < count; ++i)
        expectNear(wholeL[i], wholeR[i], 2.0e-5f, "phase edge-on is mono");
    parameters.plane.pitchDegrees = 65;
    parameters.plane.rollDegrees = 37;
    parameters.plane.scaleX = parameters.plane.scaleY = 4;
    whole.setParameters(parameters);
    wholeL = wholeR = input;
    whole.process(wholeL.data(),wholeR.data(),static_cast<int>(count),&edgeMask);
    for (std::size_t i = count-512; i < count; ++i)
        expectNear(wholeL[i],wholeR[i],2.0e-5f,"combined rotations preserve edge-on mono");
    std::fill(wholeL.begin(), wholeL.end(), 0.0f);
    std::fill(wholeR.begin(), wholeR.end(), 0.0f);
    whole.process(wholeL.data(), wholeR.data(), static_cast<int>(count), &edgeMask);
    for (std::size_t i = count - 512; i < count; ++i)
        expectNear(wholeL[i], 0.0f, 1.0e-5f, "phase silence does not self-generate");
}

void imageOffPreservesTone(double sampleRate, double hz, bool stereo = false)
{
    const auto mask = phaseFixture(false);
    constexpr std::size_t count = 65536;
    std::vector<float> left(count), right(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto angle = 2.0 * 3.141592653589793 * hz * static_cast<double>(i) / sampleRate;
        left[i] = static_cast<float>(0.2 * std::sin(angle));
        right[i] = static_cast<float>(0.2 * (stereo ? std::cos(angle) : std::sin(angle)));
    }
    holotoner::ImageSpatializer engine;
    holotoner::ImageSpatializer::Parameters parameters;
    parameters.plane.scaleX = parameters.plane.scaleY = 1.0f;
    parameters.imageAmount = 0.0f; // Image-off reference: original source must remain intact.
    engine.setParameters(parameters);
    engine.prepare(sampleRate);
    engine.process(left.data(), right.data(), static_cast<int>(count), &mask);
    double minimum = 1.0e9, maximum = 0.0, residual = 0.0, total = 0.0;
    for (std::size_t start = 32768; start + 2048 <= count; start += 2048) {
        double power = 0.0;
        for (const auto* channel : {left.data(), right.data()}) {
            double ss = 0.0, cc = 0.0, sc = 0.0, ys = 0.0, yc = 0.0, yy = 0.0;
            for (std::size_t i = start; i < start + 2048; ++i) {
                const auto angle = 2.0 * 3.141592653589793 * hz * static_cast<double>(i) / sampleRate;
                const auto s = std::sin(angle), c = std::cos(angle), y = static_cast<double>(channel[i]);
                ss += s*s; cc += c*c; sc += s*c; ys += y*s; yc += y*c; yy += y*y;
            }
            const auto determinant = ss * cc - sc * sc;
            const auto a = (ys * cc - yc * sc) / determinant;
            const auto b = (yc * ss - ys * sc) / determinant;
            power += a*a + b*b;
            residual += std::max(0.0, yy - a*ys - b*yc);
            total += yy;
        }
        minimum = std::min(minimum, power);
        maximum = std::max(maximum, power);
    }
    std::cout << "Image-off tone " << sampleRate << "/" << hz << ": residual "
              << residual/total << ", power modulation " << (maximum-minimum)/maximum << '\n';
    if (residual / std::max(total, 1.0e-20) > 0.00001
        || (maximum - minimum) / std::max(maximum, 1.0e-20) > 0.005) {
        std::cerr << "phase tone changed/pumped at " << sampleRate << "/" << hz
                  << ": residual " << residual/total << ", modulation "
                  << (maximum-minimum)/maximum << '\n';
        ++failures;
    }
    // Unity stereo power for a centred, full-width phase field.
    if (std::abs(0.5 * (minimum + maximum) / 0.08 - 1.0) > 0.02) {
        std::cerr << "phase tone stereo energy changed at " << hz << '\n';
        ++failures;
    }
    // Fixed map must be linear in the source, not a polarity/envelope generator.
    auto halfL = left, halfR = right;
    for (std::size_t i = 0; i < count; ++i) {
        const auto angle = 2.0 * 3.141592653589793 * hz * static_cast<double>(i) / sampleRate;
        halfL[i] = static_cast<float>(0.1 * std::sin(angle));
        halfR[i] = static_cast<float>(0.1 * (stereo ? std::cos(angle) : std::sin(angle)));
    }
    engine.prepare(sampleRate);
    engine.process(halfL.data(), halfR.data(), static_cast<int>(count), &mask);
    for (std::size_t i = 32768; i < count; ++i) {
        expectNear(halfL[i], 0.5f * left[i], 1.0e-6f, "phase source linearity left");
        expectNear(halfR[i], 0.5f * right[i], 1.0e-6f, "phase source linearity right");
    }
}

void imageFieldMeasuredSupport(double sampleRate)
{
    auto mask = phaseFixture(false);
    holotoner::ImageSpatializer::Parameters parameters;
    parameters.plane.scaleX = parameters.plane.scaleY = 0.8f;
    auto referenceMask = mask;
    referenceMask.hasNearestSupport = false;
    constexpr std::size_t count = 131072;
    std::vector<float> left(count), right(count);
    for (std::size_t i = 0; i < count; ++i) {
        double value = 0.0;
        for (int partial = 1; partial <= 16; ++partial)
            value += 0.009*std::sin(2.0*3.141592653589793*(97.0*partial+7.0*partial*partial)
                *static_cast<double>(i)/sampleRate + partial*2.399963229728653);
        left[i] = right[i] = static_cast<float>(value);
    }
    auto refL = left, refR = right;
    holotoner::ImageSpatializer engine, reference;
    engine.setParameters(parameters); reference.setParameters(parameters);
    engine.prepare(sampleRate); reference.prepare(sampleRate);
    engine.process(left.data(), right.data(), static_cast<int>(count), &mask);
    reference.process(refL.data(), refR.data(), static_cast<int>(count), &referenceMask);
    int hits = 0, referenceHits = 0, measured = 0, stem = 0, arm = 0;
    double maximumPowerError = 0.0;
    const auto supported = [&mask](holotoner::ScopePoint point) {
        const auto x = static_cast<int>(std::lround((point.x/0.4f+1.0f)*31.5f));
        const auto y = static_cast<int>(std::lround((1.0f-(point.y-0.5f)/0.4f)*31.5f));
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                if (mask.at(x+dx,y+dy)>0.5f) return true;
        return false;
    };
    for (std::size_t i = 32768; i < count; ++i) {
        const auto power = static_cast<double>(left[i])*left[i]+static_cast<double>(right[i])*right[i];
        const auto refPower = static_cast<double>(refL[i])*refL[i]+static_cast<double>(refR[i])*refR[i];
        maximumPowerError = std::max(maximumPowerError, std::abs(power-refPower));
        const auto point = holotoner::stereoToScopePoint(left[i]*5.0f,right[i]*5.0f);
        const auto refPoint = holotoner::stereoToScopePoint(refL[i]*5.0f,refR[i]*5.0f);
        if (std::hypot(point.x,point.y)<0.25f || std::hypot(point.x,point.y)>0.75f) continue;
        ++measured;
        hits += supported(point) ? 1 : 0;
        stem += supported(point) && point.x < -0.1f && point.y > 0.45f ? 1 : 0;
        arm += supported(point) && point.x > 0.0f && point.y < 0.4f ? 1 : 0;
        referenceHits += supported(refPoint) ? 1 : 0;
    }
    std::cout << "Measured image support " << sampleRate << ": " << hits << '/' << measured
              << ", unconstrained " << referenceHits << ", stem/arm " << stem << '/' << arm
              << ", stereo power error " << maximumPowerError << '\n';
    if (measured < 1000 || hits < measured*9/10 || hits < referenceHits + measured/4
        || stem < 2000 || arm < 300
        || maximumPowerError > 1.0e-7) {
        std::cerr << "image constraint did not shape actual audio while retaining stereo energy\n";
        ++failures;
    }
}

} // namespace

holotoner::ImageMask whiteFixture()
{
    holotoner::RgbaImage image;
    image.width = image.height = 64;
    image.pixels.assign(64 * 64 * 4, 255);
    holotoner::ImageMaskSettings settings;
    settings.channel = holotoner::PixelChannel::Luma;
    return holotoner::buildImageMask(image, settings);
}

struct SpaceRun { double left = 0, right = 0, mono = 0; bool bounded = true; };

// Space alone (Image Amount 0): measured after the 2048-sample latency has settled.
SpaceRun runSpace(double sampleRate, holotoner::ImageSpatializer::Parameters parameters,
                  const holotoner::ImageMask* mask, double hz, int blockSize = 512)
{
    constexpr std::size_t count = 32768;
    std::vector<float> left(count), right(count);
    std::uint32_t seed = 12345;
    for (std::size_t i = 0; i < count; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const auto noise = static_cast<float>(seed >> 8) / 16777216.0f - 0.5f;
        left[i] = right[i] = hz > 0 ? static_cast<float>(0.2 * std::sin(2.0 * 3.141592653589793 * hz
                                                         * static_cast<double>(i) / sampleRate))
                                    : 0.4f * noise;
    }
    holotoner::ImageSpatializer engine;
    parameters.imageAmount = 0.0f;
    engine.setParameters(parameters);
    engine.prepare(sampleRate);
    for (std::size_t at = 0; at < count; at += static_cast<std::size_t>(blockSize)) {
        const auto length = std::min(count - at, static_cast<std::size_t>(blockSize));
        engine.process(left.data() + at, right.data() + at, static_cast<int>(length), mask);
    }
    SpaceRun run;
    for (std::size_t i = 8192; i < count; ++i) {
        run.bounded = run.bounded && std::isfinite(left[i]) && std::isfinite(right[i])
            && std::abs(left[i]) < 2.0f && std::abs(right[i]) < 2.0f;
        run.left += static_cast<double>(left[i]) * left[i];
        run.right += static_cast<double>(right[i]) * right[i];
        run.mono += std::pow(0.5 * (static_cast<double>(left[i]) + right[i]), 2.0);
    }
    return run;
}

// Processes a mono input through Space alone (Image Amount 0); output is latency-compensated.
std::pair<std::vector<float>, std::vector<float>> runEffects(double sampleRate, holotoner::ImageSpatializer::Parameters parameters,
                                                             const holotoner::ImageMask* mask, const std::vector<float>& input)
{
    holotoner::ImageSpatializer engine;
    parameters.imageAmount = 0.0f;
    engine.setParameters(parameters);
    engine.prepare(sampleRate);
    const auto latency = static_cast<std::size_t>(engine.latencySamples());
    std::vector<float> left(input), right(input);
    left.resize(input.size() + latency, 0.0f);
    right.resize(input.size() + latency, 0.0f);
    for (std::size_t at = 0; at < left.size(); at += 512) {
        const auto length = std::min<std::size_t>(512, left.size() - at);
        engine.process(left.data() + at, right.data() + at, static_cast<int>(length), mask);
    }
    left.erase(left.begin(), left.begin() + static_cast<std::ptrdiff_t>(latency));
    right.erase(right.begin(), right.begin() + static_cast<std::ptrdiff_t>(latency));
    return {left, right};
}

std::vector<float> toneBurst(double sampleRate, double hz, double seconds, double total)
{
    std::vector<float> tone(static_cast<std::size_t>(total * sampleRate));
    for (std::size_t i = 0; i < static_cast<std::size_t>(seconds * sampleRate); ++i) {
        const auto t = static_cast<double>(i) / sampleRate;
        const auto fade = std::min({1.0, t / 0.005, (seconds - t) / 0.005});
        tone[i] = static_cast<float>(0.3 * fade * std::sin(2.0 * 3.141592653589793 * hz * t));
    }
    return tone;
}

double energyBetween(const std::vector<float>& signal, double sampleRate, double from, double to)
{
    double energy = 0.0;
    const auto end = std::min(signal.size(), static_cast<std::size_t>(to * sampleRate));
    for (auto i = static_cast<std::size_t>(from * sampleRate); i < end; ++i) energy += static_cast<double>(signal[i]) * signal[i];
    return energy;
}

// Time of the loudest 10 ms after `after` seconds.
double loudestAfter(const std::vector<float>& signal, double sampleRate, double after)
{
    double best = 0.0, at = 0.0;
    const auto step = 0.01;
    for (double t = after; t + step < static_cast<double>(signal.size()) / sampleRate; t += step) {
        const auto energy = energyBetween(signal, sampleRate, t, t + step);
        if (energy > best) { best = energy; at = t; }
    }
    return at;
}

void spaceEffects(double sampleRate)
{
    holotoner::ImageSpatializer::Parameters base;
    // Echo: a 60 ms burst returns after its row's distance time (about 250 ms at rest).
    auto echo = base;
    echo.echo = 1.0f;
    const auto burst = toneBurst(sampleRate, 1000.0, 0.06, 1.2);
    const auto dry = runEffects(sampleRate, base, nullptr, burst).first;
    const auto wet = runEffects(sampleRate, echo, nullptr, burst).first;
    const auto arrival = loudestAfter(wet, sampleRate, 0.12);
    if (!(energyBetween(wet, sampleRate, 0.15, 0.6) > 0.05 * energyBetween(dry, sampleRate, 0.0, 0.1)
          && energyBetween(dry, sampleRate, 0.15, 0.6) < 1.0e-6 && arrival > 0.2 && arrival < 0.35)) {
        std::cerr << "echo did not return after the plane's distance time (" << arrival << " s)\n";
        ++failures;
    }
    // Time sets the echo at the plane centre.
    auto later = echo;
    later.echoTime = 0.5f;
    const auto laterArrival = loudestAfter(runEffects(sampleRate, later, nullptr, burst).first, sampleRate, 0.12);
    if (!(laterArrival > 0.45 && laterArrival < 0.6)) {
        std::cerr << "echo Time did not move the echo (" << laterArrival << " s)\n";
        ++failures;
    }
    // Tilt: the top (highs) comes near and echoes sooner than the bottom (lows).
    auto tilted = echo;
    tilted.plane.pitchDegrees = 45.0f;
    const auto high = loudestAfter(runEffects(sampleRate, tilted, nullptr, toneBurst(sampleRate, 7040.0, 0.06, 1.2)).first, sampleRate, 0.1);
    const auto low = loudestAfter(runEffects(sampleRate, tilted, nullptr, toneBurst(sampleRate, 150.0, 0.06, 1.2)).first, sampleRate, 0.1);
    if (!(high + 0.1 < low)) {
        std::cerr << "echo time did not follow plane depth: high " << high << " s, low " << low << " s\n";
        ++failures;
    }
    // Feedback repeats the echo and stays bounded.
    auto repeating = echo;
    repeating.feedback = 1.0f;
    const auto repeats = runEffects(sampleRate, repeating, nullptr, burst).first;
    float peak = 0.0f;
    for (const auto value : repeats) peak = std::max(peak, std::abs(value));
    if (!(energyBetween(repeats, sampleRate, 0.4, 1.2) > 2.0 * energyBetween(wet, sampleRate, 0.4, 1.2) && peak < 1.0f)) {
        std::cerr << "echo feedback did not repeat, or was unbounded (peak " << peak << ")\n";
        ++failures;
    }
    // Resonate: a tone on a bright row rings on after it stops; without Resonate it does not.
    const auto white = whiteFixture();
    auto ringing = base;
    ringing.resonate = 1.0f;
    const auto rings = holotoner::resonators(holotoner::projectPlane(ringing.plane), holotoner::spaceRowWeights(white));
    const auto on = rings.hz[static_cast<std::size_t>(rings.count / 2)];
    const auto tone = toneBurst(sampleRate, on, 0.4, 1.2);
    const auto plainTone = runEffects(sampleRate, base, &white, tone).first;
    const auto rung = runEffects(sampleRate, ringing, &white, tone).first;
    float ringPeak = 0.0f;
    for (const auto value : rung) ringPeak = std::max(ringPeak, std::abs(value));
    const auto tail = energyBetween(rung, sampleRate, 0.5, 0.9), plainTail = energyBetween(plainTone, sampleRate, 0.5, 0.9);
    // Bounded by construction: a 0.3 tone may grow at most 1 + kResonanceBoost times.
    if (!(tail > 100.0 * plainTail + 1.0e-6 && ringPeak < 0.3f * (1.0f + static_cast<float>(holotoner::kResonanceBoost)) + 0.05f)) {
        std::cerr << "resonate did not ring on a bright row, or was unbounded (tail " << tail << ", peak " << ringPeak << ")\n";
        ++failures;
    }
    // Interfere with Feedback: facing is still flat; turned stays bounded.
    auto comb = base;
    comb.interfere = comb.feedback = 1.0f;
    const auto noise = runSpace(sampleRate, base, nullptr, 0.0);
    const auto facing = runSpace(sampleRate, comb, nullptr, 0.0);
    comb.plane.yawDegrees = 50.0f;
    const auto turned = runSpace(sampleRate, comb, nullptr, 0.0);
    if (std::abs(facing.left - noise.left) > 1.0e-6 * noise.left || !turned.bounded) {
        std::cerr << "interfere feedback not flat when facing, or unbounded\n";
        ++failures;
    }
}

void shapesAreDeterministic()
{
    using holotoner::ShapeKind;
    for (int kind = 0; kind < static_cast<int>(holotoner::kShapeNames.size()); ++kind) {
        const auto shape = static_cast<ShapeKind>(kind);
        const auto a = holotoner::generateShape(shape, 3), b = holotoner::generateShape(shape, 3);
        const auto c = holotoner::generateShape(shape, 6);
        std::size_t lit = 0;
        for (std::size_t i = 3; i < a.pixels.size(); i += 4) lit += a.pixels[i] > 0 ? 1 : 0;
        const auto total = a.pixels.size() / 4;
        if (a.pixels != b.pixels || a.pixels == c.pixels || lit < total / 100 || lit > total * 3 / 4) {
            std::cerr << "shape " << holotoner::kShapeNames[static_cast<std::size_t>(kind)]
                      << " not deterministic, order-sensitive or reasonably filled (" << lit << "/" << total << ")\n";
            ++failures;
        }
    }
    const auto smiley = holotoner::generateShape(ShapeKind::Smiley, 3);
    const auto smileyInk = [&](double x, double y) {
        const auto px = static_cast<int>((x + 1.0) * 0.5 * smiley.width);
        const auto py = static_cast<int>((1.0 - y) * 0.5 * smiley.height);
        return smiley.pixels[static_cast<std::size_t>((py * smiley.width + px) * 4 + 3)] > 0;
    };
    if (!smileyInk(-0.28, 0.28) || !smileyInk(0.28, 0.28) || !smileyInk(0, -0.58)
        || !smileyInk(0, 0.84) || smileyInk(0, 0) || smileyInk(0, 0.58)) {
        std::cerr << "smiley must have two eyes, a lower smile and an outline with an empty centre\n";
        ++failures;
    }
    // Clamped orders.
    if (holotoner::generateShape(ShapeKind::Grid, 0).pixels != holotoner::generateShape(ShapeKind::Grid, 1).pixels) {
        std::cerr << "shape order not clamped\n";
        ++failures;
    }
}

void deltaHearsOnlyTheChange(double sampleRate)
{
    // Delta outputs processed minus dry: nothing when nothing is changed, only the echo with Echo.
    holotoner::ImageSpatializer::Parameters parameters;
    parameters.delta = true;
    // The burst starts at 0.2 s, after the analysis window has filled (before that the
    // processed path is still silent, so the difference is the dry sound itself).
    auto burst = toneBurst(sampleRate, 1000.0, 0.06, 1.0);
    burst.insert(burst.begin(), static_cast<std::size_t>(0.2 * sampleRate), 0.0f);
    const auto silent = runEffects(sampleRate, parameters, nullptr, burst).first;
    parameters.echo = 1.0f;
    const auto echoOnly = runEffects(sampleRate, parameters, nullptr, burst).first;
    const auto input = energyBetween(burst, sampleRate, 0.2, 0.3);
    if (!(energyBetween(silent, sampleRate, 0.1, 1.2) < 1.0e-9 * input
          && energyBetween(echoOnly, sampleRate, 0.1, 0.3) < 1.0e-6 * input
          && energyBetween(echoOnly, sampleRate, 0.35, 0.8) > 0.1 * input)) {
        std::cerr << "delta did not isolate what Holotoner changes\n";
        ++failures;
    }
}

void interferenceFollowsGeometry(double sampleRate)
{
    holotoner::ImageSpatializer::Parameters facing;
    auto interfering = facing;
    interfering.interfere = 1.0f;
    // Facing: both halves are equally far, so the extra path sums flat.
    const auto plain = runSpace(sampleRate, facing, nullptr, 0.0);
    const auto flat = runSpace(sampleRate, interfering, nullptr, 0.0);
    // Turned: the halves' path difference makes a comb that removes energy.
    auto turned = interfering;
    turned.plane.yawDegrees = 50.0f;
    const auto comb = runSpace(sampleRate, turned, nullptr, 0.0);
    if (std::abs(flat.left - plain.left) > 1.0e-6 * plain.left || !(comb.left < 0.8 * plain.left) || !comb.bounded) {
        std::cerr << "interference did not follow geometry: flat " << flat.left / plain.left
                  << ", turned " << comb.left / plain.left << '\n';
        ++failures;
    }
}

void edgeTradesGritForImage(double sampleRate)
{
    // Two tones, nothing above 5 kHz: energy there is the image field's distortion.
    const auto mask = phaseFixture(false);
    const auto grit = [&](float edge, float smooth = 0.0f) {
        constexpr std::size_t count = 65536;
        std::vector<float> left(count), right(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto t = static_cast<double>(i) / sampleRate;
            left[i] = static_cast<float>(0.25 * std::sin(2.0 * 3.141592653589793 * 220.0 * t)
                                         + 0.1 * std::sin(2.0 * 3.141592653589793 * 330.0 * t));
            right[i] = static_cast<float>(0.2 * std::sin(2.0 * 3.141592653589793 * 220.0 * t + 0.4)
                                          + 0.12 * std::sin(2.0 * 3.141592653589793 * 330.0 * t));
        }
        holotoner::ImageSpatializer engine;
        holotoner::ImageSpatializer::Parameters parameters;
        parameters.plane.scaleX = parameters.plane.scaleY = 0.8f;
        parameters.plane.pitchDegrees = 50.0f;
        parameters.edge = edge;
        parameters.smooth = smooth;
        engine.setParameters(parameters);
        engine.prepare(sampleRate);
        engine.process(left.data(), right.data(), static_cast<int>(count), &mask);
        constexpr std::size_t size = 16384;
        holotoner::Fft fft;
        fft.prepare(size);
        std::vector<std::complex<double>> data(size);
        for (std::size_t i = 0; i < size; ++i) {
            const auto window = 0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * static_cast<double>(i) / size);
            data[i] = {left[count - size + i] * window, right[count - size + i] * window};
        }
        fft.transform(data, false);
        double high = 0.0, all = 0.0;
        for (std::size_t k = 1; k < size / 2; ++k) {
            const auto power = std::norm(data[k]) + std::norm(data[size - k]);
            all += power;
            if (static_cast<double>(k) * sampleRate / size > 5000.0) high += power;
        }
        return 10.0 * std::log10(high / all + 1.0e-30);
    };
    const auto hard = grit(1.0f), soft = grit(0.0f);
    const auto smoothed = grit(1.0f, 0.001f);
    std::cout << "Smooth 1 ms at " << sampleRate << " Hz: >5 kHz distortion "
              << hard << " dB -> " << smoothed << " dB\n";
    if (!(smoothed < hard - 10.0)) {
        std::cerr << "Smooth did not reduce high-frequency image-field distortion\n";
        ++failures;
    }

    if (!(soft < hard - 15.0)) {
        std::cerr << "Edge 0 did not reduce image-field grit: " << hard << " dB -> " << soft << " dB\n";
        ++failures;
    }
}

void restSteersTheBackground(double sampleRate)
{
    // Rest turns samples the image cannot hold toward mono (-1) or the sides (+1); 0 is unchanged.
    const auto mask = phaseFixture(false);
    const auto correlation = [&](float rest) {
        constexpr std::size_t count = 65536;
        std::vector<float> left(count), right(count);
        std::uint32_t seed = 7;
        double low = 0.0;
        for (std::size_t i = 0; i < count; ++i) {
            seed = seed * 1664525u + 1013904223u;
            low += 0.05 * (static_cast<double>(seed >> 8) / 16777216.0 - 0.5 - low);
            const auto level = 0.5 + 0.5 * std::sin(2.0 * 3.141592653589793 * 1.3 * static_cast<double>(i) / sampleRate);
            left[i] = right[i] = static_cast<float>(4.0 * level * low);
        }
        holotoner::ImageSpatializer engine;
        holotoner::ImageSpatializer::Parameters parameters;
        parameters.plane.scaleX = parameters.plane.scaleY = 0.8f;
        parameters.rest = rest;
        engine.setParameters(parameters);
        engine.prepare(sampleRate);
        engine.process(left.data(), right.data(), static_cast<int>(count), &mask);
        double ll = 0.0, rr = 0.0, lr = 0.0, energy = 0.0;
        for (std::size_t i = 16384; i < count; ++i) {
            ll += static_cast<double>(left[i]) * left[i];
            rr += static_cast<double>(right[i]) * right[i];
            lr += static_cast<double>(left[i]) * right[i];
        }
        energy = ll + rr;
        return std::pair {lr / std::sqrt(ll * rr + 1.0e-30), energy};
    };
    const auto [narrow, narrowEnergy] = correlation(-1.0f);
    const auto [plain, plainEnergy] = correlation(0.0f);
    const auto [wide, wideEnergy] = correlation(1.0f);
    if (!(narrow > plain + 0.01 && wide < plain - 0.1)
        || std::abs(narrowEnergy - plainEnergy) > 1.0e-6 * plainEnergy || std::abs(wideEnergy - plainEnergy) > 1.0e-6 * plainEnergy) {
        std::cerr << "rest did not steer the background by rotation: " << narrow << " / " << plain << " / " << wide << '\n';
        ++failures;
    }
}

void smoothRetainsEnergyAndPartition(double sampleRate)
{
    const auto mask = phaseFixture(false);
    constexpr std::size_t count = 32768;
    const auto render = [&](float smooth, int block, bool image = true) {
        std::pair<std::vector<float>, std::vector<float>> output {std::vector<float>(count), std::vector<float>(count)};
        for (std::size_t i = 0; i < count; ++i) {
            output.first[i] = 0.2f * std::sin(static_cast<float>(i) * 0.13f);
            output.second[i] = 0.17f * std::cos(static_cast<float>(i) * 0.077f);
        }
        auto engine = std::make_unique<holotoner::ImageSpatializer>();
        holotoner::ImageSpatializer::Parameters parameters;
        parameters.smooth = smooth;
        parameters.plane.scaleX = parameters.plane.scaleY = 0.8f;
        parameters.imageAmount = image ? 1.0f : 0.0f;
        engine->setParameters(parameters);
        engine->prepare(sampleRate);
        for (std::size_t at = 0; at < count; at += static_cast<std::size_t>(block))
            engine->process(output.first.data()+at, output.second.data()+at, static_cast<int>(std::min(static_cast<std::size_t>(block),count-at)), &mask);
        return output;
    };
    const auto hard = render(0,512), smooth = render(0.001f,512);
    for (std::size_t i = 0; i < count; ++i) {
        const auto energy = [](float l,float r) { return static_cast<double>(l)*l+static_cast<double>(r)*r; };
        if (!std::isfinite(smooth.first[i]) || !std::isfinite(smooth.second[i])
            || std::abs(energy(hard.first[i],hard.second[i])-energy(smooth.first[i],smooth.second[i])) > 3.0e-8) {
            std::cerr << "Smooth changed sample energy or produced invalid output\n"; ++failures; break;
        }
    }
    for (int block : {1,31}) {
        const auto split = render(0.001f,block);
        if (split != smooth) { std::cerr << "Smooth depends on block partition\n"; ++failures; }
    }
    if (render(0,31,false) != render(0.005f,31,false)) {
        std::cerr << "Smooth changed image-off output\n"; ++failures;
    }
}

int main()
{
    {
        using namespace holotoner;
        expectNear(static_cast<float>(kSyncBeats[15]), 1.0f, 0.0f, "quarter note is one PPQ beat");
        expectNear(static_cast<float>(kSyncBeats[20]), 4.0f, 0.0f, "whole note is four PPQ beats");
        CycleClock whole, split;
        whole.advance(48000,48000,0.25,120,true);
        for (int i = 0; i < 48000; ++i) split.advance(1,48000,0.25,120,true);
        expectNear(static_cast<float>(whole.freeCycles),static_cast<float>(split.freeCycles),1.0e-6f,"free clock partition");
        expectNear(static_cast<float>(whole.beats),static_cast<float>(split.beats),1.0e-6f,"beat clock partition");
        whole.advance(48000,48000,8,200,false);
        expectNear(static_cast<float>(whole.freeCycles),0.25f,0.0f,"pause holds free phase");
        expectNear(static_cast<float>(whole.cycles(15,-0.25)), -0.25f,0.0f,"negative PPQ preserved");
        auto base = PlaneTransform {};
        base.rollDegrees = 170;
        const auto spun = motionPlane(base,{3,true,0.5f},0.1);
        expectNear(spun.rollDegrees,-154,1.0e-5f,"spin wraps instead of reversing at seam");
        expectNear(base.rollDegrees,170,0,"motion never overwrites base geometry");
        expectNear(motionPlane(base,{4,false,0.4f},0.25).positionX,0.4f,1.0e-6f,"position swing");
    }
    projectionEndpoints();
    stereoEndpoints(44100.0);
    stereoEndpoints(48000.0);
    stereoEndpoints(96000.0);
    blockPartitionIsIndependent();
    dryIsExact();
    invalidValuesAreContained();
    sourcePlayerIsDeterministic();
    sharedExchangeRetiresOffAudioPath();
    imageMaskIsDeterministic();
    imageSpatializerIsBlockIndependent();
    imageSpatializerEndpoints(44100.0);
    imageSpatializerEndpoints(48000.0);
    imageSpatializerEndpoints(96000.0);
    phaseMapCoordinatesAreActualAudio();
    phaseImageProcessing(44100.0);
    phaseImageProcessing(48000.0);
    phaseImageProcessing(96000.0);
    for (const auto rate : {44100.0, 48000.0, 96000.0})
        for (const auto hz : {110.0, 440.0, 1760.0, 7040.0})
            imageOffPreservesTone(rate, hz);
    imageOffPreservesTone(48000.0, 440.0, true);
    imageFieldMeasuredSupport(44100.0);
    imageFieldMeasuredSupport(48000.0);
    imageFieldMeasuredSupport(96000.0);
    for (const auto rate : {44100.0, 48000.0, 96000.0})
        spaceEffects(rate);
    shapesAreDeterministic();
    for (const auto rate : {44100.0, 48000.0, 96000.0})
        deltaHearsOnlyTheChange(rate);
    for (const auto rate : {44100.0, 48000.0, 96000.0})
        restSteersTheBackground(rate);
    for (const auto rate : {44100.0, 48000.0, 96000.0})
        edgeTradesGritForImage(rate);
    for (const auto rate : {44100.0, 48000.0, 96000.0})
        smoothRetainsEnergyAndPartition(rate);
    for (const auto rate : {44100.0, 48000.0, 96000.0})
        interferenceFollowsGeometry(rate);
    if (failures != 0)
        std::cerr << failures << " test assertion(s) failed\n";
    return failures == 0 ? 0 : 1;
}
