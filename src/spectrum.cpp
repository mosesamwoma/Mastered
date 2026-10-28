#include "spectrum.h"

#include "constants.h"
#include "error.h"

#include <algorithm>
#include <cmath>

namespace mastered {

SpectrumAccumulator::SpectrumAccumulator(double sampleRate, size_t channels, size_t fftSize)
    : sampleRate_(sampleRate),
      channels_(channels),
      fftSize_(fftSize),
      fft_(fftSize),
      window_(fftSize),
      work_(fftSize),
      midSum_(fftSize / 2 + 1, 0.0),
      sideSum_(fftSize / 2 + 1, 0.0) {
    for (size_t i = 0; i < fftSize; ++i) {
        window_[i] = 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(fftSize)));
        windowEnergy_ += window_[i] * window_[i];
    }
}

void SpectrumAccumulator::push(const AudioBlock& block, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        if (channels_ >= 2) {
            midPending_.push_back(0.5 * (block[0][i] + block[1][i]));
            sidePending_.push_back(0.5 * (block[0][i] - block[1][i]));
        } else {
            midPending_.push_back(block[0][i]);
            sidePending_.push_back(0.0);
        }
    }
    const size_t hop = fftSize_ / 2;
    size_t offset = 0;
    while (midPending_.size() - offset >= fftSize_) {
        processFrame(offset);
        offset += hop;
    }
    if (offset > 0) {
        midPending_.erase(midPending_.begin(), midPending_.begin() + static_cast<std::ptrdiff_t>(offset));
        sidePending_.erase(sidePending_.begin(), sidePending_.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

void SpectrumAccumulator::processFrame(size_t offset) {
    double energy = 0.0;
    for (size_t i = 0; i < fftSize_; ++i) {
        const double m = midPending_[offset + i];
        energy += m * m;
    }
    if (energy / static_cast<double>(fftSize_) < 1e-7) {
        ++silent_;
        return;
    }
    for (size_t i = 0; i < fftSize_; ++i) {
        work_[i] = std::complex<double>(midPending_[offset + i] * window_[i], sidePending_[offset + i] * window_[i]);
    }
    fft_.forward(work_);
    const size_t bins = fftSize_ / 2 + 1;
    for (size_t k = 0; k < bins; ++k) {
        const std::complex<double> zk = work_[k];
        const std::complex<double> zn = std::conj(work_[(fftSize_ - k) % fftSize_]);
        const std::complex<double> xm = (zk + zn) * 0.5;
        const std::complex<double> xs = (zk - zn) * std::complex<double>(0.0, -0.5);
        midSum_[k] += std::norm(xm);
        sideSum_[k] += std::norm(xs);
    }
    ++accepted_;
}

AverageSpectrum SpectrumAccumulator::finish() {
    if (accepted_ == 0) {
        throw MasteredError(silent_ > 0 ? "Audio is silent" : "Audio is too short for spectral analysis");
    }
    AverageSpectrum result;
    result.sampleRate = sampleRate_;
    result.fftSize = fftSize_;
    result.frames = accepted_;
    result.hasSide = channels_ >= 2;
    const double scale = 1.0 / (static_cast<double>(accepted_) * windowEnergy_);
    result.mid.resize(midSum_.size());
    result.side.resize(sideSum_.size());
    for (size_t k = 0; k < midSum_.size(); ++k) {
        result.mid[k] = midSum_[k] * scale;
        result.side[k] = sideSum_[k] * scale;
    }
    return result;
}

std::vector<double> makeBandCentres(double minHz, double maxHz) {
    std::vector<double> centres;
    for (int i = 0;; ++i) {
        const double f = minHz * std::pow(2.0, static_cast<double>(i) / 6.0);
        if (f > maxHz * (1.0 + 1e-9)) {
            break;
        }
        centres.push_back(f);
    }
    return centres;
}

std::vector<double> bandLevelsDb(const AverageSpectrum& spectrum, bool side, const std::vector<double>& centres, double widthOctaves) {
    if (side && !spectrum.hasSide) {
        throw MasteredError("Side spectrum is not available for this audio");
    }
    const std::vector<double>& power = side ? spectrum.side : spectrum.mid;
    const double resolution = spectrum.sampleRate / static_cast<double>(spectrum.fftSize);
    const size_t lastBin = power.size() - 1;
    std::vector<double> levels(centres.size());
    for (size_t b = 0; b < centres.size(); ++b) {
        const double centre = centres[b];
        const double low = centre * std::pow(2.0, -widthOctaves / 2.0);
        const double high = centre * std::pow(2.0, widthOctaves / 2.0);
        const size_t first = static_cast<size_t>(std::ceil(low / resolution));
        const size_t last = std::min(static_cast<size_t>(std::floor(high / resolution)), lastBin);
        double value = 0.0;
        if (first <= last) {
            double sum = 0.0;
            for (size_t k = first; k <= last; ++k) {
                sum += power[k];
            }
            value = sum / static_cast<double>(last - first + 1);
        } else {
            const double position = centre / resolution;
            const size_t k0 = std::min(static_cast<size_t>(std::floor(position)), lastBin - 1);
            const double fraction = position - static_cast<double>(k0);
            value = power[k0] * (1.0 - fraction) + power[k0 + 1] * fraction;
        }
        levels[b] = 10.0 * std::log10(std::max(value, 1e-20));
    }
    return levels;
}

}
