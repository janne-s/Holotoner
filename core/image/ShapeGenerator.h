#pragma once

#include "core/image/ImageMask.h"

#include <array>

namespace holotoner {

/** Generated image sources: white marks on transparent black, deterministic. */
enum class ShapeKind { Grid, Checker, Rings, Triangle, Sierpinski, Spiral, Lissajous, Smiley };

inline constexpr std::array<const char*, 8> kShapeNames {"Grid", "Checker", "Rings", "Triangle",
                                                         "Sierpinski", "Spiral", "Lissajous", "Smiley"};
inline constexpr int kShapeMinimumOrder = 1;
inline constexpr int kShapeMaximumOrder = 8;
inline constexpr int kShapeSize = 256; ///< pixels per side

/**
 * Order is density, Sierpinski's depth, or Smiley's stroke weight. Out-of-range
 * orders are clamped. Same kind and order always give the same pixels.
 */
RgbaImage generateShape(ShapeKind kind, int order);

} // namespace holotoner
