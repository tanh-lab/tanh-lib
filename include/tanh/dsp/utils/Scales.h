#pragma once

#include <string>
#include <vector>

#include <tanh/core/Exports.h>

namespace thl::dsp::utils {

enum RootNote {
    C,
    Db,
    D,
    Eb,
    E,
    F,
    Gb,
    G,
    Ab,
    A,
    Bb,
    B,
};

enum ScaleMode { Major, Minor, Phrygian };

TANH_API std::string note_number_to_note_name(int note_number);

TANH_API int get_note_offset(int scale_note_index, ScaleMode mode);

}  // namespace thl::dsp::utils
