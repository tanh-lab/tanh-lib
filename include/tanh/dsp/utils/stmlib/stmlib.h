#pragma once

#include <cstddef>
#include <cstdint>

// Temporary compatibility macros for vendored Mutable Instruments DSP sources.
#ifndef DISALLOW_COPY_AND_ASSIGN
#define DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&) = delete;      \
  TypeName& operator=(const TypeName&) = delete
#endif

#ifndef CLIP
#define CLIP(x) if ((x) < -32767) (x) = -32767; if ((x) > 32767) (x) = 32767;
#endif

#ifndef CONSTRAIN
#define CONSTRAIN(var, min, max) \
  if ((var) < (min)) {           \
    (var) = (min);               \
  } else if ((var) > (max)) {    \
    (var) = (max);               \
  }
#endif

#ifndef JOIN
#define JOIN(lhs, rhs) JOIN_1(lhs, rhs)
#define JOIN_1(lhs, rhs) JOIN_2(lhs, rhs)
#define JOIN_2(lhs, rhs) lhs##rhs
#endif

#ifndef STATIC_ASSERT
#define STATIC_ASSERT(expression, message) static_assert((expression), #message)
#endif

#ifndef IN_RAM
#define IN_RAM
#endif

#ifndef UNROLL2
#define UNROLL2(x) x; x;
#define UNROLL4(x) x; x; x; x;
#define UNROLL8(x) x; x; x; x; x; x; x; x;
#endif

namespace stmlib {

union Word {
  uint16_t value;
  uint8_t bytes[2];
};

union LongWord {
  uint32_t value;
  uint16_t words[2];
  uint8_t bytes[4];
};

template<uint32_t a, uint32_t b, uint32_t c, uint32_t d>
struct FourCC {
  static constexpr uint32_t value = (((((d << 8) | c) << 8) | b) << 8) | a;
};

}  // namespace stmlib

