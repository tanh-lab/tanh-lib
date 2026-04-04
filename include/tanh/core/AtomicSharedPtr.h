#pragma once

#include <memory>

namespace thl {

/// Platform-portable atomic shared_ptr wrapper.
/// Uses std::atomic<std::shared_ptr<T>> on libstdc++ (Linux/Android) and falls
/// back to the deprecated free-function API on libc++ (Apple Clang) where the
/// C++20 specialization is not yet available.
#ifdef __cpp_lib_atomic_shared_ptr

template <typename T>
using AtomicSharedPtr = std::atomic<std::shared_ptr<T>>;

template <typename T>
std::shared_ptr<T> atomic_load(const AtomicSharedPtr<T>& ptr) {
    return ptr.load(std::memory_order_acquire);
}

template <typename T>
void atomic_store(AtomicSharedPtr<T>& ptr, std::shared_ptr<T> desired) {
    ptr.store(std::move(desired), std::memory_order_release);
}

#else

template <typename T>
using AtomicSharedPtr = std::shared_ptr<T>;

template <typename T>
std::shared_ptr<T> atomic_load(const AtomicSharedPtr<T>& ptr) {
    return std::atomic_load_explicit(&ptr, std::memory_order_acquire);
}

template <typename T>
void atomic_store(AtomicSharedPtr<T>& ptr, std::shared_ptr<T> desired) {
    std::atomic_store_explicit(&ptr, std::move(desired), std::memory_order_release);
}

#endif

}  // namespace thl
