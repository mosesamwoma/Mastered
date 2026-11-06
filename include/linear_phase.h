#pragma once

#include "fft.h"
#include "types.h"

#include <complex>
#include <cstddef>
#include <deque>
#include <vector>

namespace mastered {

class LinearPhaseFilter {
public:
    LinearPhaseFilter(const std::vector<EQBand>& bands, double sampleRate);

    static size_t latencyFor(double sampleRate);

    size_t latency() const { return block_ + (taps_ - 1) / 2; }
    void process(double* data, size_t frames);

private:
    static size_t fftSizeFor(double sampleRate);
    void runBlock();

    size_t size_;
    size_t taps_;
    size_t block_;
    FFT fft_;
    std::vector<std::complex<double>> response_;
    std::vector<std::complex<double>> work_;
    std::vector<double> window_;
    std::vector<double> pending_;
    size_t pendingCount_ = 0;
    std::deque<double> output_;
};

}
