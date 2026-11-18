#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/utils/RealtimeSanitizer.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <span>

namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics {

/// How the channels of a dynamics processor share one gain.
enum class StereoLink : uint8_t {
    Linked,    ///< the loudest channel drives all (the image stays where it is)
    Average,   ///< the channels' mean power drives all
    DualMono,  ///< every channel its own gain
};

/// The one power of linked channels: their maximum (Linked) or mean (Average). Not for DualMono,
/// which links nothing.
[[nodiscard]] inline float link_powers(StereoLink link,
                                       std::span<const float> powers) TANH_NONBLOCKING_FUNCTION {
    assert(link != StereoLink::DualMono);
    if (powers.empty()) { return 0.0f; }
    if (link == StereoLink::Linked) { return *std::ranges::max_element(powers); }
    float sum = 0.0f;
    for (const float power : powers) { sum += power; }
    return sum / static_cast<float>(powers.size());
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::dynamics
