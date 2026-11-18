// What every backend of RealFft shares; the backend itself is src/dsp/fft/RealFft<Backend>.cpp.
#include <tanh/core/AbiNamespace.h>
#include <tanh/dsp/fft/RealFft.h>

#include <bit>
#include <cstddef>
#include <utility>

namespace thl::inline THL_ABI_NAMESPACE::dsp::fft {

RealFft::RealFft() = default;
RealFft::~RealFft() = default;

RealFft::RealFft(RealFft&& other) noexcept
    : m_impl(std::move(other.m_impl)), m_size(std::exchange(other.m_size, 0)) {}

RealFft& RealFft::operator=(RealFft&& other) noexcept {
    if (this != &other) {
        m_impl = std::move(other.m_impl);
        m_size = std::exchange(other.m_size, 0);
    }
    return *this;
}

bool RealFft::is_valid_size(size_t size) {
    return size >= k_min_size && size <= k_max_size && std::has_single_bit(size);
}

bool RealFft::prepare(size_t size) {
    m_impl.reset();
    m_size = 0;
    if (!is_valid_size(size)) { return false; }
    m_impl.reset(create_impl(size));
    if (m_impl == nullptr) { return false; }
    m_size = size;
    return true;
}

}  // namespace thl::inline THL_ABI_NAMESPACE::dsp::fft
