#include "core/dsp/Fft.h"

#include <cmath>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace holotoner {

void Fft::prepare(std::size_t size)
{
    if (size < 2 || (size & (size - 1)) != 0)
        throw std::invalid_argument("FFT size must be a power of two");
    size_ = size;
    twiddles_.resize(size / 2);
    for (std::size_t k = 0; k < size / 2; ++k) {
        const auto angle = -2.0 * std::numbers::pi * static_cast<double>(k) / static_cast<double>(size);
        twiddles_[k] = {std::cos(angle), std::sin(angle)};
    }
    reversed_.resize(size);
    std::size_t bits = 0;
    while ((std::size_t {1} << bits) < size)
        ++bits;
    for (std::size_t i = 0; i < size; ++i) {
        std::size_t reversed = 0;
        for (std::size_t bit = 0; bit < bits; ++bit)
            if ((i & (std::size_t {1} << bit)) != 0)
                reversed |= std::size_t {1} << (bits - 1 - bit);
        reversed_[i] = reversed;
    }
}

void Fft::transform(std::span<std::complex<double>> data, bool inverse) const
{
    const auto size = size_;
    if (data.size() != size || size == 0)
        return;
    for (std::size_t i = 0; i < size; ++i)
        if (i < reversed_[i])
            std::swap(data[i], data[reversed_[i]]);
    for (std::size_t length = 2; length <= size; length <<= 1) {
        const auto half = length / 2;
        const auto stride = size / length;
        for (std::size_t start = 0; start < size; start += length)
            for (std::size_t k = 0; k < half; ++k) {
                const auto twiddle = twiddles_[k * stride];
                const auto wr = twiddle.real();
                const auto wi = inverse ? -twiddle.imag() : twiddle.imag();
                const auto b = data[start + k + half];
                const auto product = std::complex<double> {b.real() * wr - b.imag() * wi,
                                                           b.real() * wi + b.imag() * wr};
                const auto a = data[start + k];
                data[start + k] = a + product;
                data[start + k + half] = a - product;
            }
    }
}

} // namespace holotoner
