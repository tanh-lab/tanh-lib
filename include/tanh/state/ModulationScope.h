#pragma once

#include <cstdint>

namespace thl::modulation {

// Library-neutral scope handle for modulation. Identifies a polyphony domain
// (e.g. "synth_voice", "lfo_bank") registered at runtime on ModulationMatrix
// via register_scope(name, voice_count). Id 0 is library-reserved for Global.
//
// The `m_name` pointer references stable storage owned by the matrix's scope
// registry — copying/comparing handles is safe, and serialization can emit
// the name without a matrix lookup. Handles produced by different
// ModulationMatrix instances are NOT interchangeable: the id space is
// per-matrix, and the name pointer is only stable within the matrix that
// produced it. ensure_target_with_lock validates this at bind time.
//
// k_global_scope is pre-registered at matrix construction with the reserved
// name "global" (k_global_scope_name) and voice count 1. Hosts cannot register
// this name — ModulationMatrix::register_scope rejects "global" and hands back
// the global handle.
//
// Physical location note: this header lives under tanh/state/ because
// ParameterDefinition stores a ModulationScope and tanh_modulation depends on
// tanh_state (not the reverse). The namespace is `thl::modulation` because
// scope is a modulation concept semantically — the file path reflects module
// ownership for the dependency graph; the namespace reflects semantic domain.
inline constexpr const char* k_global_scope_name = "global";

struct ModulationScope {
    uint16_t m_id = 0;
    const char* m_name = k_global_scope_name;

    constexpr bool operator==(ModulationScope other) const { return m_id == other.m_id; }
    constexpr bool operator!=(ModulationScope other) const { return m_id != other.m_id; }
};

inline constexpr ModulationScope k_global_scope{.m_id = 0, .m_name = k_global_scope_name};

}  // namespace thl::modulation
