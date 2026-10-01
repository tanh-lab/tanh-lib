#pragma once

#include <tanh/core/Exports.h>
#include <tanh/modulation/ParameterBackend.h>

#include <optional>
#include <string_view>

namespace thl {
class State;
}

namespace thl::modulation {

/**
 * @brief ParameterBackend over a thl::State.
 *
 * Bindings point straight into the State's ParameterRecord (the native-type
 * atomic of its AtomicCacheEntry, and m_in_gesture). Records are heap-owned by
 * the State and never move, so the pointers stay valid until State::clear()
 * or the State's destruction — the same lifetime ParameterHandle has.
 *
 * Only available when tanh is built with the State component.
 */
class TANH_API StateParameterBackend final : public ParameterBackend {
public:
    explicit StateParameterBackend(thl::State& state) : m_state(state) {}

    [[nodiscard]] std::optional<ParameterBinding> find(std::string_view key) const override;

    thl::State& state() { return m_state; }
    const thl::State& state() const { return m_state; }

private:
    thl::State& m_state;
};

}  // namespace thl::modulation
