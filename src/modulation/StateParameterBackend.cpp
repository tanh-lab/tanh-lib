#include "tanh/modulation/StateParameterBackend.h"

#include <optional>
#include <string_view>
#include <variant>

#include "tanh/core/parameters/ParameterDefinitions.h"
#include "tanh/modulation/ParameterBackend.h"
#include "tanh/state/Exceptions.h"
#include "tanh/state/Parameter.h"
#include "tanh/state/State.h"

namespace thl::modulation {

std::optional<ParameterBinding> StateParameterBackend::find(std::string_view key) const {
    ParameterRecord* record = m_state.find_record(key);
    if (record == nullptr) { return std::nullopt; }

    ParameterBinding binding{.m_def = record->m_def,
                             .m_base = std::monostate{},
                             .m_in_gesture = &record->m_in_gesture};
    auto& cache = record->m_cache;
    switch (record->m_def.m_type) {
        case ParameterType::Float: binding.m_base = &cache.m_atomic_float; break;
        case ParameterType::Double: binding.m_base = &cache.m_atomic_double; break;
        case ParameterType::Int: binding.m_base = &cache.m_atomic_int; break;
        case ParameterType::Bool: binding.m_base = &cache.m_atomic_bool; break;
        case ParameterType::String: break;
    }
    return binding;
}

void StateParameterBackend::throw_not_found(std::string_view key) const {
    // State's own lookup names a missing group; it only returns if the key exists after all.
    (void)m_state.get_parameter(key);
    throw thl::StateKeyNotFoundException(key);
}

}  // namespace thl::modulation
