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
// Base class for building reverb.

#pragma once

#include <algorithm>

#include <tanh/dsp/utils/DspMath.h>
#include <tanh/dsp/utils/CosineOscillator.h>

namespace thl::dsp::fx {

#define TAIL , -1

enum Format { Format12Bit, Format16Bit, Format32Bit };

enum LFOIndex { Lfo1, Lfo2 };

template <Format format>
struct DataType {};

template <>
struct DataType<Format12Bit> {
    typedef uint16_t T;

    static inline float decompress(T value) {
        return static_cast<float>(static_cast<int16_t>(value)) / 4096.0f;
    }

    static inline T compress(float value) {
        return static_cast<uint16_t>(
            thl::dsp::utils::clip16(static_cast<int32_t>(value * 4096.0f)));
    }
};

template <>
struct DataType<Format16Bit> {
    typedef uint16_t T;

    static inline float decompress(T value) {
        return static_cast<float>(static_cast<int16_t>(value)) / 32768.0f;
    }

    static inline T compress(float value) {
        return static_cast<uint16_t>(
            thl::dsp::utils::clip16(static_cast<int32_t>(value * 32768.0f)));
    }
};

template <>
struct DataType<Format32Bit> {
    typedef float T;

    static inline float decompress(T value) {
        return value;
        ;
    }

    static inline T compress(float value) { return value; }
};

template <size_t size, Format format = Format12Bit>
class RingsFxEngine {
public:
    typedef typename DataType<format>::T T;
    RingsFxEngine() {}
    ~RingsFxEngine() {}

    void prepare(T* buffer) {
        m_buffer = buffer;
        clear();
    }

    void clear() {
        std::fill(&m_buffer[0], &m_buffer[size], 0);
        m_write_ptr = 0;
    }

    struct Empty {};

    template <int32_t l, typename T = Empty>
    struct Reserve {
        typedef T Tail;
        enum { Length = l };
    };

    template <typename Memory, int32_t index>
    struct DelayLine {
        enum {
            Length = DelayLine<typename Memory::Tail, index - 1>::Length,
            Base = DelayLine<Memory, index - 1>::Base + DelayLine<Memory, index - 1>::Length + 1
        };
    };

    template <typename Memory>
    struct DelayLine<Memory, 0> {
        enum { Length = Memory::Length, Base = 0 };
    };

    class Context {
        friend class RingsFxEngine;

    public:
        Context() {}
        ~Context() {}

        inline void load(float value) { m_accumulator = value; }

        inline void read(float value, float scale) { m_accumulator += value * scale; }

        inline void read(float value) { m_accumulator += value; }

        inline void write(float& value) { value = m_accumulator; }

        inline void write(float& value, float scale) {
            value = m_accumulator;
            m_accumulator *= scale;
        }

        template <typename D>
        inline void write(D& d, int32_t offset, float scale) {
            static_assert((D::Base + D::Length <= size), "delay_memory_full");
            T w = DataType<format>::compress(m_accumulator);
            if (offset == -1) {
                m_buffer[(m_write_ptr + D::Base + D::Length - 1) & Mask] = w;
            } else {
                m_buffer[(m_write_ptr + D::Base + offset) & Mask] = w;
            }
            m_accumulator *= scale;
        }

        template <typename D>
        inline void write(D& d, float scale) {
            write(d, 0, scale);
        }

        template <typename D>
        inline void write_all_pass(D& d, int32_t offset, float scale) {
            write(d, offset, scale);
            m_accumulator += m_previous_read;
        }

        template <typename D>
        inline void write_all_pass(D& d, float scale) {
            write_all_pass(d, 0, scale);
        }

        template <typename D>
        inline void read(D& d, int32_t offset, float scale) {
            static_assert((D::Base + D::Length <= size), "delay_memory_full");
            T r;
            if (offset == -1) {
                r = m_buffer[(m_write_ptr + D::Base + D::Length - 1) & Mask];
            } else {
                r = m_buffer[(m_write_ptr + D::Base + offset) & Mask];
            }
            float r_f = DataType<format>::decompress(r);
            m_previous_read = r_f;
            m_accumulator += r_f * scale;
        }

        template <typename D>
        inline void read(D& d, float scale) {
            read(d, 0, scale);
        }

        inline void lp(float& state, float coefficient) {
            state += coefficient * (m_accumulator - state);
            m_accumulator = state;
        }

        inline void hp(float& state, float coefficient) {
            state += coefficient * (m_accumulator - state);
            m_accumulator -= state;
        }

        template <typename D>
        inline void interpolate(D& d, float offset, float scale) {
            static_assert((D::Base + D::Length <= size), "delay_memory_full");
            auto [offset_integral, offset_fractional] =
                thl::dsp::utils::split_integral_fractional(offset);
            float a = DataType<format>::decompress(
                m_buffer[(m_write_ptr + offset_integral + D::Base) & Mask]);
            float b = DataType<format>::decompress(
                m_buffer[(m_write_ptr + offset_integral + D::Base + 1) & Mask]);
            float x = a + (b - a) * offset_fractional;
            m_previous_read = x;
            m_accumulator += x * scale;
        }

        template <typename D>
        inline void interpolate(D& d, float offset, LFOIndex index, float amplitude, float scale) {
            static_assert((D::Base + D::Length <= size), "delay_memory_full");
            offset += amplitude * m_lfo_value[index];
            auto [offset_integral, offset_fractional] =
                thl::dsp::utils::split_integral_fractional(offset);
            float a = DataType<format>::decompress(
                m_buffer[(m_write_ptr + offset_integral + D::Base) & Mask]);
            float b = DataType<format>::decompress(
                m_buffer[(m_write_ptr + offset_integral + D::Base + 1) & Mask]);
            float x = a + (b - a) * offset_fractional;
            m_previous_read = x;
            m_accumulator += x * scale;
        }

    private:
        float m_accumulator = 0.0f;
        float m_previous_read = 0.0f;
        float m_lfo_value[2] = {};
        T* m_buffer = nullptr;
        int32_t m_write_ptr = 0;

        Context(const Context&) = delete;
        Context& operator=(const Context&) = delete;
    };

    inline void set_lfo_frequency(LFOIndex index, float frequency) {
        m_lfo[index].template prepare<thl::dsp::utils::CosineOscillatorMode::Approximate>(
            frequency * 32.0f);
    }

    inline void start(Context* c) {
        --m_write_ptr;
        if (m_write_ptr < 0) { m_write_ptr += size; }
        c->m_accumulator = 0.0f;
        c->m_previous_read = 0.0f;
        c->m_buffer = m_buffer;
        c->m_write_ptr = m_write_ptr;
        if ((m_write_ptr & 31) == 0) {
            c->m_lfo_value[0] = m_lfo[0].next();
            c->m_lfo_value[1] = m_lfo[1].next();
        } else {
            c->m_lfo_value[0] = m_lfo[0].value();
            c->m_lfo_value[1] = m_lfo[1].value();
        }
    }

private:
    enum { Mask = size - 1 };

    int32_t m_write_ptr = 0;
    T* m_buffer = nullptr;
    thl::dsp::utils::CosineOscillator m_lfo[2];

    RingsFxEngine(const RingsFxEngine&) = delete;
    RingsFxEngine& operator=(const RingsFxEngine&) = delete;
};

}  // namespace thl::dsp::fx
