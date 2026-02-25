#include <tanh/dsp/utils/Random.h>

namespace thl::dsp::utils {

uint32_t Random::rng_state_ = 0x21u;

uint32_t Random::nlog2_16(uint16_t x) {
    uint32_t r = 0;
    uint32_t t;
    uint32_t a = x;

    if ((t = (a * 256u)) < 0x10000u) { a = t; r += 0x80000u; }
    if ((t = (a * 16u)) < 0x10000u) { a = t; r += 0x40000u; }
    if ((t = (a * 4u)) < 0x10000u) { a = t; r += 0x20000u; }
    if ((t = (a * 2u)) < 0x10000u) { a = t; r += 0x10000u; }
    if ((t = (a * 3u) >> 1u) < 0x10000u) { a = t; r += 0x095c0u; }
    if ((t = (a * 5u) >> 2u) < 0x10000u) { a = t; r += 0x0526au; }
    if ((t = (a * 9u) >> 3u) < 0x10000u) { a = t; r += 0x02b80u; }
    if ((t = (a * 17u) >> 4u) < 0x10000u) { a = t; r += 0x01664u; }
    if ((t = (a * 33u) >> 5u) < 0x10000u) { a = t; r += 0x00b5du; }
    if ((t = (a * 65u) >> 6u) < 0x10000u) { a = t; r += 0x005bau; }
    if ((t = (a * 129u) >> 7u) < 0x10000u) { a = t; r += 0x002e0u; }
    if ((t = (a * 257u) >> 8u) < 0x10000u) { a = t; r += 0x00171u; }
    if ((t = (a * 513u) >> 9u) < 0x10000u) { a = t; r += 0x000b8u; }
    if ((t = (a * 1025u) >> 10u) < 0x10000u) { a = t; r += 0x0005cu; }
    if ((t = (a * 2049u) >> 11u) < 0x10000u) { a = t; r += 0x0002eu; }
    if ((t = (a * 4097u) >> 12u) < 0x10000u) { a = t; r += 0x00017u; }
    if ((t = (a * 8193u) >> 13u) < 0x10000u) { a = t; r += 0x0000cu; }
    if ((t = (a * 16385u) >> 14u) < 0x10000u) { a = t; r += 0x00006u; }
    if ((t = (a * 32769u) >> 15u) < 0x10000u) { a = t; r += 0x00003u; }
    if ((t = (a * 65537u) >> 16u) < 0x10000u) { a = t; r += 0x00001u; }
    if (r == 0) r++;
    return r;
}

} // namespace thl::dsp::utils

