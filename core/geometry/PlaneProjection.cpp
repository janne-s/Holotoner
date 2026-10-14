#include "PlaneProjection.h"

#include <algorithm>
#include <cmath>

namespace holotoner {
namespace {

constexpr float kPi = 3.14159265358979323846f;

float radians(float degrees) { return degrees * kPi / 180.0f; }

float finiteOr(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

struct Point3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Point3 rotateX(Point3 point, float angle)
{
    const auto c = std::cos(angle);
    const auto s = std::sin(angle);
    return {point.x, c * point.y - s * point.z, s * point.y + c * point.z};
}

Point3 rotateY(Point3 point, float angle)
{
    const auto c = std::cos(angle);
    const auto s = std::sin(angle);
    return {c * point.x + s * point.z, point.y, -s * point.x + c * point.z};
}

Point3 rotateZ(Point3 point, float angle)
{
    const auto c = std::cos(angle);
    const auto s = std::sin(angle);
    return {c * point.x - s * point.y, s * point.x + c * point.y, point.z};
}

} // namespace

ProjectedPlane projectPlane(const PlaneTransform& transform)
{
    const auto pitch = radians(std::remainder(finiteOr(transform.pitchDegrees, 0.0f), 360.0f));
    const auto yaw = radians(std::remainder(finiteOr(transform.yawDegrees, 0.0f), 360.0f));
    const auto roll = radians(std::remainder(finiteOr(transform.rollDegrees, 0.0f), 360.0f));
    const auto sx = std::max(0.0f, finiteOr(transform.scaleX, 1.0f));
    const auto sy = std::max(0.0f, finiteOr(transform.scaleY, 1.0f));
    const auto positionX = finiteOr(transform.positionX, 0.0f);
    const auto positionY = finiteOr(transform.positionY, 0.0f);
    constexpr std::array<Point3, 4> unit {{{-1.0f, -1.0f, 0.0f},
                                           {1.0f, -1.0f, 0.0f},
                                           {1.0f, 1.0f, 0.0f},
                                           {-1.0f, 1.0f, 0.0f}}};

    ProjectedPlane result;
    float minX = 0.0f;
    float maxX = 0.0f;
    float minY = 0.0f;
    float maxY = 0.0f;

    for (std::size_t i = 0; i < unit.size(); ++i) {
        auto point = Point3 {unit[i].x * sx, unit[i].y * sy, 0.0f};
        point = rotateX(point, pitch);
        point = rotateY(point, yaw);
        point = rotateZ(point, roll);
        result.corners[i] = {point.x + positionX, point.y + positionY, point.z};
        if (i == 0) {
            minX = maxX = result.corners[i].stereo;
            minY = maxY = result.corners[i].frequency;
        } else {
            minX = std::min(minX, result.corners[i].stereo);
            maxX = std::max(maxX, result.corners[i].stereo);
            minY = std::min(minY, result.corners[i].frequency);
            maxY = std::max(maxY, result.corners[i].frequency);
        }
    }

    result.signedFacing = std::cos(yaw);
    result.stereoSpan = maxX - minX;
    result.frequencySpan = maxY - minY;
    return result;
}

ProjectedPoint planePoint(const ProjectedPlane& plane, float u, float v)
{
    // The plane is affine: a + u(b - a) + v(d - a) of corners 0, 1 and 3.
    const auto& a = plane.corners[0];
    const auto& b = plane.corners[1];
    const auto& d = plane.corners[3];
    return {a.stereo + u * (b.stereo - a.stereo) + v * (d.stereo - a.stereo),
            a.frequency + u * (b.frequency - a.frequency) + v * (d.frequency - a.frequency),
            a.depth + u * (b.depth - a.depth) + v * (d.depth - a.depth)};
}

} // namespace holotoner
