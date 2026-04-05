// Copyright 2014 Emilie Gillet.
//
// Author: Emilie Gillet (emilie.o.gillet@gmail.com)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// See http://creativecommons.org/licenses/MIT/ for more information.
//
// -----------------------------------------------------------------------------
//
// Reverb.

#pragma once

#include <tanh/dsp/audio/AudioBufferView.h>
#include <tanh/dsp/rings-resonator/RingsDsp.h>
#include <tanh/dsp/rings-resonator/fx/RingsFxEngine.h>

namespace thl::dsp::fx {

class RingsReverb {
public:
    RingsReverb() = default;
    ~RingsReverb() = default;

    void prepare(uint16_t* buffer, float sample_rate = thl::dsp::resonator::k_default_sample_rate) {
        m_engine.prepare(buffer);
        m_engine.set_lfo_frequency(Lfo1, 0.5f / sample_rate);
        m_engine.set_lfo_frequency(Lfo2, 0.3f / sample_rate);
        m_rate_ratio = sample_rate / thl::dsp::resonator::k_default_sample_rate;
        m_lp = 0.7f;
        m_diffusion = 0.625f;
    }

    static constexpr size_t k_reverb_buffer_size = 65536;

    void process(thl::dsp::audio::AudioBufferView stereo) {
        float* left = stereo.get_write_pointer(0);
        float* right = stereo.get_write_pointer(1);
        size_t size = stereo.get_num_frames();
        // This is the Griesinger topology described in the Dattorro paper
        // (4 AP diffusers on the input, then a loop of 2x 2AP+1Delay).
        // Modulation is applied in the loop of the first diffuser AP for
        // additional smearing; and to the two long delays for a slow
        // shimmer/chorus effect.
        //
        // Reserve sizes are doubled vs original 48kHz values to accommodate up
        // to 96kHz. Runtime tap offsets are scaled by m_rate_ratio so that the
        // actual delay times (in seconds) remain constant across sample rates.
        using Memory = E::Reserve<
            300,
            E::Reserve<
                428,
                E::Reserve<
                    638,
                    E::Reserve<
                        1054,
                        E::Reserve<
                            4364,
                            E::Reserve<
                                5380,
                                E::Reserve<
                                    9002,
                                    E::Reserve<5050,
                                               E::Reserve<4394, E::Reserve<12624> > > > > > > > > >;
        E::DelayLine<Memory, 0> ap1;
        E::DelayLine<Memory, 1> ap2;
        E::DelayLine<Memory, 2> ap3;
        E::DelayLine<Memory, 3> ap4;
        E::DelayLine<Memory, 4> dap1a;
        E::DelayLine<Memory, 5> dap1b;
        E::DelayLine<Memory, 6> del1;
        E::DelayLine<Memory, 7> dap2a;
        E::DelayLine<Memory, 8> dap2b;
        E::DelayLine<Memory, 9> del2;
        E::Context c;

        const float kap = m_diffusion;
        const float klp = m_lp;
        const float krt = m_reverb_time;
        const float amount = m_amount;
        const float gain = m_input_gain;
        const float r = m_rate_ratio;

        // Scaled allpass tap offsets.  Original TAIL reads at length-1; we
        // preserve those base offsets and scale by rate ratio.
        const auto ap1_tap = static_cast<int32_t>(149.0f * r);
        const auto ap2_tap = static_cast<int32_t>(213.0f * r);
        const auto ap3_tap = static_cast<int32_t>(318.0f * r);
        const auto ap4_tap = static_cast<int32_t>(526.0f * r);
        const auto dap1a_tap = static_cast<int32_t>(2181.0f * r);
        const auto dap1b_tap = static_cast<int32_t>(2689.0f * r);
        const auto dap2a_tap = static_cast<int32_t>(2524.0f * r);
        const auto dap2b_tap = static_cast<int32_t>(2196.0f * r);

        float lp_1 = m_lp_decay_1;
        float lp_2 = m_lp_decay_2;

        while (size--) {
            float wet;
            float apout = 0.0f;
            m_engine.start(&c);

            c.read(*left + *right, gain);

            // Diffuse through 4 allpasses (runtime-scaled tap offsets).
            c.read(ap1, ap1_tap, kap);
            c.write_all_pass(ap1, -kap);
            c.read(ap2, ap2_tap, kap);
            c.write_all_pass(ap2, -kap);
            c.read(ap3, ap3_tap, kap);
            c.write_all_pass(ap3, -kap);
            c.read(ap4, ap4_tap, kap);
            c.write_all_pass(ap4, -kap);
            c.write(apout);

            // Main reverb loop.
            c.load(apout);
            c.interpolate(del2, 6261.0f * r, Lfo2, 50.0f * r, krt);
            c.lp(lp_1, klp);
            c.read(dap1a, dap1a_tap, -kap);
            c.write_all_pass(dap1a, kap);
            c.read(dap1b, dap1b_tap, kap);
            c.write_all_pass(dap1b, -kap);
            c.write(del1, 2.0f);
            c.write(wet, 0.0f);

            *left += (wet - *left) * amount;

            c.load(apout);
            c.interpolate(del1, 4460.0f * r, Lfo1, 40.0f * r, krt);
            c.lp(lp_2, klp);
            c.read(dap2a, dap2a_tap, kap);
            c.write_all_pass(dap2a, -kap);
            c.read(dap2b, dap2b_tap, -kap);
            c.write_all_pass(dap2b, kap);
            c.write(del2, 2.0f);
            c.write(wet, 0.0f);

            *right += (wet - *right) * amount;

            ++left;
            ++right;
        }

        m_lp_decay_1 = lp_1;
        m_lp_decay_2 = lp_2;
    }

    inline void set_amount(float amount) { m_amount = amount; }

    inline void set_input_gain(float input_gain) { m_input_gain = input_gain; }

    inline void set_time(float reverb_time) { m_reverb_time = reverb_time; }

    inline void set_diffusion(float diffusion) { m_diffusion = diffusion; }

    inline void set_lp(float lp) { m_lp = lp; }

    inline void clear() { m_engine.clear(); }

    RingsReverb(const RingsReverb&) = delete;
    RingsReverb& operator=(const RingsReverb&) = delete;

private:
    using E = RingsFxEngine<65536, Format16Bit>;
    E m_engine;

    float m_amount = 0.0f;
    float m_input_gain = 0.0f;
    float m_reverb_time = 0.0f;
    float m_diffusion = 0.625f;
    float m_lp;
    float m_rate_ratio = 1.0f;

    float m_lp_decay_1 = 0.0f;
    float m_lp_decay_2 = 0.0f;
};

}  // namespace thl::dsp::fx
