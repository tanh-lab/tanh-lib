#include "tanh/modulation/MotionShape.h"

#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <string_view>

namespace thl::modulation {

namespace {

constexpr double k_tau = 2.0 * std::numbers::pi;

constexpr MotionShapePoint point(double x, double y) {
    return {.m_x = static_cast<float>(x), .m_y = static_cast<float>(y)};
}

// The point at @p angle, clockwise from the top (y up), and @p radius.
MotionShapePoint polar(double angle, double radius) {
    return point(radius * std::sin(angle), radius * std::cos(angle));
}

// Walks the closed polygon @p vertices at constant time per edge.
template <size_t N>
MotionShapePoint polyline(const std::array<MotionShapePoint, N>& vertices, double u) {
    const double position = u * static_cast<double>(N);
    const auto edge = std::min(static_cast<size_t>(position), N - 1);
    const double t = position - static_cast<double>(edge);
    const MotionShapePoint& a = vertices[edge];
    const MotionShapePoint& b = vertices[(edge + 1) % N];
    return point(a.m_x + ((b.m_x - a.m_x) * t), a.m_y + ((b.m_y - a.m_y) * t));
}

// Regular polygon with @p N corners, the first at @p start (clockwise from the top), visited
// every @p step corners (2 for a star).
template <size_t N>
MotionShapePoint regular(double u, double start, size_t step) {
    std::array<MotionShapePoint, N> vertices{};
    for (size_t k = 0; k < N; ++k) {
        vertices[k] =
            polar(start + (k_tau * static_cast<double>((k * step) % N) / static_cast<double>(N)),
                  1.0);
    }
    return polyline(vertices, u);
}

// A rose r = cos(k t), traced from the top.
MotionShapePoint rose(double t, double k) {
    return polar(t, std::cos(k * t));
}

constexpr std::array<MotionShapePoint, 9> k_scribble = {point(0.0, 0.9),
                                                        point(0.7, 0.35),
                                                        point(-0.2, 0.1),
                                                        point(0.85, -0.6),
                                                        point(0.15, -0.9),
                                                        point(-0.55, -0.3),
                                                        point(-0.9, -0.75),
                                                        point(-0.6, 0.55),
                                                        point(-0.25, 0.4)};

}  // namespace

std::string_view motion_shape_name(MotionShape shape) {
    switch (shape) {
        case MotionShape::Take: return "Take";
        case MotionShape::Circle: return "Circle";
        case MotionShape::Square: return "Square";
        case MotionShape::Triangle: return "Triangle";
        case MotionShape::Line: return "Line";
        case MotionShape::Infinity: return "Infinity";
        case MotionShape::Knot: return "Knot";
        case MotionShape::Mesh: return "Mesh";
        case MotionShape::Scan: return "Scan";
        case MotionShape::Petals3: return "Petals 3";
        case MotionShape::Petals4: return "Petals 4";
        case MotionShape::Petals8: return "Petals 8";
        case MotionShape::Spiral: return "Spiral";
        case MotionShape::Heart: return "Heart";
        case MotionShape::Star: return "Star";
        case MotionShape::Ellipse: return "Ellipse";
        case MotionShape::Cloud: return "Cloud";
        case MotionShape::Scribble: return "Scribble";
        case MotionShape::Spirograph: return "Spirograph";
    }
    return "Take";
}

MotionShapePoint evaluate_motion_shape(MotionShape shape, double u) TANH_NONBLOCKING_FUNCTION {
    if (!std::isfinite(u)) { u = 0.0; }
    u -= std::floor(u);
    if (u >= 1.0) { u = 0.0; }
    const double t = k_tau * u;
    switch (shape) {
        case MotionShape::Take: return {};
        case MotionShape::Circle: return polar(t, 1.0);
        case MotionShape::Square: {
            // Corners on the diagonals, starting at the middle of the top edge.
            const double c = std::numbers::sqrt2 / 2.0;
            const std::array<MotionShapePoint, 8> v = {point(0.0, c),
                                                       point(c, c),
                                                       point(c, 0.0),
                                                       point(c, -c),
                                                       point(0.0, -c),
                                                       point(-c, -c),
                                                       point(-c, 0.0),
                                                       point(-c, c)};
            return polyline(v, u);
        }
        case MotionShape::Triangle: return regular<3>(u, 0.0, 1);
        case MotionShape::Line: return point(0.0, std::cos(t));
        case MotionShape::Infinity: return point(std::sin(t), 0.6 * std::sin(2.0 * t));
        case MotionShape::Knot: return point(std::sin(3.0 * t), std::cos(2.0 * t));
        case MotionShape::Mesh: return point(std::sin(5.0 * t), std::cos(4.0 * t));
        case MotionShape::Scan: return point(std::sin(6.0 * t), std::cos(t));
        case MotionShape::Petals3: return rose(t, 3.0);
        case MotionShape::Petals4: return rose(t, 2.0);
        case MotionShape::Petals8: return rose(t, 4.0);
        case MotionShape::Spiral: {
            const double r = 1.0 - std::abs(1.0 - (2.0 * u));  // out, then back in
            return polar(4.0 * t, std::max(r, 0.04));
        }
        case MotionShape::Heart: {
            const double s = std::sin(t);
            const double x = 16.0 * s * s * s;
            const double y = (13.0 * std::cos(t)) - (5.0 * std::cos(2.0 * t)) -
                             (2.0 * std::cos(3.0 * t)) - std::cos(4.0 * t);
            // From the dip at the top, clockwise; centred vertically.
            return point(x / 17.0, (y + 2.5) / 17.0);
        }
        case MotionShape::Star: return regular<5>(u, 0.0, 2);
        case MotionShape::Ellipse: {
            const double x = 0.35 * std::sin(t);
            const double y = std::cos(t);
            const double c = std::numbers::sqrt2 / 2.0;
            return point((x + y) * c, (y - x) * c);
        }
        case MotionShape::Cloud: return polar(t, 0.8 + (0.2 * std::cos(5.0 * t)));
        case MotionShape::Scribble: return polyline(k_scribble, u);
        case MotionShape::Spirograph: {
            // Hypotrochoid R = 5, r = 3, d = 5: closes after three turns.
            const double a = 3.0 * t;
            const double x = (2.0 * std::sin(a)) - (5.0 * std::sin(2.0 * a / 3.0));
            const double y = (2.0 * std::cos(a)) + (5.0 * std::cos(2.0 * a / 3.0));
            return point(x / 7.0, y / 7.0);
        }
    }
    return {};
}

MotionShapePoint place_motion_shape(MotionShape shape,
                                    const MotionShapeMix& mix,
                                    double u) TANH_NONBLOCKING_FUNCTION {
    const MotionShapePoint p = evaluate_motion_shape(shape, u);
    const double angle = k_tau * static_cast<double>(mix.m_rotation);
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const double size = 0.5 * std::clamp(static_cast<double>(mix.m_size), 0.0, 1.0);
    // Clockwise with y up.
    const double x = (p.m_x * c) + (p.m_y * s);
    const double y = (p.m_y * c) - (p.m_x * s);
    return point(std::clamp(0.5 + (x * size), 0.0, 1.0), std::clamp(0.5 + (y * size), 0.0, 1.0));
}

MotionShapePoint mix_motion_shapes(const MotionShapeMix& mix,
                                   double u,
                                   MotionShapePoint take) TANH_NONBLOCKING_FUNCTION {
    const MotionShapePoint a =
        mix.m_a == MotionShape::Take ? take : place_motion_shape(mix.m_a, mix, u);
    const MotionShapePoint b =
        mix.m_b == MotionShape::Take ? take : place_motion_shape(mix.m_b, mix, u);
    const double w = std::clamp(static_cast<double>(mix.m_morph), 0.0, 1.0);
    return point(a.m_x + ((b.m_x - a.m_x) * w), a.m_y + ((b.m_y - a.m_y) * w));
}

}  // namespace thl::modulation
