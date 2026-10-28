#include "fft.h"

#include "constants.h"
#include "error.h"

#include <cmath>
#include <utility>

namespace mastered {

FFT::FFT(size_t size) : size_(size) {
    if (size < 2 || (size & (size - 1)) != 0) {
        throw MasteredError("FFT size must be a power of two and at least 2");
    }
    size_t bits = 0;
    while ((static_cast<size_t>(1) << bits) < size) {
        ++bits;
    }
    bitReverse_.resize(size);
    for (size_t i = 0; i < size; ++i) {
        size_t reversed = 0;
        for (size_t b = 0; b < bits; ++b) {
            if (i & (static_cast<size_t>(1) << b)) {
                reversed |= static_cast<size_t>(1) << (bits - 1 - b);
            }
        }
        bitReverse_[i] = reversed;
    }
    twiddles_.resize(size / 2);
    for (size_t k = 0; k < size / 2; ++k) {
        const double angle = -2.0 * kPi * static_cast<double>(k) / static_cast<double>(size);
        twiddles_[k] = std::complex<double>(std::cos(angle), std::sin(angle));
    }
}

void FFT::forward(std::vector<std::complex<double>>& data) const {
    if (data.size() != size_) {
        throw MasteredError("FFT input size mismatch");
    }
    for (size_t i = 0; i < size_; ++i) {
        if (i < bitReverse_[i]) {
            std::swap(data[i], data[bitReverse_[i]]);
        }
    }
    for (size_t length = 2; length <= size_; length <<= 1) {
        const size_t half = length >> 1;
        const size_t step = size_ / length;
        for (size_t start = 0; start < size_; start += length) {
            for (size_t k = 0; k < half; ++k) {
                const std::complex<double> w = twiddles_[k * step];
                const std::complex<double> u = data[start + k];
                const std::complex<double> v = data[start + k + half] * w;
                data[start + k] = u + v;
                data[start + k + half] = u - v;
            }
        }
    }
}

}
