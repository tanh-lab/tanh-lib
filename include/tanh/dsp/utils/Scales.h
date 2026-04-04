#pragma once

#include <string>
#include <vector>

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

std::string note_number_to_note_name(int note_number);

int get_note_offset(int scale_note_index, ScaleMode mode);

}  // namespace thl::dsp::utils
