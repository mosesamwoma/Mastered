#include "linear_phase.h"

#include "biquad.h"
#include "constants.h"

#include <algorithm>
#include <cmath>

namespace mastered {

namespace {

double besselI0(double x) {
    double sum = 1.0;
    double term = 1.0;
    const double q = x * x * 0.25;
    for (int k = 1; k < 80; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17) {
            break;
        }
    }
    return sum;
}

}

size_t LinearPhaseFilter::fftSizeFor(double sampleRate) {
    if (sampleRate <= 56000.0) {
        return 16384;
    }
    return sampleRate <= 112000.0 ? 32768 : 65536;
}

size_t LinearPhaseFilter::latencyFor(double sampleRate) {
    const size_t size = fftSizeFor(sampleRate);
    return size / 2 - 1;
}

LinearPhaseFilter::LinearPhaseFilter(const std::vector<EQBand>& bands, double sampleRate)
    : size_(fftSizeFor(sampleRate)),
      taps_(size_ / 2 - 1),
      block_(size_ / 4),
      fft_(size_),
      response_(size_),
      work_(size_),
      window_(size_, 0.0),
      pending_(block_, 0.0),
      output_(block_, 0.0) {
    std::vector<BiquadCoefficients> filters;
    filters.reserve(bands.size());
    for (const EQBand& band : bands) {
        filters.push_back(designBand(band, sampleRate));
    }
    const size_t half = size_ / 2;
    std::vector<double> magnitude(half + 1, 1.0);
    for (size_t k = 0; k <= half; ++k) {
        const double w = 2.0 * kPi * static_cast<double>(k) / static_cast<double>(size_);
        const double cosW = std::cos(w);
        const double sinW = std::sin(w);
        const double cos2W = std::cos(2.0 * w);
        const double sin2W = std::sin(2.0 * w);
        double db = 0.0;
        for (const BiquadCoefficients& c : filters) {
            db += biquadResponseDb(c, cosW, sinW, cos2W, sin2W);
        }
        magnitude[k] = std::pow(10.0, db / 20.0);
    }
    for (size_t k = 0; k < size_; ++k) {
        work_[k] = std::complex<double>(magnitude[k <= half ? k : size_ - k], 0.0);
    }
    fft_.forward(work_);
    std::vector<double> zeroPhase(size_);
    for (size_t n = 0; n < size_; ++n) {
        zeroPhase[n] = work_[n].real() / static_cast<double>(size_);
    }
    const size_t centre = (taps_ - 1) / 2;
    constexpr double kBeta = 8.0;
    const double norm = besselI0(kBeta);
    std::fill(response_.begin(), response_.end(), std::complex<double>(0.0, 0.0));
    for (size_t m = 0; m < taps_; ++m) {
        const double r = (static_cast<double>(m) - static_cast<double>(centre)) / static_cast<double>(centre);
        const double w = besselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / norm;
        const size_t source = (m + size_ - centre) % size_;
        response_[m] = std::complex<double>(zeroPhase[source] * w, 0.0);
    }
    fft_.forward(response_);
}

void LinearPhaseFilter::runBlock() {
    const size_t keep = size_ - block_;
    std::copy(window_.begin() + static_cast<std::ptrdiff_t>(block_), window_.end(), window_.begin());
    std::copy(pending_.begin(), pending_.end(), window_.begin() + static_cast<std::ptrdiff_t>(keep));
    for (size_t n = 0; n < size_; ++n) {
        work_[n] = std::complex<double>(window_[n], 0.0);
    }
    fft_.forward(work_);
    for (size_t k = 0; k < size_; ++k) {
        work_[k] = std::conj(work_[k] * response_[k]);
    }
    fft_.forward(work_);
    const double scale = 1.0 / static_cast<double>(size_);
    for (size_t n = keep; n < size_; ++n) {
        output_.push_back(work_[n].real() * scale);
    }
}

void LinearPhaseFilter::process(double* data, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        pending_[pendingCount_++] = data[i];
        if (pendingCount_ == block_) {
            runBlock();
            pendingCount_ = 0;
        }
        data[i] = output_.front();
        output_.pop_front();
    }
}

}
