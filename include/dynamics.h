#pragma once

#include "biquad.h"
#include "types.h"

#include <array>
#include <cstddef>
#include <vector>

namespace mastered {

struct CompressorBand {
    double thresholdDb = 0.0;
    double ratio = 1.0;
    double kneeDb = 6.0;
    double attackMs = 10.0;
    double releaseMs = 150.0;
};

struct DynamicsSettings {
    bool enabled = false;
    double lowCrossoverHz = 150.0;
    double highCrossoverHz = 3000.0;
    std::array<CompressorBand, 3> bands;
};

class ThreeBandSplitter {
public:
    ThreeBandSplitter(double lowHz, double highHz, double sampleRate);

    void split(double x, double* out);

private:
    struct LR4 {
        Biquad lowA;
        Biquad lowB;
        Biquad highA;
        Biquad highB;

        void configure(const BiquadCoefficients& low, const BiquadCoefficients& high) {
            lowA.setCoefficients(low);
            lowB.setCoefficients(low);
            highA.setCoefficients(high);
            highB.setCoefficients(high);
        }

        void process(double x, double& low, double& high) {
            low = lowB.process(lowA.process(x));
            high = highB.process(highA.process(x));
        }
    };

    LR4 first_;
    LR4 second_;
    LR4 align_;
};

class BandLevelMeter {
public:
    BandLevelMeter(double sampleRate, double lowHz, double highHz);

    void process(const AudioBlock& block, size_t frames);
    std::array<double, 3> rmsDb() const;

private:
    ThreeBandSplitter splitter_;
    std::array<double, 3> sums_{{0.0, 0.0, 0.0}};
    double count_ = 0.0;
};

class MultibandCompressor {
public:
    MultibandCompressor(const DynamicsSettings& settings, double sampleRate, size_t channels);

    void process(AudioBlock& block, size_t frames);

private:
    double gainFor(size_t band, double level) const;

    DynamicsSettings settings_;
    size_t channels_;
    std::vector<ThreeBandSplitter> splitters_;
    std::array<double, 3> attack_{{0.0, 0.0, 0.0}};
    std::array<double, 3> release_{{0.0, 0.0, 0.0}};
    std::array<double, 3> envelope_{{0.0, 0.0, 0.0}};
};

class SoftClipper {
public:
    SoftClipper(double ceilingLinear, double kneeDb);

    double apply(double x) const;

private:
    double ceiling_;
    double threshold_;
};

}
