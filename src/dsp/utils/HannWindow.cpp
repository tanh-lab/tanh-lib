#include <tanh/dsp/utils/HannWindow.h>
#include <cmath>
#include <algorithm> // For std::clamp

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace thl::dsp::utils {

HannWindow::HannWindow()
    : m_state(State::IDLE), 
      m_sample_rate(44100.0f), // Default sample rate
      m_duration_samples(0.0f),
      m_current_sample(0.0f)
{
    // Default to a reasonable duration
    set_duration(100.0f); // 100ms default
}

void HannWindow::set_duration(float duration_ms) {
    // Convert from milliseconds to samples
    m_duration_samples = m_sample_rate * duration_ms / 1000.0f;
}

void HannWindow::set_sample_rate(float sample_rate) {
    m_sample_rate = sample_rate;
}

void HannWindow::start() {
    m_state = State::ACTIVE;
    m_current_sample = 0.0f;
}

void HannWindow::reset() {
    m_state = State::IDLE;
    m_current_sample = 0.0f;
}

float HannWindow::process() {
    if (m_state != State::ACTIVE || m_duration_samples <= 0.0f) {
        return 0.0f;
    }
    
    // Calculate normalized position in the window (0.0 - 1.0)
    float position = m_current_sample / m_duration_samples;
    
    // Check if we've reached the end
    if (position >= 1.0f) {
        m_state = State::IDLE;
        return 0.0f;
    }
    
    // Increment for next call
    m_current_sample += 1.0f;
    
    // Return the Hann window value at the current position
    return process_at_position(position);
}

float HannWindow::process_at_position(float position) {
    // Clamp position to valid range [0.0, 1.0)
    position = std::clamp(position, 0.0f, 0.9999f);
    
    // Hann window formula: 0.5 * (1 - cos(2π * n/N))
    return 0.5f * (1.0f - std::cos(2.0f * M_PI * position));
}

bool HannWindow::is_active() const {
    return m_state == State::ACTIVE;
}

HannWindow::State HannWindow::get_state() const {
    return m_state;
}

} // namespace thl::dsp::utils
