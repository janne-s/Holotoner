#pragma once

#include <complex>
#include <cstddef>
#include <span>
#include <vector>

namespace holotoner {

/** Prepared in-place radix-2 FFT. transform() allocates nothing and never throws. */
class Fft {
public:
    void prepare(std::size_t size);
    void transform(std::span<std::complex<double>> data, bool inverse) const;

private:
    std::size_t size_ = 0;
    std::vector<std::complex<double>> twiddles_;
    std::vector<std::size_t> reversed_;
};

} // namespace holotoner
