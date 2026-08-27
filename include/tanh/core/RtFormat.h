// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tanh/utils/RealtimeSanitizer.h>

#include <array>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>

/// @file RtFormat.h
/// @brief A small, allocation-free printf subset for real-time contexts.
///
/// std::snprintf is not guaranteed real-time safe: some C libraries take
/// locale locks or allocate for floating-point conversions. This formatter
/// touches nothing but the caller's buffer and is deterministic in time
/// relative to the output length.
///
/// Supported: flags `-`, `+`, ` `, `0`; width and precision (literal or `*`);
/// length modifiers `hh h l ll z j t` (parsed, value read at the matching
/// width); conversions `d i u x X o c s p %` and `f F e E g G`.
/// Not supported (emitted verbatim): `%n`, `%a`, wide strings.
///
/// Floating-point output matches the C library (round-to-nearest, ties to
/// even on the exact binary value) for up to 17 fractional digits and
/// magnitudes below 1e18 in fixed notation; beyond that the last digit may
/// differ, or fixed notation falls back to exponent notation.
///
/// Semantics match snprintf: the output is always NUL-terminated when
/// `capacity > 0`, and the return value is the length the full output would
/// have had, so `result >= capacity` signals truncation.
namespace thl::core {

int rt_vsnprintf(char* out,
                 size_t capacity,
                 const char* fmt,
                 va_list args) noexcept TANH_NONBLOCKING_FUNCTION;

// NOLINTNEXTLINE(modernize-avoid-variadic-functions,cert-dcl50-cpp) printf-compatible by design
int rt_snprintf(char* out,
                size_t capacity,
                const char* fmt,
                ...) noexcept TANH_NONBLOCKING_FUNCTION;

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

namespace rt_format_detail {

constexpr size_t k_digits_cap = 24;
using DigitBuf = std::array<char, k_digits_cap>;

struct Sink {
    char* m_out = nullptr;
    size_t m_cap = 0;
    size_t m_len = 0;  // characters the full output would have

    void put(char c) noexcept TANH_NONBLOCKING_FUNCTION {
        if (m_len + 1 < m_cap) { m_out[m_len] = c; }
        ++m_len;
    }

