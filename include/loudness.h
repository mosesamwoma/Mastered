#pragma once

#include "biquad.h"
#include "constants.h"
#include "types.h"

#include <array>
#include <cstddef>
#include <vector>

namespace mastered {

struct OversamplingKernel {
    size_t factor = 4;
    size_t tapsPerPhase = 24;
    size_t delay = 12;
    size_t firstPhase = 1;
    double bound = 1.0;
    std::vector<double> coefficients;

    const double* phase(size_t index) const { return coefficients.data() + index * tapsPerPhase; }
};

enum class PeakMode { Off, Standard, Accurate };

const OversamplingKernel& standardKernel();
const OversamplingKernel& accurateKernel();
double truePeakOfWindow(const double* window, const OversamplingKernel& kernel);
double channelWeight(size_t index, size_t channels);

struct LoudnessResult {
    double integratedLufs = -120.0;
    double loudnessRangeLu = 0.0;
};

class LoudnessMeter {
public:
    LoudnessMeter(double sampleRate, size_t channels);

    void process(const AudioBlock& block, size_t frames);
    LoudnessResult finish() const;

private:
    struct KWeighting {
        Biquad shelf;
        Biquad highPass;
        double process(double x) { return highPass.process(shelf.process(x)); }
    };

    size_t channels_;
    size_t hop_;
    std::vector<KWeighting> filters_;
    std::vector<double> weights_;
    std::vector<double> subBlocks_;
    double accumulator_ = 0.0;
    size_t accumulated_ = 0;
};

class TruePeakMeter {
public:
    TruePeakMeter(size_t channels, const OversamplingKernel& kernel);

    void process(const AudioBlock& block, size_t frames);
    void finish();
    double peakLinear() const { return peak_; }
    double peakDb() const;

private:
    size_t channels_;
    const OversamplingKernel& kernel_;
    std::vector<std::vector<double>> history_;
    std::vector<size_t> hot_;
    std::vector<double> work_;
    double peak_ = 0.0;
};

struct Measurements {
    double integratedLufs = -120.0;
    double loudnessRangeLu = 0.0;
    double truePeakDb = -200.0;
    double correlation = 1.0;
    double sideToMidDb = -120.0;
    bool hasStereo = false;
};

class Meter {
public:
    Meter(double sampleRate, size_t channels, PeakMode peakMode = PeakMode::Standard);

    void process(const AudioBlock& block, size_t frames);
    Measurements finish();

private:
    size_t channels_;
    PeakMode peakMode_;
    LoudnessMeter loudness_;
    TruePeakMeter truePeak_;
    double leftEnergy_ = 0.0;
    double rightEnergy_ = 0.0;
    double crossEnergy_ = 0.0;
    double midEnergy_ = 0.0;
    double sideEnergy_ = 0.0;
};

}
