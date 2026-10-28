#pragma once

#include "fft.h"
#include "types.h"

#include <complex>
#include <cstddef>
#include <vector>

namespace mastered {

struct AverageSpectrum {
    double sampleRate = 0.0;
    size_t fftSize = 0;
    size_t frames = 0;
    bool hasSide = false;
    std::vector<double> mid;
    std::vector<double> side;
};

class SpectrumAccumulator {
public:
    SpectrumAccumulator(double sampleRate, size_t channels, size_t fftSize);

    void push(const AudioBlock& block, size_t frames);
    AverageSpectrum finish();

private:
    void processFrame(size_t offset);

    double sampleRate_;
    size_t channels_;
    size_t fftSize_;
    FFT fft_;
    std::vector<double> window_;
    double windowEnergy_ = 0.0;
    std::vector<double> midPending_;
    std::vector<double> sidePending_;
    std::vector<std::complex<double>> work_;
    std::vector<double> midSum_;
    std::vector<double> sideSum_;
    size_t accepted_ = 0;
    size_t silent_ = 0;
};

std::vector<double> makeBandCentres(double minHz, double maxHz);
std::vector<double> bandLevelsDb(const AverageSpectrum& spectrum, bool side, const std::vector<double>& centres, double widthOctaves);

}
