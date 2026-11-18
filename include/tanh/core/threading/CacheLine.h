// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tanh/core/AbiNamespace.h>

#include <cstddef>

namespace thl::inline THL_ABI_NAMESPACE::core {

/// The distance that keeps two variables written by different threads off one cache line (no
/// false sharing): 128 bytes, Apple Silicon's line (hw.cachelinesize); on x86-64 (64-byte lines,
/// with adjacent-line prefetch) it is the usual padding too. One constant for every lock-free
/// structure in core, rather than std::hardware_destructive_interference_size, whose value may
/// differ between compilers of one build and is part of a type's layout.
inline constexpr std::size_t k_cache_line_size = 128;

}  // namespace thl::inline THL_ABI_NAMESPACE::core
