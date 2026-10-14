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
// Edge 0 pulls within this width (radians); each Edge step widens it geometrically to
// kAttraction * kAttractionSpan at 99%, and Edge 100% is the exact hard snap.
constexpr double kAttraction = 0.2;
constexpr double kAttractionSpan = 30.0;
constexpr double kRestSide = 1.0;
constexpr double kEchoLevel = 2.0; // Echo at 100%: delayed level relative to the direct sound // radians from mono where Rest +100% sends unheld samples

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
    // Echo history: the longest echo plus interpolation, in hops at this rate. Allocated only here.
    echoFrames_ = static_cast<std::size_t>(std::ceil(kEchoMaxSeconds * sampleRate_ / static_cast<double>(kHopSize))) + 3;
    echoHistory_.assign(echoFrames_ * (kFftSize / 2) * 2, {});
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
    deltaCurrent_ = parameters_.delta ? 1.0f : 0.0f;
    safetyResets_ = 0;
    phaseControls_.fill({});
    phaseInitialized_ = false;
    fieldTime_ = 0.0;
    geometryBlend_ = 1.0f;
    fieldRotationCurrent_ = 0.0;
    fieldFollow_ = previousFieldFollow_ = parameters_.smooth > 0.0f
        ? -std::expm1(-1.0 / (sampleRate_ * parameters_.smooth)) : 1.0;
    fieldHasSupport_ = false;
    supportValues_.fill(0);
    softSupport_.fill(0);
    fieldAspectX_ = fieldAspectY_ = 1;
    rowWeight_.fill(0); rowWeightTarget_.fill(0);
    std::fill(echoHistory_.begin(), echoHistory_.end(), std::complex<float> {});
    echoAt_ = 0; echoLive_ = false;
    ringLeft_.fill({}); ringRight_.fill({}); ringLive_ = false;
    imagePlane_ = previousImagePlane_ = projectPlane(parameters_.plane);
    for (int y = 0; y < kImageMaskHeight; ++y)
        for (int x = 0; x < kImageMaskWidth; ++x)
            nearest_[static_cast<std::size_t>(y*kImageMaskWidth+x)] = {static_cast<float>(x),static_cast<float>(y)};
    previousNearest_ = nearest_;

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
    parameters.edge = std::clamp(finiteOr(parameters.edge, 1.0f), 0.0f, 1.0f);
    parameters.smooth = std::clamp(finiteOr(parameters.smooth, 0.0f), 0.0f, 0.005f);
    parameters.rest = std::clamp(finiteOr(parameters.rest, 0.0f), -1.0f, 1.0f);
    parameters.wet = std::clamp(finiteOr(parameters.wet, 1.0f), 0.0f, 1.0f);
    for (auto* amount : {&parameters.interfere, &parameters.feedback, &parameters.echo, &parameters.resonate})
        *amount = std::clamp(finiteOr(*amount, 0.0f), 0.0f, 1.0f);
    parameters.echoTime = std::clamp(finiteOr(parameters.echoTime, 0.25f), static_cast<float>(kEchoMinSeconds),
                                     static_cast<float>(kEchoMaxSeconds));
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
        deltaCurrent_ += ((parameters_.delta ? 1.0f : 0.0f) - deltaCurrent_) * wetRate_;
        const auto dryGain = 1.0 - static_cast<double>(deltaCurrent_);
        const auto outLeft = dryGain * static_cast<double>(delayedLeft) + static_cast<double>(wetCurrent_)
            * (wetLeft - static_cast<double>(delayedLeft));
        const auto outRight = dryGain * static_cast<double>(delayedRight) + static_cast<double>(wetCurrent_)
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
    const SpaceAmounts space {smoothedParameters_.interfere, smoothedParameters_.feedback,
                              smoothedParameters_.echo, smoothedParameters_.resonate, smoothedParameters_.echoTime};
    const bool spaced = spaceActive(space);
    // Effects switched off drop their memory, so switching on never releases stale sound.
    if (space.echo <= 1.0e-6 && echoLive_) {
        std::fill(echoHistory_.begin(), echoHistory_.end(), std::complex<float> {});
        echoLive_ = false;
    }
    if (space.resonate <= 1.0e-6 && ringLive_) {
        ringLeft_.fill({}); ringRight_.fill({});
        ringLive_ = false;
    }
    const auto rings = space.resonate > 1.0e-6 ? resonators(imagePlane_, rowWeight_) : Resonators {};
    if (space.echo > 1.0e-6) { echoAt_ = (echoAt_ + 1) % echoFrames_; echoLive_ = true; }
    ringLive_ = ringLive_ || space.resonate > 1.0e-6;
    for (std::size_t i = 0; i < kFftSize; ++i) {
        const auto at = (inputAt_ + i) % kFftSize;
        spectrum_[i] = {static_cast<double>(inputLeft_[at]) * window_[i],
                        static_cast<double>(inputRight_[at]) * window_[i]};
    }
    fft_.transform(spectrum_, false);

    const auto widthMagnitude = std::clamp(0.5 * static_cast<double>(projection.stereoSpan), 0.0, 2.0);
    const auto signedWidth = widthMagnitude*static_cast<double>(projection.signedFacing);
    const auto basePosition = static_cast<double>(smoothedParameters_.plane.positionX);
    const std::complex<double> imaginary {0.0, 1.0};
    const auto half = kFftSize / 2;
    const auto phase = phaseFrame();
    for (std::size_t k = 1; k < half; ++k) {
        const auto packed = spectrum_[k];
        const auto mirrored = std::conj(spectrum_[kFftSize - k]);
        const auto inLeft = 0.5 * (packed + mirrored);
        const auto inRight = -0.5 * imaginary * (packed - mirrored);
        std::complex<double> outLeft, outRight;
        spatialize(inLeft, inRight, signedWidth, basePosition, outLeft, outRight);
        applyPhase(k, phase, inLeft, inRight, signedWidth, outLeft, outRight);
        if (spaced)
            applyEffects(k, space, rings, outLeft, outRight);
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
        targetMaskPresence_ = targetMask_.hasPhasePoints ? 1.0 : 0.0;
        if (targetMask_.hasNearestSupport) {
            fieldHasSupport_ = true;
            supportValues_ = targetMask_.values;
            // Once per mask change, not per sample: each pixel takes its neighbours' support
            // fading to zero 3 px away, so the soft field has no edge to jump at.
            for (int y = 0; y < kImageMaskHeight; ++y)
                for (int x = 0; x < kImageMaskWidth; ++x) {
                    float strongest = 0.0f;
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dx = -2; dx <= 2; ++dx) {
                            const auto px = x+dx, py = y+dy;
                            if (px < 0 || py < 0 || px >= kImageMaskWidth || py >= kImageMaskHeight) continue;
                            const auto falloff = 1.0f-std::sqrt(static_cast<float>(dx*dx+dy*dy))/3.0f;
                            strongest = std::max(strongest, falloff*supportValues_[static_cast<std::size_t>(py*kImageMaskWidth+px)]);
                        }
                    softSupport_[static_cast<std::size_t>(y*kImageMaskWidth+x)] = strongest;
                }
            fieldAspectX_ = targetMask_.aspectX; fieldAspectY_ = targetMask_.aspectY;
        }
        rowWeightTarget_ = spaceRowWeights(targetMask_);
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
    const auto smoothAngle = [this](float& current, float target) {
        current = std::remainder(current + static_cast<float>(frameSmoothing_)
            * std::remainder(target-current, 360.0f), 360.0f);
    };
    smoothAngle(currentPlane.pitchDegrees, targetPlane.pitchDegrees);
    smoothAngle(currentPlane.yawDegrees, targetPlane.yawDegrees);
    smoothAngle(currentPlane.rollDegrees, targetPlane.rollDegrees);
    smooth(smoothedParameters_.imageAmount, parameters_.imageAmount);
    smooth(smoothedParameters_.edge, parameters_.edge);
    smooth(smoothedParameters_.smooth, parameters_.smooth);
    previousFieldFollow_ = fieldFollow_;
    fieldFollow_ = smoothedParameters_.smooth > 0.0f
        ? -std::expm1(-1.0 / (sampleRate_ * smoothedParameters_.smooth)) : 1.0;
    smooth(smoothedParameters_.rest, parameters_.rest);
    smooth(smoothedParameters_.interfere, parameters_.interfere);
    smooth(smoothedParameters_.feedback, parameters_.feedback);
    smooth(smoothedParameters_.echo, parameters_.echo);
    smooth(smoothedParameters_.resonate, parameters_.resonate);
    smooth(smoothedParameters_.echoTime, parameters_.echoTime);
    for (std::size_t y = 0; y < rowWeight_.size(); ++y)
        smooth(rowWeight_[y], rowWeightTarget_[y]);
    geometryBlend_ = 0.0f;
    smooth(maskPresence_, targetMaskPresence_);
    previousNearest_ = nearest_;
    if (targetMask_.hasNearestSupport)
        for (std::size_t i = 0; i < nearest_.size(); ++i) {
            smooth(nearest_[i].x,targetMask_.nearestSupport[i].x);
            smooth(nearest_[i].y,targetMask_.nearestSupport[i].y);
        }
    auto imageTransform = smoothedParameters_.plane;
    imageTransform.scaleX *= fieldAspectX_;
    imageTransform.scaleY *= fieldAspectY_;
    previousImagePlane_ = imagePlane_;
    imagePlane_ = projectPlane(imageTransform);
}

