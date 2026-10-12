// SPDX-License-Identifier: Apache-2.0
//
// A plugin-shaped shared object embedding every built tanh-lib component — what a
// DAW plugin that uses tanh-lib looks like to the dynamic linker. test/exports checks
// its export table: the one entry point below and nothing else (no thl::, nothing of
// miniaudio, nlohmann::json, moodycamel or the Ableton Link SDK and its asio), in both
// the static and the shared shape.
//
// Each component is referenced by address so that its archive is pulled into the link
// in a static build; nothing is executed.
#include <tanh/core.h>

#ifdef TANH_STATE_ENABLED
#include <tanh/state/State.h>
#endif
#ifdef TANH_DSP_ENABLED
#include <tanh/dsp/rings-resonator/RingsString.h>
#include <tanh/dsp/utils/Scales.h>
#endif
#ifdef TANH_MODULATION_ENABLED
#include <tanh/modulation/ModulationMatrix.h>
#include <tanh/modulation/ParameterBackend.h>
#endif
#ifdef TANH_LINK_ENABLED
#include <tanh/link/LinkSession.h>
#endif
#ifdef TANH_AUDIO_IO_ENABLED
#include <tanh/audio-io/AudioDeviceManager.h>
#endif

#include <cstddef>
#include <optional>
#include <string_view>

#if defined(_WIN32)
#define TANH_EXPORTS_MODULE_API __declspec(dllexport)
#else
#define TANH_EXPORTS_MODULE_API __attribute__((visibility("default")))
#endif

namespace {

template <typename T>
void* address_of(T* p) {
    return reinterpret_cast<void*>(p);
}

#ifdef TANH_MODULATION_ENABLED
// A host's parameter store: Modulation without State is linked through this.
class NoParameters final : public thl::modulation::ParameterBackend {
public:
    [[nodiscard]] std::optional<thl::modulation::ParameterBinding> find(
        std::string_view /*key*/) const override {
        return std::nullopt;
    }
};
#endif

}  // namespace

extern "C" TANH_EXPORTS_MODULE_API std::size_t tanh_exports_entry() {
    static void* const references[] = {
        address_of(&thl::core::get_version),
#ifdef TANH_DSP_ENABLED
        address_of(&thl::dsp::utils::note_number_to_note_name),
#endif
#ifdef TANH_AUDIO_IO_ENABLED
        address_of(&thl::get_supported_bluetooth_profiles),
#endif
    };
#ifdef TANH_STATE_ENABLED
    // Constructors have no address: instantiate on demand behind a never-true branch
    // so that the state (and modulation) archives are linked, yet nothing runs.
    if (references[0] == nullptr) {
        thl::State state;
#ifdef TANH_MODULATION_ENABLED
        thl::modulation::ModulationMatrix matrix(state);
        (void)matrix;
#endif
    }
#endif
#ifdef TANH_MODULATION_ENABLED
    if (references[0] == nullptr) {
        NoParameters parameters;
        thl::modulation::ModulationMatrix matrix(parameters);
        (void)matrix;
    }
#endif
#ifdef TANH_LINK_ENABLED
    if (references[0] == nullptr) {
        thl::link::LinkSession session;
        (void)session;
    }
#endif
#ifdef TANH_DSP_ENABLED
    if (references[0] == nullptr) {
        thl::dsp::resonator::RingsString string;
        (void)string;
    }
#endif
    return sizeof(references) / sizeof(references[0]);
}
