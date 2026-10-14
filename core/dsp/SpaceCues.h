#pragma once

#include "core/geometry/PlaneProjection.h"
#include "core/image/ImageMask.h"

#include <array>
#include <complex>

namespace holotoner {

/**
 * Space effects: the plane's 3D pose turned into strong spectral effects. Pure functions
 * shared by the engine (per FFT bin) and the editor's response view.
 *
 * Frequency maps to the plane's local v by log frequency, kSpaceLowHz at the bottom edge
 * to kSpaceHighHz at the top. The listener's head is kListenerDistance plane units in
 * front of the plane centre (+z toward it).
 *
 * - Interfere: each ear also hears the row's other half, delayed by the path difference;
 *   Feedback makes that comb resonant. Facing the listener both paths are equal: flat.
 * - Echo: each frequency echoes after Time scaled by its row's distance (kEchoMinSeconds…
 *   kEchoMaxSeconds), louder on bright rows; Feedback repeats it. The plane centre at rest
 *   echoes exactly at Time, so a note-synced Time stays in tempo there.
 * - Resonate: bright rows ring at the frequency of their projected height.
 */
inline constexpr double kSpaceLowHz = 60.0;
inline constexpr double kSpaceHighHz = 16000.0;
inline constexpr double kListenerDistance = 2.0;
inline constexpr double kEchoMinSeconds = 0.04;
inline constexpr double kEchoMaxSeconds = 2.0;
inline constexpr double kMaxFeedback = 0.85;

struct SpaceAmounts {
    double interfere = 0.0; ///< 0..1
    double feedback = 0.0;  ///< 0..1, scaled to kMaxFeedback
    double echo = 0.0;      ///< 0..1
    double resonate = 0.0;  ///< 0..1
    double echoTime = 0.25; ///< seconds: the echo time at the plane centre at rest (free or note-synced)
};

struct EarResponse {
    std::complex<double> left {1.0, 0.0};
    std::complex<double> right {1.0, 0.0};
};

bool spaceActive(const SpaceAmounts& amounts);

/** Local v (0 bottom ... 1 top) that a frequency in Hz occupies. */
double spaceRowPosition(double hz);

/** Mask row brightness (index 0 = top row), normalized to the brightest row; zero without a source. */
std::array<double, kImageMaskHeight> spaceRowWeights(const ImageMask& mask);

/** Brightness at a frequency, linearly between rows. */
double spaceRowWeight(const std::array<double, kImageMaskHeight>& weights, double hz);

/** Interfere (with Feedback) per ear at one frequency; exactly 1 when Interfere is 0. */
EarResponse interfereResponse(const ProjectedPlane& plane, const SpaceAmounts& amounts, double hz);

/** Echo time in seconds for a frequency: `base` scaled by its row's distance from the listener. */
double echoSeconds(const ProjectedPlane& plane, double hz, double base);

/** The resonators: one per bright image row, at the frequency of its projected height. */
struct Resonators {
    std::array<double, kImageMaskHeight> hz {};
    std::array<double, kImageMaskHeight> weight {};
    int count = 0;
};
Resonators resonators(const ProjectedPlane& plane, const std::array<double, kImageMaskHeight>& weights);

/** How strongly a frequency lies on a resonator, 0..1 (narrow, about a quarter tone wide). */
double resonance(const Resonators& resonators, double hz);

/// Resonate's ringing: per-hop feedback at the centre of a resonance, and its level boost.
inline constexpr double kResonanceFeedback = 0.97; // about 2.3 s to -60 dB at 48 kHz
inline constexpr double kResonanceBoost = 4.0;     // a steady partial gains at most 5× (+14 dB)

} // namespace holotoner