double ImageSpatializer::fieldRotation(double left, double right)
{
    const auto mid = 0.5*(left+right), side = 0.5*(right-left);
    const auto level = std::hypot(mid, side);
    geometryBlend_ = std::min(1.0f, geometryBlend_ + 1.0f/static_cast<float>(kHopSize));
    const auto facing = previousImagePlane_.signedFacing
        + geometryBlend_*(imagePlane_.signedFacing-previousImagePlane_.signedFacing);
    const auto amount = maskPresence_*smoothedParameters_.imageAmount*std::abs(facing);
    if (level < 1.0e-9 || amount < 1.0e-6 || !fieldHasSupport_)
        return 0.0;
    const auto polarity = mid >= 0.0 ? 1.0 : -1.0;
    const auto angle = std::atan2(polarity*side, std::abs(mid));
    // Same calibrated level axis as the measured +14 dB scope. No audio gain is applied.
    const auto radius = std::clamp(1.0 + std::log10(std::max(level*5.0, 1.0e-6))/3.0, 0.0, 1.0);
    const auto corner = [this](int i) {
        const auto& p = previousImagePlane_.corners[static_cast<std::size_t>(i)];
        const auto& q = imagePlane_.corners[static_cast<std::size_t>(i)];
        return ProjectedPoint {p.stereo+geometryBlend_*(q.stereo-p.stereo),
            p.frequency+geometryBlend_*(q.frequency-p.frequency),0};
    };
    const auto a = corner(0),b = corner(1),d = corner(3);
    const auto bx = b.stereo-a.stereo, by = b.frequency-a.frequency;
    const auto dx = d.stereo-a.stereo, dy = d.frequency-a.frequency;
    const auto determinant = static_cast<double>(bx*dy-by*dx);
    if (std::abs(determinant) < 1.0e-7) return 0.0;
    const auto imageCoordinates = [&](double direction) {
        const auto x = parameters_.phaseProjection == PhaseProjection::Cartesian
            ? 2*radius*std::sin(direction) : direction*4/std::numbers::pi;
        const auto y = parameters_.phaseProjection == PhaseProjection::Cartesian
            ? 2*(radius*std::cos(direction)-0.5) : 4*(radius-0.5);
        return ImageMask::PhasePoint {static_cast<float>(63*((x-a.stereo)*dy-(y-a.frequency)*dx)/determinant),
            static_cast<float>(63*(1-(bx*(y-a.frequency)-by*(x-a.stereo))/determinant))};
    };
    // One search from a start angle: image support, then the original sample's radius.
    // Only queries immutable image-coordinate maps; no rasterization/field compilation here.
    struct Solution { double angle = 0; bool hard = false; double soft = 0; };
    const auto solve = [&](double start) {
        Solution solution {start, false, 0.0};
        auto targetAngle = start;
        for (int iteration = 0; iteration < 16; ++iteration) {
            const auto query = imageCoordinates(targetAngle);
            const auto px = std::clamp(query.x,0.0f,63.0f), py = std::clamp(query.y,0.0f,63.0f);
            const auto ix = static_cast<int>(px), iy = static_cast<int>(py);
            const auto nx = std::min(ix+1,63), ny = std::min(iy+1,63);
            const auto tx = px-static_cast<float>(ix), ty = py-static_cast<float>(iy);
            const auto read = [&](int x,int y) {
                const auto at = static_cast<std::size_t>(y*64+x);
                const auto& p = previousNearest_[at]; const auto& q = nearest_[at];
                return ImageMask::PhasePoint {p.x+geometryBlend_*(q.x-p.x),p.y+geometryBlend_*(q.y-p.y)};
            };
            const auto p = read(ix,iy), q = read(nx,iy), r = read(ix,ny), s = read(nx,ny);
            const auto nearX = (p.x*(1-tx)+q.x*tx)*(1-ty)+(r.x*(1-tx)+s.x*tx)*ty;
            const auto nearY = (p.y*(1-tx)+q.y*tx)*(1-ty)+(r.y*(1-tx)+s.y*tx)*ty;
            const auto u = nearX/63, v = 1-nearY/63;
            const auto worldX = a.stereo+u*bx+v*dx, worldY = a.frequency+u*by+v*dy;
            const auto scopeX = 0.5*worldX, scopeY = 0.5+0.5*worldY;
            const auto previousAngle = targetAngle;
            if (parameters_.phaseProjection == PhaseProjection::Cartesian) {
                if (scopeY < 0) break;
                targetAngle = std::atan2(scopeX,scopeY);
            } else targetAngle = std::clamp(static_cast<double>(worldX)*std::numbers::pi/4,-0.5*std::numbers::pi,0.5*std::numbers::pi);
            if (std::abs(targetAngle-previousAngle) < 1.0e-5) break;
        }
        solution.angle = targetAngle;
        const auto finalPoint = imageCoordinates(targetAngle);
        if (!std::isfinite(finalPoint.x) || !std::isfinite(finalPoint.y)
            || finalPoint.x < -2 || finalPoint.x > 65 || finalPoint.y < -2 || finalPoint.y > 65) return solution;
        const auto fx = static_cast<int>(std::lround(finalPoint.x)), fy = static_cast<int>(std::lround(finalPoint.y));
        for (int y = -2; y <= 2; ++y)
            for (int x = -2; x <= 2; ++x) {
                const auto px = fx+x,py = fy+y;
                if (px>=0 && px<64 && py>=0 && py<64)
                    solution.hard = solution.hard || supportValues_[static_cast<std::size_t>(py*64+px)]>0.001f;
            }
        // Soft support: bilinear in the softened map, fading out over 3 px beyond the bitmap.
        const auto cx = std::clamp(finalPoint.x,0.0f,63.0f), cy = std::clamp(finalPoint.y,0.0f,63.0f);
        const auto outside = std::hypot(finalPoint.x-cx, finalPoint.y-cy);
        const auto ix = std::min(static_cast<int>(cx),62), iy = std::min(static_cast<int>(cy),62);
        const auto tx = cx-static_cast<float>(ix), ty = cy-static_cast<float>(iy);
        const auto at = [this](int x,int y) { return softSupport_[static_cast<std::size_t>(y*64+x)]; };
        const auto value = (at(ix,iy)*(1-tx)+at(ix+1,iy)*tx)*(1-ty)+(at(ix,iy+1)*(1-tx)+at(ix+1,iy+1)*tx)*ty;
        solution.soft = std::clamp(3.0*static_cast<double>(value)*std::max(0.0,1.0-static_cast<double>(outside)/3.0),0.0,1.0);
        return solution;
    };
    const auto first = solve(angle);
    const auto edge = static_cast<double>(smoothedParameters_.edge);
    // Fallback start for samples whose own direction finds no support.
    const auto needFallback = !first.hard || edge < 1.0;
    const auto second = needFallback ? solve(-std::copysign(0.8,angle)) : Solution {};
    const auto hard = first.hard ? first.angle-angle : second.hard ? second.angle-angle : 0.0;
    // Soft: attraction fades with the distance moved, so a sample between two strokes is
    // barely moved instead of jumping from one stroke's angle to the other's.
    const auto width = kAttraction*std::pow(kAttractionSpan, edge);
    // Support also hardens with Edge, so the soft map's halo shrinks toward the hard one.
    const auto pull = [width,edge](const Solution& solution, double from) {
        const auto moved = (solution.angle-from)/width;
        const auto support = edge*(solution.hard ? 1.0 : 0.0) + (1.0-edge)*solution.soft;
        return support*std::exp(-moved*moved);
    };
    const auto pullFirst = pull(first, angle), pullSecond = pull(second, angle);
    const auto soft = pullFirst*(first.angle-angle) + (1.0-pullFirst)*pullSecond*(second.angle-angle);
    auto offset = edge >= 1.0 ? hard : soft;
    // Rest steers what the image cannot hold (no support at this level) toward the centre
    // or the sides, by how little the image holds it. Level is never invented or removed.
    const auto rest = static_cast<double>(smoothedParameters_.rest);
    if (std::abs(rest) > 1.0e-6) {
        const auto held = edge >= 1.0 ? (first.hard || second.hard ? 1.0 : 0.0)
                                      : std::clamp(pullFirst+(1.0-pullFirst)*pullSecond, 0.0, 1.0);
        // Sideways is continuous through mono (no jump between the two sides).
        const auto target = rest < 0.0 ? 0.0 : kRestSide*std::tanh(angle/0.15);
        offset += std::abs(rest)*(1.0-held)*(target-angle);
    }
    if (std::abs(offset) < 1.0e-12) return 0.0; // nothing to hold or steer
    // Avoid a waveform discontinuity at Polar Sample's polarity-fold seam.
    const auto seam = std::min(1.0, std::abs(mid)/(0.05*level));
    return amount*seam*offset;
}

