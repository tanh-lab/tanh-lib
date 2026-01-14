#include <tanh/dsp/utils/Scales.h>

#include <string>
#include <vector>
#include <unordered_map>

namespace thl::dsp::utils {

std::string note_number_to_note_name(int note_number){
    std::string note_name;
    switch (note_number % 12) {
        case 0: note_name = "C"; break;
        case 1: note_name = "C#"; break;
        case 2: note_name = "D"; break;
        case 3: note_name = "D#"; break;
        case 4: note_name = "E"; break;
        case 5: note_name = "F"; break;
        case 6: note_name = "F#"; break;
        case 7: note_name = "G"; break;
        case 8: note_name = "G#"; break;
        case 9: note_name = "A"; break;
        case 10: note_name = "A#"; break;
        case 11: note_name = "B"; break;
    }
    return note_name;
}

std::unordered_map<ScaleMode, std::vector<int>> scale_halfsteps = {
    {ScaleMode::MINOR, {2, 5}},
    {ScaleMode::MAJOR, {3, 7}},
    {ScaleMode::PHRYGIAN, {1,5}}
};

int get_note_offset(int scale_note_index, ScaleMode mode){

    int note_offset = 2 * scale_note_index;
    std::vector<int>& halfsteps = scale_halfsteps[mode];

    while (scale_note_index >= 0){
        // subtract one halfstep for each halfstep that was passed by this scale note index
        for (int halfstep: halfsteps){
            if (scale_note_index < halfstep) break;
            note_offset--;
        }

        // For now assume that note 7 is the octave, loop afterwards
        scale_note_index -= 7; 
    }
    return note_offset; 
}

} // namespace thl::dsp
