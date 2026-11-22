#pragma once

#include <tanh/core/AbiNamespace.h>
#include <tanh/core/Exports.h>
#include <tanh/modulation/ParameterBackend.h>

#include <optional>
#include <string_view>

namespace thl::inline THL_ABI_NAMESPACE {
class State;
}

namespace thl::inline THL_ABI_NAMESPACE::modulation {

/**
 * @brief ParameterBackend over a thl::State.
 *
 * Bindings point straight into the State's ParameterRecord (the native-type
 * atomic of its AtomicCacheEntry, m_in_gesture, and m_change_stamp as the
 * host-change stamp: every write through State counts as host activity).
 * Records are heap-owned by
 * the State and never move, so the pointers stay valid until State::clear()
 * or the State's destruction — the same lifetime ParameterHandle has.
 *
 * Only available when tanh is built with the State component.
 */
class TANH_API StateParameterBackend final : public ParameterBackend {
public:
    explicit StateParameterBackend(thl::State& state) : m_state(state) {}

    [[nodiscard]] std::optional<ParameterBinding> find(std::string_view key) const override;
    /// Throws what thl::State throws for @p key: StateKeyNotFoundException, or
    /// StateGroupNotFoundException for a missing group.
    [[noreturn]] void throw_not_found(std::string_view key) const override;

private:
    thl::State& m_state;
};

}  // namespace thl::inline THL_ABI_NAMESPACE::modulation
