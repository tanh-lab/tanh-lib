#include <tanh/dsp/utils/Random.h>

namespace thl::dsp::utils {

uint32_t Random::m_rng_state = 0x21u;

uint32_t Random::nlog2_16(uint16_t x) {
    uint32_t r = 0;
    uint32_t t;
    uint32_t a = x;

    t = a * 256u;
    if (t < 0x10000u) {
        a = t;
        r += 0x80000u;
    }
    t = a * 16u;
    if (t < 0x10000u) {
        a = t;
        r += 0x40000u;
    }
    t = a * 4u;
    if (t < 0x10000u) {
        a = t;
        r += 0x20000u;
    }
    t = a * 2u;
    if (t < 0x10000u) {
        a = t;
        r += 0x10000u;
    }
    t = (a * 3u) >> 1u;
    if (t < 0x10000u) {
        a = t;
        r += 0x095c0u;
    }
    t = (a * 5u) >> 2u;
    if (t < 0x10000u) {
        a = t;
        r += 0x0526au;
    }
    t = (a * 9u) >> 3u;
    if (t < 0x10000u) {
        a = t;
        r += 0x02b80u;
    }
    t = (a * 17u) >> 4u;
    if (t < 0x10000u) {
        a = t;
        r += 0x01664u;
    }
    t = (a * 33u) >> 5u;
    if (t < 0x10000u) {
        a = t;
        r += 0x00b5du;
    }
    t = (a * 65u) >> 6u;
    if (t < 0x10000u) {
        a = t;
        r += 0x005bau;
    }
    t = (a * 129u) >> 7u;
    if (t < 0x10000u) {
        a = t;
        r += 0x002e0u;
    }
    t = (a * 257u) >> 8u;
    if (t < 0x10000u) {
        a = t;
        r += 0x00171u;
    }
    t = (a * 513u) >> 9u;
    if (t < 0x10000u) {
        a = t;
        r += 0x000b8u;
    }
    t = (a * 1025u) >> 10u;
    if (t < 0x10000u) {
        a = t;
        r += 0x0005cu;
    }
    t = (a * 2049u) >> 11u;
    if (t < 0x10000u) {
        a = t;
        r += 0x0002eu;
    }
    t = (a * 4097u) >> 12u;
    if (t < 0x10000u) {
        a = t;
        r += 0x00017u;
    }
    t = (a * 8193u) >> 13u;
    if (t < 0x10000u) {
        a = t;
        r += 0x0000cu;
    }
    t = (a * 16385u) >> 14u;
    if (t < 0x10000u) {
        a = t;
        r += 0x00006u;
    }
    t = (a * 32769u) >> 15u;
    if (t < 0x10000u) {
        a = t;
        r += 0x00003u;
    }
    if (((a * 65537u) >> 16u) < 0x10000u) { r += 0x00001u; }
    if (r == 0) { r++; }
    return r;
}

}  // namespace thl::dsp::utils
