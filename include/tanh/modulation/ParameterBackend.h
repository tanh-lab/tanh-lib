#pragma once

#include <tanh/state/ParameterDefinitions.h>

#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace thl::modulation {

/**
 * @brief Pointer to the atomic that holds a parameter's base (unmodulated) value.
 *
 * Exactly one alternative is set, matching the parameter's native storage type.
 * SmartHandle<T> requires the `std::atomic<T>*` alternative, so the
 * type a host stores a value in is the type it must request a handle for.
 * Hosts that keep every parameter as a float (e.g. JUCE's
 * AudioProcessorValueTreeState) bind int/bool/choice parameters as
 * `std::atomic<float>*` and request SmartHandle<float>. Modulated values
 * are then not snapped to the Range step (SmartHandle<int> does that), so such
 * hosts round/snap the loaded value themselves.
 *
 * The matrix and SmartHandle only read through the pointer (relaxed loads);
 * the host keeps writing base values through its own API.
 *
 * std::monostate means the parameter has no numeric base value (e.g. a
 * String parameter) and cannot be modulated.
 */
using ParameterBaseValue = std::variant<std::monostate,
                                        std::atomic<float>*,
                                        std::atomic<double>*,
                                        std::atomic<int>*,
                                        std::atomic<bool>*>;

/**
 * @brief Everything the modulation matrix needs to know about one parameter.
 *
 * Returned by ParameterBackend::find() and copied into the matrix's
 * ResolvedTarget the first time a key is resolved; it is never re-queried for
 * that key afterwards. The pointed-to atomics must therefore stay valid (same
 * address) for the lifetime of the ModulationMatrix.
 */
struct ParameterBinding {
    /// Immutable metadata: range (normalized depth, replace range, snapping),
    /// type, flags (ParameterDefinition::is_modulatable()), id, modulation
    /// scope, name and text formatters. Exposed via SmartHandle::def().
    ParameterDefinition m_def;

    /// Base value read by SmartHandle::load() on the audio thread
    /// (relaxed load).
    ParameterBaseValue m_base;

    /// Gesture flag read on the audio thread for routings with
    /// ModulationRouting::m_skip_during_gesture (relaxed load). nullptr means
    /// the parameter is never in a gesture.
    const std::atomic<bool>* m_in_gesture = nullptr;
};

/**
 * @brief Source of parameter values for a ModulationMatrix.
 *
 * Decouples the matrix from thl::State: a host implements find() on top of
 * its own parameter store. find() is only called from non-real-time matrix
 * methods (get_smart_handle(), add_routing(),
 * update_routing_replace_range_normalized()) under the matrix's writer
 * mutex, so it may allocate and lock.
 *
 * StateParameterBackend (tanh/modulation/StateParameterBackend.h) is the
 * implementation used by ModulationMatrix(thl::State&).
 */
class ParameterBackend {
public:
    ParameterBackend() = default;
    virtual ~ParameterBackend() = default;

    ParameterBackend(const ParameterBackend&) = delete;
    ParameterBackend& operator=(const ParameterBackend&) = delete;
    ParameterBackend(ParameterBackend&&) = delete;
    ParameterBackend& operator=(ParameterBackend&&) = delete;

    /// Look up a parameter by key. Returns std::nullopt if the key is unknown.
    [[nodiscard]] virtual std::optional<ParameterBinding> find(std::string_view key) const = 0;
};

}  // namespace thl::modulation