    void finish() noexcept TANH_NONBLOCKING_FUNCTION {
        if (m_cap == 0) { return; }
        m_out[m_len < m_cap ? m_len : m_cap - 1] = '\0';
    }
};

struct Spec {
    bool m_left = false;
    bool m_plus = false;
    bool m_space = false;
    bool m_zero = false;
    int m_width = 0;
    int m_precision = -1;
};

inline char sign_char(bool negative, const Spec& spec) noexcept TANH_NONBLOCKING_FUNCTION {
    if (negative) { return '-'; }
    if (spec.m_plus) { return '+'; }
    if (spec.m_space) { return ' '; }
    return '\0';
}

inline size_t str_len(const char* s, size_t max) noexcept TANH_NONBLOCKING_FUNCTION {
    size_t n = 0;
    while (n < max && s[n] != '\0') { ++n; }
    return n;
}

// Emit `body` (already converted, without sign) with sign/padding rules.
// `zero_pad_ok` is false for %s/%c where '0' is ignored.
inline void emit_padded(Sink& sink,
                        const char* body,
                        size_t body_len,
                        char sign,
                        const Spec& spec,
                        bool zero_pad_ok) noexcept TANH_NONBLOCKING_FUNCTION {
    const size_t sign_len = sign != '\0' ? 1 : 0;
    const size_t total = body_len + sign_len;
    const size_t width = spec.m_width > 0 ? static_cast<size_t>(spec.m_width) : 0;
    const size_t pad = width > total ? width - total : 0;

    if (spec.m_left) {
        if (sign != '\0') { sink.put(sign); }
        for (size_t i = 0; i < body_len; ++i) { sink.put(body[i]); }
        for (size_t i = 0; i < pad; ++i) { sink.put(' '); }
        return;
    }
    if (spec.m_zero && zero_pad_ok) {
        if (sign != '\0') { sink.put(sign); }
        for (size_t i = 0; i < pad; ++i) { sink.put('0'); }
        for (size_t i = 0; i < body_len; ++i) { sink.put(body[i]); }
        return;
    }
    for (size_t i = 0; i < pad; ++i) { sink.put(' '); }
    if (sign != '\0') { sink.put(sign); }
    for (size_t i = 0; i < body_len; ++i) { sink.put(body[i]); }
}

// Writes the digits of `v` in `base` into the END of buf, returns start index.
inline size_t to_digits(uint64_t v,
                        unsigned base,
                        bool upper,
                        DigitBuf& buf) noexcept TANH_NONBLOCKING_FUNCTION {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t pos = buf.size();
    do {
        buf[--pos] = digits[v % base];
        v /= base;
    } while (v != 0 && pos > 0);
    return pos;
}

inline void emit_integer(Sink& sink,
                         uint64_t magnitude,
                         bool negative,
                         unsigned base,
                         bool upper,
                         const Spec& spec) noexcept TANH_NONBLOCKING_FUNCTION {
    DigitBuf digits{};
    const size_t start = to_digits(magnitude, base, upper, digits);
    const size_t len = digits.size() - start;
    const char sign = sign_char(negative, spec);

    if (spec.m_precision < 0) {
        emit_padded(sink, digits.data() + start, len, sign, spec, true);
        return;
    }

    // Precision on integers = minimum digit count (and disables '0' flag).
    DigitBuf padded{};
    const auto want = static_cast<size_t>(spec.m_precision);
    size_t out_len = len > want ? len : want;
    if (out_len > padded.size()) { out_len = padded.size(); }
    const size_t lead = out_len - len;
    for (size_t i = 0; i < lead; ++i) { padded[i] = '0'; }
    for (size_t i = 0; i < len; ++i) { padded[lead + i] = digits[start + i]; }
    if (spec.m_precision == 0 && magnitude == 0) { out_len = 0; }  // "%.0d" of 0 prints nothing
    Spec s = spec;
    s.m_zero = false;
    emit_padded(sink, padded.data(), out_len, sign, s, false);
}

// --- floating point ---------------------------------------------------------

constexpr int k_max_exact_pow10 = 22;  // 1e22 is the largest exactly representable power of ten

inline double pow10_exact(int k) noexcept TANH_NONBLOCKING_FUNCTION {
    constexpr std::array<double, k_max_exact_pow10 + 1> k_table = {
        1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
        1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    return k_table[static_cast<size_t>(k)];
}

inline uint64_t pow10_u64(int k) noexcept TANH_NONBLOCKING_FUNCTION {
    uint64_t r = 1;
    for (int i = 0; i < k; ++i) { r *= 10U; }
    return r;
}

/// round(v * 10^k) for v >= 0 with the C library's rounding rule: nearest,
/// ties decided on the exact product (an fma recovers the rounding error of
/// the scaling), true ties to even. Exact while |k| <= 22 and the result
/// fits in the double's integer range; otherwise best effort.
inline uint64_t scaled_round(double v, int k) noexcept TANH_NONBLOCKING_FUNCTION {
    double scaled = 0.0;
    double err = 0.0;
    if (k >= 0 && k <= k_max_exact_pow10) {
        const double p = pow10_exact(k);
        scaled = v * p;
        err = std::fma(v, p, -scaled);
    } else if (k < 0 && -k <= k_max_exact_pow10) {
        const double p = pow10_exact(-k);
        scaled = v / p;
        err = std::fma(-scaled, p, v);
    } else {
        scaled = v;
        int kk = k;
        while (kk > 0) {
            scaled *= 10.0;
            --kk;
        }
        while (kk < 0) {
            scaled /= 10.0;
            ++kk;
        }
    }

    const auto floor_u = static_cast<uint64_t>(scaled);
    const double frac = scaled - static_cast<double>(floor_u);
    bool up = false;
    if (frac > 0.5) {
        up = true;
    } else if (frac == 0.5) {
        if (err > 0.0) {
            up = true;
        } else if (err == 0.0) {
            up = (floor_u & 1U) != 0U;
        }
    }
    return floor_u + (up ? 1U : 0U);
}

/// Fixed notation of a finite v >= 0 into buf. Returns the length written.
inline size_t fixed_to_chars(double v,
                             int precision,
                             char* buf,
                             size_t cap) noexcept TANH_NONBLOCKING_FUNCTION {
    if (precision > 17) { precision = 17; }

    // Split into integer and fractional part; both steps are exact.
    auto int_part = static_cast<uint64_t>(v);
    const double frac = v - static_cast<double>(int_part);

    uint64_t frac_digits = scaled_round(frac, precision);
    const uint64_t frac_limit = pow10_u64(precision);
    if (frac_digits >= frac_limit) {  // rounded up to the next integer
        frac_digits -= frac_limit;
        ++int_part;
    }

    DigitBuf digits{};
    const size_t start = to_digits(int_part, 10, false, digits);
    size_t len = 0;
    for (size_t i = start; i < digits.size() && len < cap; ++i) { buf[len++] = digits[i]; }
    if (precision > 0 && len < cap) {
        buf[len++] = '.';
        DigitBuf fd{};
        const size_t fstart = to_digits(frac_digits, 10, false, fd);
        const size_t flen = fd.size() - fstart;
        const auto want = static_cast<size_t>(precision);
        for (size_t i = flen; i < want && len < cap; ++i) { buf[len++] = '0'; }
        for (size_t i = fstart; i < fd.size() && len < cap; ++i) { buf[len++] = fd[i]; }
    }
    return len;
}

/// Decimal exponent of v > 0 (floor(log10 v)), by repeated scaling.
inline int decimal_exponent(double v) noexcept TANH_NONBLOCKING_FUNCTION {
    int e = 0;
    while (v >= 10.0) {
        v /= 10.0;
        ++e;
    }
    while (v < 1.0) {
        v *= 10.0;
        --e;
    }
    return e;
}

struct ExpDigits {
    uint64_t m_mantissa = 0;  // precision + 1 significant digits
    int m_exponent = 0;
};

/// Mantissa digits and exponent for %e with `precision` fractional digits.
inline ExpDigits exp_digits(double v, int precision) noexcept TANH_NONBLOCKING_FUNCTION {
    if (precision > 17) { precision = 17; }
    ExpDigits r;
    if (v == 0.0) { return r; }
    r.m_exponent = decimal_exponent(v);
    r.m_mantissa = scaled_round(v, precision - r.m_exponent);
    const uint64_t upper = pow10_u64(precision + 1);
    if (r.m_mantissa >= upper) {  // rounding carried into a new digit
        ++r.m_exponent;
        r.m_mantissa = scaled_round(v, precision - r.m_exponent);
    } else if (r.m_mantissa < upper / 10U) {  // decimal_exponent overshot
        --r.m_exponent;
        r.m_mantissa = scaled_round(v, precision - r.m_exponent);
    }
    return r;
}

inline size_t exp_to_chars(const ExpDigits& d,
                           int precision,
                           bool upper,
                           char* buf,
                           size_t cap) noexcept TANH_NONBLOCKING_FUNCTION {
    if (precision > 17) { precision = 17; }
    DigitBuf md{};
    const size_t mstart = to_digits(d.m_mantissa, 10, false, md);
    const size_t mlen = md.size() - mstart;
    const auto want = static_cast<size_t>(precision) + 1;

    size_t len = 0;
    // Leading zeros if the mantissa has fewer digits than requested (only 0.0).
    size_t emitted = 0;
    for (size_t i = mlen; i < want; ++i) {
        if (len < cap) { buf[len++] = '0'; }
        if (emitted == 0 && precision > 0 && len < cap) { buf[len++] = '.'; }
        ++emitted;
    }
    for (size_t i = mstart; i < md.size(); ++i) {
        if (len < cap) { buf[len++] = md[i]; }
        if (emitted == 0 && precision > 0 && len < cap) { buf[len++] = '.'; }
        ++emitted;
    }

    if (len < cap) { buf[len++] = upper ? 'E' : 'e'; }
    if (len < cap) { buf[len++] = d.m_exponent < 0 ? '-' : '+'; }
    const auto mag = static_cast<uint64_t>(d.m_exponent < 0 ? -d.m_exponent : d.m_exponent);
    DigitBuf ed{};
    const size_t es = to_digits(mag, 10, false, ed);
    if (ed.size() - es < 2 && len < cap) { buf[len++] = '0'; }
    for (size_t i = es; i < ed.size() && len < cap; ++i) { buf[len++] = ed[i]; }
    return len;
}

/// %g: strip trailing zeros of the mantissa (and a dangling '.').
inline size_t strip_trailing_zeros(char* buf, size_t len) noexcept TANH_NONBLOCKING_FUNCTION {
    size_t exp_start = len;
    for (size_t i = 0; i < len; ++i) {
        if (buf[i] == 'e' || buf[i] == 'E') {
            exp_start = i;
            break;
        }
    }
    bool has_dot = false;
    for (size_t i = 0; i < exp_start; ++i) { has_dot = has_dot || buf[i] == '.'; }
    if (!has_dot) { return len; }

    size_t new_end = exp_start;
    while (new_end > 0 && buf[new_end - 1] == '0') { --new_end; }
    if (new_end > 0 && buf[new_end - 1] == '.') { --new_end; }
    const size_t tail = len - exp_start;
    for (size_t i = 0; i < tail; ++i) { buf[new_end + i] = buf[exp_start + i]; }
    return new_end + tail;
}

inline void emit_double(Sink& sink,
                        double v,
                        char conv,
                        const Spec& spec) noexcept TANH_NONBLOCKING_FUNCTION {
    const bool upper = (conv == 'F' || conv == 'E' || conv == 'G');
    const bool negative = std::signbit(v);
    if (negative) { v = -v; }
    const char sign = sign_char(negative, spec);

    if (std::isnan(v) || std::isinf(v)) {
        Spec s = spec;
        s.m_zero = false;
        const char* text = std::isnan(v) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        emit_padded(sink, text, 3, sign, s, false);
        return;
    }

    int precision = spec.m_precision >= 0 ? spec.m_precision : 6;
    std::array<char, 64> body{};
    size_t len = 0;

    if (conv == 'g' || conv == 'G') {
        if (precision == 0) { precision = 1; }
        // Exponent as %e with P-1 digits would print it (rounding included).
        const ExpDigits d = exp_digits(v, precision - 1);
        const int x = d.m_exponent;
        if (x < -4 || x >= precision) {
            len = exp_to_chars(d, precision - 1, upper, body.data(), body.size());
        } else {
            len = fixed_to_chars(v, precision - 1 - x, body.data(), body.size());
        }
        len = strip_trailing_zeros(body.data(), len);
    } else if (conv == 'e' || conv == 'E' || v >= 1e18) {
        // (%f of a magnitude beyond the exact integer range falls back to %e.)
        len = exp_to_chars(exp_digits(v, precision), precision, upper, body.data(), body.size());
    } else {
        len = fixed_to_chars(v, precision, body.data(), body.size());
    }

    emit_padded(sink, body.data(), len, sign, spec, true);
}

}  // namespace rt_format_detail

inline int rt_vsnprintf(char* out,
                        size_t capacity,
                        const char* fmt,
                        va_list args) noexcept TANH_NONBLOCKING_FUNCTION {
    using namespace rt_format_detail;
    Sink sink{.m_out = out, .m_cap = capacity};
    if (fmt == nullptr) {
        sink.finish();
        return 0;
    }

    // Argument fetchers capture `args` directly: va_list is an array type on
    // some ABIs (x86-64) and a struct on others (AArch64), so it cannot be
    // passed to a helper by reference portably. Length modifier: 0 = int,
    // 1 = long, 2 = long long, 3 = size_t/ptrdiff/intmax, -1 = short, -2 = char.
    const auto fetch_signed = [&args](int length) noexcept -> int64_t {
        switch (length) {
            case 2: return va_arg(args, long long);
            case 1: return va_arg(args, long);
            case 3: return static_cast<int64_t>(va_arg(args, ptrdiff_t));
            case -1: return static_cast<short>(va_arg(args, int));
            case -2:
                return static_cast<signed char>(
                    va_arg(args, int));  // NOLINT(bugprone-signed-char-misuse)
            default: return va_arg(args, int);
        }
    };
    const auto fetch_unsigned = [&args](int length) noexcept -> uint64_t {
        switch (length) {
            case 2: return va_arg(args, unsigned long long);
            case 1: return va_arg(args, unsigned long);
            case 3: return static_cast<uint64_t>(va_arg(args, size_t));
            case -1: return static_cast<unsigned short>(va_arg(args, unsigned));
            case -2: return static_cast<unsigned char>(va_arg(args, unsigned));
            default: return va_arg(args, unsigned);
        }
    };

    for (const char* p = fmt; *p != '\0'; ++p) {
        if (*p != '%') {
            sink.put(*p);
            continue;
        }
        const char* spec_start = p;
        ++p;
        if (*p == '\0') {
            sink.put('%');
            break;
        }

        Spec spec;
        for (;; ++p) {
            if (*p == '-') {
                spec.m_left = true;
            } else if (*p == '+') {
                spec.m_plus = true;
            } else if (*p == ' ') {
                spec.m_space = true;
            } else if (*p == '0') {
                spec.m_zero = true;
            } else if (*p != '#') {  // '#' (alt form) is accepted and ignored
                break;
            }
        }
        if (*p == '*') {
            spec.m_width = va_arg(args, int);
            if (spec.m_width < 0) {
                spec.m_left = true;
                spec.m_width = -spec.m_width;
            }
            ++p;
        } else {
            while (*p >= '0' && *p <= '9') {
                spec.m_width = spec.m_width * 10 + (*p - '0');
                ++p;
            }
        }
        if (*p == '.') {
            ++p;
            spec.m_precision = 0;
            if (*p == '*') {
                spec.m_precision = va_arg(args, int);
                if (spec.m_precision < 0) { spec.m_precision = -1; }
                ++p;
            } else {
                while (*p >= '0' && *p <= '9') {
                    spec.m_precision = spec.m_precision * 10 + (*p - '0');
                    ++p;
                }
            }
        }

        int length = 0;
        if (*p == 'h') {
            ++p;
            length = -1;
            if (*p == 'h') {
                ++p;
                length = -2;
            }
        } else if (*p == 'l') {
            ++p;
            length = 1;
            if (*p == 'l') {
                ++p;
                length = 2;
            }
        } else if (*p == 'z' || *p == 'j' || *p == 't') {
            ++p;
            length = 3;
        }

        const char conv = *p;
        switch (conv) {
            case 'd':
            case 'i': {
                const int64_t v = fetch_signed(length);
                const bool neg = v < 0;
                const uint64_t mag =
                    neg ? (~static_cast<uint64_t>(v) + 1U) : static_cast<uint64_t>(v);
                emit_integer(sink, mag, neg, 10, false, spec);
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o': {
                const uint64_t v = fetch_unsigned(length);
                const unsigned base = conv == 'u' ? 10U : (conv == 'o' ? 8U : 16U);
                Spec s = spec;
                s.m_plus = false;
                s.m_space = false;
                emit_integer(sink, v, false, base, conv == 'X', s);
                break;
            }
            case 'p': {
                const auto v = reinterpret_cast<uintptr_t>(va_arg(args, void*));
                DigitBuf digits{};
                const size_t start = to_digits(v, 16, false, digits);
                std::array<char, k_digits_cap + 2> body{'0', 'x'};
                const size_t n = digits.size() - start;
                for (size_t i = 0; i < n; ++i) { body[2 + i] = digits[start + i]; }
                Spec s = spec;
                s.m_zero = false;
                emit_padded(sink, body.data(), n + 2, '\0', s, false);
                break;
            }
            case 'c': {
                const char c = static_cast<char>(va_arg(args, int));
                Spec s = spec;
                s.m_zero = false;
                emit_padded(sink, &c, 1, '\0', s, false);
                break;
            }
            case 's': {
                const char* s = va_arg(args, const char*);
                if (s == nullptr) { s = "(null)"; }
                const size_t max = spec.m_precision >= 0 ? static_cast<size_t>(spec.m_precision)
                                                         : static_cast<size_t>(-1);
                const size_t n = str_len(s, max);
                Spec sp = spec;
                sp.m_zero = false;
                emit_padded(sink, s, n, '\0', sp, false);
                break;
            }
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G': emit_double(sink, va_arg(args, double), conv, spec); break;
            case '%': sink.put('%'); break;
            case '\0':
                // Dangling spec at end of string: emit verbatim.
                for (const char* q = spec_start; *q != '\0'; ++q) { sink.put(*q); }
                sink.finish();
                return static_cast<int>(sink.m_len);
            default:
                // Unknown conversion: emit the spec verbatim, consume nothing.
                for (const char* q = spec_start; q <= p; ++q) { sink.put(*q); }
                break;
        }
    }

    sink.finish();
    return static_cast<int>(sink.m_len);
}

// NOLINTNEXTLINE(modernize-avoid-variadic-functions,cert-dcl50-cpp) printf-compatible by design
inline int rt_snprintf(char* out,
                       size_t capacity,
                       const char* fmt,
                       ...) noexcept TANH_NONBLOCKING_FUNCTION {
    va_list args;
    va_start(args, fmt);
    const int n = rt_vsnprintf(out, capacity, fmt, args);
    va_end(args);
    return n;
}

}  // namespace thl::core
