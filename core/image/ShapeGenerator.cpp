#include "core/image/ShapeGenerator.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace holotoner {
namespace {

constexpr double kLine = 2.5 / kShapeSize; ///< half line width in unit coordinates

// Distance from (x, y) to the segment a-b, all in unit coordinates.
double segmentDistance(double x, double y, double ax, double ay, double bx, double by)
{
    const auto dx = bx - ax, dy = by - ay;
    const auto t = std::clamp(((x - ax) * dx + (y - ay) * dy) / std::max(dx * dx + dy * dy, 1.0e-12), 0.0, 1.0);
    return std::hypot(x - (ax + t * dx), y - (ay + t * dy));
}

bool inside(ShapeKind kind, int order, double u, double v)
{
    // u right, v down, both 0..1 across the bitmap; x/y centred, y up.
    const auto x = 2.0 * u - 1.0, y = 1.0 - 2.0 * v;
    const auto n = static_cast<double>(order);
    switch (kind) {
    case ShapeKind::Grid: {
        const auto cells = n + 1.0;
        const auto near = [&](double c) { const auto f = c * cells; return std::abs(f - std::round(f)) < kLine * cells; };
        return near(u) || near(v);
    }
    case ShapeKind::Checker: {
        const auto cells = 2 * order;
        const auto cx = std::min(static_cast<int>(u * cells), cells - 1), cy = std::min(static_cast<int>(v * cells), cells - 1);
        return ((cx + cy) & 1) == 0;
    }
    case ShapeKind::Rings: {
        const auto r = std::hypot(x, y) * (n + 0.5);
        return r <= n + 0.5 && std::abs(r - std::round(r)) < 2.0 * kLine * (n + 0.5) && std::round(r) >= 1.0;
    }
    case ShapeKind::Triangle: {
        // Nested equilateral outlines around the centre.
        for (int i = 1; i <= order; ++i) {
            const auto s = 0.95 * static_cast<double>(i) / n;
            const double ax = 0, ay = s, bx = -0.866 * s, by = -0.5 * s, cx = 0.866 * s, cy = -0.5 * s;
            if (std::min({segmentDistance(x, y, ax, ay, bx, by), segmentDistance(x, y, bx, by, cx, cy),
                          segmentDistance(x, y, cx, cy, ax, ay)}) < 2.0 * kLine)
                return true;
        }
        return false;
    }
    case ShapeKind::Sierpinski: {
        // Pascal's triangle mod 2: row r, column c (0..r) is filled when C(r, c) is odd.
        const auto rows = 1 << std::min(order, 7);
        const auto r = std::min(static_cast<int>(v * rows), rows - 1);
        const auto s = (u - 0.5) * rows + 0.5 * (r + 1);
        if (s < 0.0) return false;
        const auto c = static_cast<int>(s);
        return c <= r && (c & r) == c;
    }
    case ShapeKind::Spiral: {
        const auto radius = std::hypot(x, y);
        const auto turns = radius / 0.97 * n; // the last whole turn ends on a 0.97 rim
        const auto angle = (std::atan2(y, x) / (2.0 * std::numbers::pi) + 0.5);
        const auto phase = turns - angle;
        // Only whole turns: the turn that would start past the rim is not drawn.
        return std::round(phase) <= n - 1.0 && std::abs(phase - std::round(phase)) / n < 2.0 * kLine;
    }
    case ShapeKind::Smiley: {
        const auto stroke = 2.0 * kLine * (0.6 + 0.2 * n);
        const auto outline = std::abs(std::hypot(x, y) - 0.84) < stroke;
        const auto eye = [&](double cx) {
            const auto dx = (x - cx) / 0.075, dy = (y - 0.28) / 0.11;
            return dx * dx + dy * dy <= 1.0;
        };
        const auto smile = y <= -0.12 && std::abs(std::hypot(x, y + 0.08) - 0.5) < stroke;
        return outline || eye(-0.28) || eye(0.28) || smile;
    }
    case ShapeKind::Lissajous:
        return false; // stamped along the curve in generateShape
    }
    return false;
}

} // namespace

RgbaImage generateShape(ShapeKind kind, int order)
{
    order = std::clamp(order, kShapeMinimumOrder, kShapeMaximumOrder);
    RgbaImage image;
    image.width = image.height = kShapeSize;
    image.pixels.assign(static_cast<std::size_t>(kShapeSize * kShapeSize * 4), 0);
    const auto set = [&image](int px, int py) {
        if (px < 0 || py < 0 || px >= kShapeSize || py >= kShapeSize) return;
        const auto at = static_cast<std::size_t>((py * kShapeSize + px) * 4);
        image.pixels[at] = image.pixels[at + 1] = image.pixels[at + 2] = image.pixels[at + 3] = 255;
    };
    if (kind == ShapeKind::Lissajous) {
        // x = sin(n t + offset), y = sin((n + 1) t): a closed curve, stamped every ~1/4 px.
        const auto n = static_cast<double>(order);
        constexpr int samples = 32768;
        for (int i = 0; i < samples; ++i) {
            const auto t = 2.0 * std::numbers::pi * i / samples;
            const auto x = 0.95 * std::sin(n * t + 0.5 * std::numbers::pi / (n + 1.0));
            const auto y = 0.95 * std::sin((n + 1.0) * t);
            const auto cx = static_cast<int>(std::lround((x + 1.0) * 0.5 * kShapeSize - 0.5));
            const auto cy = static_cast<int>(std::lround((1.0 - y) * 0.5 * kShapeSize - 0.5));
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx)
                    if (dx * dx + dy * dy <= 5) set(cx + dx, cy + dy);
        }
        return image;
    }
    for (int py = 0; py < kShapeSize; ++py)
        for (int px = 0; px < kShapeSize; ++px)
            if (inside(kind, order, (px + 0.5) / kShapeSize, (py + 0.5) / kShapeSize))
                set(px, py);
    return image;
}

} // namespace holotoner
