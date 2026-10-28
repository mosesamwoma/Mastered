#pragma once

#include <complex>
#include <cstddef>
#include <vector>

namespace mastered {

class FFT {
public:
    explicit FFT(size_t size);

    size_t size() const { return size_; }
    void forward(std::vector<std::complex<double>>& data) const;

private:
    size_t size_;
    std::vector<size_t> bitReverse_;
    std::vector<std::complex<double>> twiddles_;
};

}