void ImageSpatializer::constrainField(double& left, double& right)
{
    auto rotation = fieldRotation(left, right);
    const auto follow = previousFieldFollow_ + geometryBlend_ * (fieldFollow_ - previousFieldFollow_);
    if (follow < 1.0) {
        // Interpolate on the unit circle, including across the +/-pi seam.
        // Only steering is smoothed: no source-waveform filtering or gain crossfade.
        fieldRotationCurrent_ = std::remainder(fieldRotationCurrent_
            + follow * std::remainder(rotation - fieldRotationCurrent_, 2.0 * std::numbers::pi),
            2.0 * std::numbers::pi);
        rotation = fieldRotationCurrent_;
    } else {
        fieldRotationCurrent_ = rotation; // zero Smooth follows the original path exactly
    }
    if (std::abs(rotation) < 1.0e-12) return;
    const auto mid = 0.5*(left+right), side = 0.5*(right-left);
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

ImageSpatializer::PhaseFrame ImageSpatializer::phaseFrame() const
{
    const auto facing = std::abs(std::cos(static_cast<double>(smoothedParameters_.plane.yawDegrees)*std::numbers::pi/180));
    return {maskPresence_ * smoothedParameters_.imageAmount*facing,
            0.5 * std::asin(std::clamp(static_cast<double>(smoothedParameters_.plane.positionX), -1.0, 1.0))};
}

void ImageSpatializer::applyPhase(std::size_t bin, const PhaseFrame& frame, std::complex<double> left,
                                  std::complex<double> right, double signedWidth, std::complex<double>& outLeft,
                                  std::complex<double>& outRight) const
{
    const auto amount = frame.amount;
    if (amount < 1.0e-9)
        return;
    const auto hz = static_cast<double>(bin) * sampleRate_ / static_cast<double>(kFftSize);
    const auto position = std::min(1.0, hz / kHighHz) * static_cast<double>(kPhaseAnchors - 1);
    const auto at = std::min(static_cast<std::size_t>(position), kPhaseAnchors - 2);
    const auto t = position - static_cast<double>(at);
    const auto& a = phaseControls_[at];
    const auto& b = phaseControls_[at + 1];
    const auto baseAngle = frame.baseAngle;
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

void ImageSpatializer::applyEffects(std::size_t bin, const SpaceAmounts& amounts, const Resonators& rings,
                                    std::complex<double>& left, std::complex<double>& right)
{
    const auto hz = static_cast<double>(bin) * sampleRate_ / static_cast<double>(kFftSize);
    if (amounts.interfere > 1.0e-6) {
        const auto response = interfereResponse(imagePlane_, amounts, hz);
        left *= response.left;
        right *= response.right;
    }
    if (amounts.resonate > 1.0e-6) {
        // y = x + g·advance·y[previous hop]: a bin-centred partial is reinforced and rings on
        // (about 2.3 s at the centre of a resonance). The added part is scaled by (1 − g),
        // so a steady partial gains at most kResonanceBoost: bounded by construction.
        const auto peak = resonance(rings, hz);
        if (peak > 1.0e-4) {
            const auto ring = kResonanceFeedback * peak;
            const auto advance = std::polar(ring, 2.0 * std::numbers::pi * static_cast<double>(bin * kHopSize)
                                                  / static_cast<double>(kFftSize));
            ringLeft_[bin] = left + advance * ringLeft_[bin];
            ringRight_[bin] = right + advance * ringRight_[bin];
            const auto mix = amounts.resonate * kResonanceBoost * peak * (1.0 - ring);
            left += mix * ringLeft_[bin];
            right += mix * ringRight_[bin];
        } else ringLeft_[bin] = ringRight_[bin] = {};
    }
    if (amounts.echo > 1.0e-6) {
        // Spectral delay: this bin's own echo time from its row's distance, read between two
        // frames; the image's bright rows echo loudest. Repeats ≤ kMaxFeedback: bounded.
        const auto frames = std::clamp(echoSeconds(imagePlane_, hz, amounts.echoTime) * sampleRate_ / static_cast<double>(kHopSize),
                                       1.0, static_cast<double>(echoFrames_ - 2));
        const auto whole = static_cast<std::size_t>(frames);
        const auto fraction = static_cast<float>(frames - static_cast<double>(whole));
        const auto half = kFftSize / 2;
        const auto slot = [&](std::size_t back) { return (((echoAt_ + echoFrames_ - back) % echoFrames_) * half + bin) * 2; };
        const auto a = slot(whole), b = slot(whole + 1);
        const auto delayedLeft = (1.0f - fraction) * echoHistory_[a] + fraction * echoHistory_[b];
        const auto delayedRight = (1.0f - fraction) * echoHistory_[a + 1] + fraction * echoHistory_[b + 1];
        const auto repeats = static_cast<float>(kMaxFeedback * amounts.feedback);
        const auto write = slot(0);
        echoHistory_[write] = std::complex<float>(left) + repeats * delayedLeft;
        echoHistory_[write + 1] = std::complex<float>(right) + repeats * delayedRight;
        // Echo 100% gives twice the delayed level (it was too quiet beside the direct sound).
        const auto weight = maskPresence_ > 0.5 ? 0.3 + 0.7 * spaceRowWeight(rowWeight_, hz) : 1.0;
        const auto mix = kEchoLevel * amounts.echo * weight * (1.0 - 0.5 * static_cast<double>(repeats));
        left += mix * std::complex<double>(delayedLeft);
        right += mix * std::complex<double>(delayedRight);
    }
}

std::uint32_t ImageSpatializer::takeSafetyResets()
{
    const auto resets = safetyResets_;
    safetyResets_ = 0;
    return resets;
}

} // namespace holotoner
