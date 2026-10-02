#include "tanh/modulation/StateParameterBackend.h"

#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string_view>

#include "tanh/modulation/ModulationMatrix.h"
#include "tanh/modulation/ParameterBackend.h"
#include "tanh/state/Parameter.h"
#include "tanh/state/ParameterDefinitions.h"
#include "tanh/state/State.h"

namespace thl::modulation {

std::optional<ParameterBinding> StateParameterBackend::find(std::string_view key) const {
    ParameterRecord* record = m_state.get_record(key);
    if (record == nullptr) { return std::nullopt; }

    ParameterBinding binding{.m_def = record->m_def,
                             .m_base = std::monostate{},
                             .m_in_gesture = &record->m_in_gesture,
                             .m_state_record = record};
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

ModulationMatrix::ModulationMatrix(thl::State& state)
    : ModulationMatrix(
          std::make_unique<StateParameterBackend>(state),
          &state,
          [](thl::State& s) { return s.to_json(); },
          [](thl::State& s, const nlohmann::json& json) { s.from_json(json); },
          [](const thl::State& s, std::string_view key) {
              // Only called for a key State has no record for, so this throws
              // StateKeyNotFoundException / StateGroupNotFoundException.
              (void)s.get_handle<float>(key);
          }) {}

}  // namespace thl::modulation
