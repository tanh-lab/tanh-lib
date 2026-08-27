#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <limits>
#include <string>

#include "tanh/core/RtFormat.h"

using thl::core::rt_snprintf;

namespace {

// Format with both rt_snprintf and the C library and compare. Only used for
// cases whose output the C standard fully specifies.
// NOLINTBEGIN(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-security"
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
template <typename... Args>
void expect_same_as_libc(const char* fmt, Args... args) {
    std::array<char, 256> ours{};
    std::array<char, 256> ref{};
    const int n_ours = rt_snprintf(ours.data(), ours.size(), fmt, args...);
    const int n_ref = std::snprintf(ref.data(), ref.size(), fmt, args...);
    EXPECT_STREQ(ours.data(), ref.data()) << "fmt=" << fmt;
    EXPECT_EQ(n_ours, n_ref) << "fmt=" << fmt;
}

template <typename... Args>
std::string fmt(const char* format, Args... args) {
    std::array<char, 256> buf{};
    rt_snprintf(buf.data(), buf.size(), format, args...);
    return buf.data();
}
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
// NOLINTEND(cppcoreguidelines-pro-type-vararg,hicpp-vararg)

}  // namespace

TEST(RtFormat, PlainText) {
    expect_same_as_libc("hello");
    expect_same_as_libc("");
    expect_same_as_libc("100%%");
}

TEST(RtFormat, Integers) {
    expect_same_as_libc("%d", 0);
    expect_same_as_libc("%d", 42);
    expect_same_as_libc("%d", -42);
    expect_same_as_libc("%i", -2147483647 - 1);
    expect_same_as_libc("%u", 4294967295U);
    expect_same_as_libc("%ld", -1234567890L);  // fits a 32-bit long (Windows)
    expect_same_as_libc("%lld", -9223372036854775807LL - 1);
    expect_same_as_libc("%llu", 18446744073709551615ULL);
    expect_same_as_libc("%zu", static_cast<size_t>(123456));
    expect_same_as_libc("%hd", 70000);  // wraps to short
    expect_same_as_libc("%hhu", 300);   // wraps to unsigned char
}

TEST(RtFormat, IntegerFlagsWidthPrecision) {
    expect_same_as_libc("%5d|", 42);
    expect_same_as_libc("%-5d|", 42);
    expect_same_as_libc("%05d|", 42);
    expect_same_as_libc("%05d|", -42);
    expect_same_as_libc("%+d", 42);
    expect_same_as_libc("% d", 42);
    expect_same_as_libc("%.4d", 42);
    expect_same_as_libc("%8.4d|", -42);
    expect_same_as_libc("%.0d|", 0);
    expect_same_as_libc("%*d|", 6, 42);
    expect_same_as_libc("%-*d|", 6, 42);
    expect_same_as_libc("%.*d|", 3, 7);
}

TEST(RtFormat, HexOctal) {
    expect_same_as_libc("%x", 0xdeadbeefU);
    expect_same_as_libc("%X", 0xdeadbeefU);
    expect_same_as_libc("%08x", 0xbeefU);
    expect_same_as_libc("%o", 511U);
    expect_same_as_libc("%llx", 0x123456789abcdefULL);
}

TEST(RtFormat, StringsAndChars) {
    expect_same_as_libc("%s", "abc");
    expect_same_as_libc("%10s|", "abc");
    expect_same_as_libc("%-10s|", "abc");
    expect_same_as_libc("%.2s", "abcdef");
    expect_same_as_libc("%c", 'x');
    expect_same_as_libc("%3c|", 'x');
    expect_same_as_libc("[%s] %s: %d", "thl", "value", 7);
    EXPECT_EQ(fmt("%s", static_cast<const char*>(nullptr)), "(null)");
}

TEST(RtFormat, FixedFloat) {
    expect_same_as_libc("%f", 0.0);
    expect_same_as_libc("%f", 3.14159265);
    expect_same_as_libc("%f", -3.14159265);
    expect_same_as_libc("%.2f", 2.675);  // 2.67 or 2.68 depending on binary repr; libc agrees
    expect_same_as_libc("%.0f", 2.5);
    expect_same_as_libc("%.3f", 0.0005);
    expect_same_as_libc("%.3f", 1e-9);
    expect_same_as_libc("%8.3f|", 3.14159);
    expect_same_as_libc("%-8.3f|", 3.14159);
    expect_same_as_libc("%08.3f|", -3.14159);
    expect_same_as_libc("%+.1f", 1.0);
    expect_same_as_libc("%.1f", 99.95);
    expect_same_as_libc("%.2f", 48000.0);
    expect_same_as_libc("%.6f", 123456789.123456);
    expect_same_as_libc("%f", -0.0);
}

