#include "core/audio/SharedExchange.h"
#include "core/audio/SourcePlayer.h"
#include "core/dsp/ImageSpatializer.h"
#include "core/dsp/StereoWidth.h"
#include "core/geometry/PlaneProjection.h"
#include "core/image/ImageMask.h"

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

void imageSpatializerMakesMonoImageStereo()
{
    constexpr std::size_t count = 12288;
    std::array<float, count> left {};
    std::array<float, count> right {};
    for (std::size_t i = 0; i < count; ++i)
        left[i] = right[i] = static_cast<float>(0.2 * std::sin(static_cast<double>(i) * 0.21));

    holotoner::ImageMask mask;
    mask.hasSource = true;
    for (int y = 0; y < holotoner::kImageMaskHeight; ++y)
        for (int x = 0; x < holotoner::kImageMaskWidth; ++x)
            mask.values[static_cast<std::size_t>(y * holotoner::kImageMaskWidth + x)]
                = x >= 40 ? 1.0f : 0.0f;

    holotoner::ImageSpatializer::Parameters parameters;
    parameters.target = holotoner::ImageTarget::Place;
    parameters.imageAmount = 1.0f;
    parameters.wet = 1.0f;
    holotoner::ImageSpatializer engine;
    engine.setParameters(parameters);
    engine.prepare(48000.0);
    engine.process(left.data(), right.data(), static_cast<int>(count), &mask);
    double differenceEnergy = 0.0;
    for (std::size_t i = 2 * holotoner::ImageSpatializer::kFftSize; i < count; ++i) {
        const auto difference = static_cast<double>(left[i] - right[i]);
        differenceEnergy += difference * difference;
    }
    if (differenceEnergy < 1.0e-4) {
        std::cerr << "image placement did not make mono input stereo\n";
        ++failures;
    }
    const auto& analysis = engine.analysis();
    float analysisPeak = 0.0f;
    bool showsStereoField = false;
    for (std::size_t i = 0; i < holotoner::ImageSpatializer::kAnalysisBins; ++i) {
        analysisPeak = std::max(analysisPeak, analysis.level[i]);
        showsStereoField = showsStereoField || (analysis.level[i] > 1.0e-4f
            && (std::abs(analysis.balance[i]) > 0.01f || analysis.correlation[i] < 0.98f));
        if (!std::isfinite(analysis.level[i]) || !std::isfinite(analysis.balance[i])
            || !std::isfinite(analysis.correlation[i]) || analysis.level[i] < 0.0f
            || std::abs(analysis.balance[i]) > 1.0f || std::abs(analysis.correlation[i]) > 1.0f) {
            std::cerr << "polar analysis escaped finite bounds\n";
            ++failures;
            break;
        }
    }
    if (analysis.revision == 0 || analysisPeak < 1.0e-4f) {
        std::cerr << "polar analysis did not publish output energy\n";
        ++failures;
    }
    if (!showsStereoField) {
        std::cerr << "polar analysis did not expose the image stereo field\n";
        ++failures;
    }
}

void blackImageCanSilenceLevelTarget()
{
    constexpr std::size_t count = 12288;
    std::array<float, count> left {};
    std::array<float, count> right {};
    for (std::size_t i = 0; i < count; ++i)
        left[i] = right[i] = static_cast<float>(0.2 * std::sin(static_cast<double>(i) * 0.11));
    holotoner::ImageMask black;
    black.hasSource = true;
    holotoner::ImageSpatializer::Parameters parameters;
    parameters.target = holotoner::ImageTarget::Level;
    parameters.imageAmount = 1.0f;
    parameters.wet = 1.0f;
    holotoner::ImageSpatializer engine;
    engine.setParameters(parameters);
    engine.prepare(48000.0);
    engine.process(left.data(), right.data(), static_cast<int>(count), &black);
    float endingPeak = 0.0f;
    for (std::size_t i = count - holotoner::ImageSpatializer::kHopSize; i < count; ++i)
        endingPeak = std::max({endingPeak, std::abs(left[i]), std::abs(right[i])});
    if (endingPeak > 1.0e-4f) {
        std::cerr << "black image level fade did not converge, peak " << endingPeak << '\n';
        ++failures;
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
    parameters.target = holotoner::ImageTarget::Phase;
    parameters.imageAmount = 1.0f;
    parameters.plane.scaleX = parameters.plane.scaleY = 0.8f;
    holotoner::buildImageField(mask, parameters.plane, parameters.phaseProjection);
    holotoner::buildImageField(other, parameters.plane, parameters.phaseProjection);
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
    holotoner::buildImageField(mask, parameters.plane, parameters.phaseProjection);
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
    holotoner::buildImageField(edgeMask, parameters.plane, parameters.phaseProjection);
    whole.setParameters(parameters);
    wholeL = wholeR = input;
    whole.process(wholeL.data(), wholeR.data(), static_cast<int>(count), &edgeMask);
    for (std::size_t i = count - 512; i < count; ++i)
        expectNear(wholeL[i], wholeR[i], 2.0e-5f, "phase edge-on is mono");
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
    holotoner::buildImageField(mask, parameters.plane, parameters.phaseProjection);
    auto referenceMask = mask;
    for (int row = 0; row < holotoner::ImageMask::kFieldRows; ++row)
        for (int column = 0; column < holotoner::ImageMask::kFieldAngles; ++column)
            referenceMask.fieldAngles[static_cast<std::size_t>(row*holotoner::ImageMask::kFieldAngles+column)]
                = static_cast<float>(-0.5*3.141592653589793 + 3.141592653589793
                    *column/(holotoner::ImageMask::kFieldAngles-1));
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
    imageMaskIsDeterministic();
    imageSpatializerIsBlockIndependent();
    imageSpatializerEndpoints(44100.0);
    imageSpatializerEndpoints(48000.0);
    imageSpatializerEndpoints(96000.0);
    imageSpatializerMakesMonoImageStereo();
    blackImageCanSilenceLevelTarget();
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
    if (failures != 0)
        std::cerr << failures << " test assertion(s) failed\n";
    return failures == 0 ? 0 : 1;
}
