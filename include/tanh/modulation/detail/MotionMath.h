#pragma once

namespace thl::modulation::detail {

// Not std::numbers: Apple Clang needs tanh/core/Numbers.h for it, and that shim
// clashes with <numbers> in the files that include the motion headers.
inline constexpr double k_motion_pi = 3.14159265358979323846;

}  // namespace thl::modulation::detail