TEST(RtFormat, FloatSpecials) {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_EQ(fmt("%f", nan), "nan");
    EXPECT_EQ(fmt("%F", nan), "NAN");
    EXPECT_EQ(fmt("%f", inf), "inf");
    EXPECT_EQ(fmt("%f", -inf), "-inf");
    EXPECT_EQ(fmt("%+f", inf), "+inf");
    EXPECT_EQ(fmt("%6f|", inf), "   inf|");
}

TEST(RtFormat, ExponentFloat) {
    expect_same_as_libc("%e", 0.0);
    expect_same_as_libc("%e", 12345.678);
    expect_same_as_libc("%E", 12345.678);
    expect_same_as_libc("%.2e", 0.000123456);
    expect_same_as_libc("%.3e", -9.9996e10);  // rounds up into next exponent
    expect_same_as_libc("%e", 1e100);
    expect_same_as_libc("%e", 1e-100);
    expect_same_as_libc("%12.3e|", 1234.5);
}

TEST(RtFormat, GeneralFloat) {
    expect_same_as_libc("%g", 0.0);
    expect_same_as_libc("%g", 100000.0);
    expect_same_as_libc("%g", 1000000.0);
    expect_same_as_libc("%g", 0.0001);
    expect_same_as_libc("%g", 0.00001);
    expect_same_as_libc("%g", 3.14159265);
    expect_same_as_libc("%g", 48000.0);
    expect_same_as_libc("%.3g", 3.14159265);
    expect_same_as_libc("%G", 1e-10);
    expect_same_as_libc("%g", 123456789.0);
}

TEST(RtFormat, HugeFixedFallsBackToExponent) {
    // Fixed conversion would overflow the integer path; we switch to %e
    // rather than print garbage.
    const std::string out = fmt("%f", 1e30);
    EXPECT_NE(out.find('e'), std::string::npos) << out;
    EXPECT_EQ(out.substr(0, 2), "1.");
}

TEST(RtFormat, Pointer) {
    int x = 0;
    const std::string out = fmt("%p", static_cast<void*>(&x));
    EXPECT_EQ(out.substr(0, 2), "0x");
    EXPECT_EQ(fmt("%p", static_cast<void*>(nullptr)), "0x0");
}

TEST(RtFormat, UnknownConversionEmittedVerbatim) {
    EXPECT_EQ(fmt("a%qb"), "a%qb");
    EXPECT_EQ(fmt("100%"), "100%");
    EXPECT_EQ(fmt("%"), "%");
}

TEST(RtFormat, TruncationIsReportedAndTerminated) {
    std::array<char, 8> buf{};
    buf.fill('#');
    const int n = rt_snprintf(buf.data(), buf.size(), "%s", "0123456789");
    EXPECT_EQ(n, 10);
    EXPECT_STREQ(buf.data(), "0123456");
    EXPECT_EQ(buf[7], '\0');

    // Truncation inside a conversion, not just between them.
    const int m = rt_snprintf(buf.data(), buf.size(), "%d-%d", 12345, 67890);
    EXPECT_EQ(m, 11);
    EXPECT_STREQ(buf.data(), "12345-6");
}

TEST(RtFormat, ZeroCapacityWritesNothing) {
    std::array<char, 4> buf{'a', 'b', 'c', 'd'};
    const int n = rt_snprintf(buf.data(), 0, "%d", 42);
    EXPECT_EQ(n, 2);
    EXPECT_EQ(buf[0], 'a');
}

TEST(RtFormat, NullFormat) {
    std::array<char, 4> buf{'x', 'x', 'x', 'x'};
    const int n = thl::core::rt_snprintf(buf.data(), buf.size(), nullptr);
    EXPECT_EQ(n, 0);
    EXPECT_STREQ(buf.data(), "");
}

TEST(RtFormat, MixedRealisticMessages) {
    EXPECT_EQ(fmt("Missing samples: %zu in session %d for tensor %u", static_cast<size_t>(480), 3, 1U),
              "Missing samples: 480 in session 3 for tensor 1");
    EXPECT_EQ(fmt("xrun: %.2f ms late (%d frames @ %.1f kHz)", 1.2345, 64, 48.0),
              "xrun: 1.23 ms late (64 frames @ 48.0 kHz)");
    EXPECT_EQ(fmt("%s=%g", "gain", 0.5), "gain=0.5");
}
