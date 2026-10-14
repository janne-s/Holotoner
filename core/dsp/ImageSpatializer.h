#pragma once

#include "core/dsp/Fft.h"
#include "core/dsp/PhaseMap.h"
#include "core/dsp/SpaceCues.h"
#include "core/geometry/PlaneProjection.h"
#include "core/image/ImageMask.h"

#include <array>
#include <complex>
#include <cstdint>
#include <vector>

namespace holotoner {

/**
 * Fixed-latency image spatializer.
 *
 * Phase combines bounded spectral phase drift with a projected image steering field.
 * Uses a 2048-sample, four-times-overlapped spectral stage plus image steering.
 * Wet and Dry have the same fixed latency. No allocation after prepare.
 */
class ImageSpatializer {
public:
    static constexpr std::size_t kFftSize = 2048;
    static constexpr std::size_t kHopSize = kFftSize / 4;
    struct Parameters {
        PlaneTransform plane;
        PhaseProjection phaseProjection = PhaseProjection::Cartesian;
        float imageAmount = 1.0f;
        /// 1: each sample snaps to its nearest image support (crisp polar image, audible
        /// grit where the nearest stroke changes). Lower: a narrower, softer pull, so samples
        /// between strokes move less (less grit, a fainter image). Measured, not free.
        float edge = 1.0f;
        /// Optional shortest-arc interpolation of image steering, in seconds (0..5 ms).
        /// Zero retains sample-exact Snap; positive times trade image sharpness for less grit.
        float smooth = 0.0f;
        /// Samples whose level finds no image support: -1 turns them to the centre (mono),
        /// +1 out to the sides, 0 leaves them where they are. A rotation: level is kept.
        float rest = 0.0f;
        float wet = 1.0f;
        /// 1: output only what Holotoner changes (processed minus dry, scaled by Wet).
        bool delta = false;
        // Space effects (see SpaceCues.h): all 0..1; 0 bypasses each exactly.
        float interfere = 0.0f;
        float feedback = 0.0f; ///< Interfere's resonance and Echo's repeats
        float echo = 0.0f;
        float resonate = 0.0f;
        float echoTime = 0.25f; ///< seconds at the plane centre (the wrapper resolves note sync)
    };

    void prepare(double sampleRate);
    void reset();
    void setParameters(Parameters parameters);
    void process(float* left, float* right, int sampleCount, const ImageMask* mask);

    int latencySamples() const { return static_cast<int>(kFftSize - 1); }
    PlaneTransform currentPlane() const { return smoothedParameters_.plane; } // audio thread only
    std::uint32_t takeSafetyResets();

private:
    void renderFrame(const ImageMask* mask);
    void smoothFrameState(const ImageMask* mask);
    static void spatialize(std::complex<double> left, std::complex<double> right,
                           double signedWidth, double basePosition,
                           std::complex<double>& outputLeft, std::complex<double>& outputRight);

    double sampleRate_ = 48000.0;
    Fft fft_;
    std::array<double, kFftSize> window_ {};
    std::array<float, kFftSize> inputLeft_ {}, inputRight_ {};
    std::array<float, kFftSize> dryLeft_ {}, dryRight_ {};
    std::array<double, 2 * kFftSize> outputLeft_ {}, outputRight_ {};
    std::array<std::complex<double>, kFftSize> spectrum_ {};
    std::size_t inputAt_ = 0;
    std::size_t dryAt_ = 0;
    std::size_t outputAt_ = 0;
    std::size_t filled_ = 0;
    std::size_t sinceHop_ = 0;
    Parameters parameters_;
    Parameters smoothedParameters_;
    ImageMask targetMask_;
    const ImageMask* targetMaskIdentity_ = nullptr;
    double maskPresence_ = 0.0;
    double targetMaskPresence_ = 0.0;
    struct PhaseControls { double angle = 0.0, relative = 0.0, common = 0.0; };
    static constexpr std::size_t kPhaseAnchors = 33;
    std::array<PhaseControls, kPhaseAnchors> phaseControls_ {};
    bool phaseInitialized_ = false;
    double fieldTime_ = 0.0;
    float geometryBlend_ = 1.0f;
    double fieldRotationCurrent_ = 0.0, fieldFollow_ = 1.0, previousFieldFollow_ = 1.0;
    std::array<ImageMask::PhasePoint,kImageMaskWidth*kImageMaskHeight> nearest_ {},previousNearest_ {};
    std::array<float,kImageMaskWidth*kImageMaskHeight> supportValues_ {};
    std::array<float,kImageMaskWidth*kImageMaskHeight> softSupport_ {}; ///< support with a 3 px falloff
    bool fieldHasSupport_ = false;
    float fieldAspectX_ = 1,fieldAspectY_ = 1;
    ProjectedPlane imagePlane_,previousImagePlane_;
    void constrainField(double& left, double& right);
    double fieldRotation(double left, double right);
    // Space: smoothed row brightness (compiled on mask change, never per bin).
    std::array<double,kImageMaskHeight> rowWeight_ {},rowWeightTarget_ {};
    // Echo: a per-bin spectral history of frames, sized in prepare for kEchoMaxSeconds.
    std::vector<std::complex<float>> echoHistory_;
    std::size_t echoFrames_ = 0, echoAt_ = 0;
    bool echoLive_ = false;
    // Resonate: per-bin, per-ear ringing state.
    std::array<std::complex<double>, kFftSize / 2> ringLeft_ {}, ringRight_ {};
    bool ringLive_ = false;
    void applyEffects(std::size_t bin, const SpaceAmounts& amounts, const Resonators& rings,
                      std::complex<double>& left, std::complex<double>& right);
    void buildPhaseControls(const ProjectedPlane&);
    /// Per-frame inputs of applyPhase, computed once per hop instead of once per bin.
    struct PhaseFrame { double amount = 0.0, baseAngle = 0.0; };
    PhaseFrame phaseFrame() const;
    void applyPhase(std::size_t bin, const PhaseFrame& frame, std::complex<double> left, std::complex<double> right,
                    double signedWidth, std::complex<double>& outLeft, std::complex<double>& outRight) const;
    double frameSmoothing_ = 1.0;
    float wetCurrent_ = 1.0f;
    float deltaCurrent_ = 0.0f; ///< smoothed Delta, so switching never clicks
    float wetRate_ = 1.0f;
    std::uint32_t safetyResets_ = 0;
};

} // namespace holotoner
