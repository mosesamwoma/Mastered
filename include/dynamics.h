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

struct BandStatistics {
    double meanDb = -90.0;
    double p10Db = -90.0;
    double p50Db = -90.0;
    double p95Db = -90.0;

    double spreadDb() const { return p95Db - p50Db; }
};

class BandLevelMeter {
public:
    BandLevelMeter(double sampleRate, double lowHz, double highHz);

    void process(const AudioBlock& block, size_t frames);
    std::array<BandStatistics, 3> statistics() const;

private:
    ThreeBandSplitter splitter_;
    size_t blockFrames_;
    size_t blockFilled_ = 0;
    std::array<double, 3> blockSums_{{0.0, 0.0, 0.0}};
    std::array<double, 3> totalSums_{{0.0, 0.0, 0.0}};
    double totalFrames_ = 0.0;
    std::array<std::vector<double>, 3> levels_;
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
    std::array<double, 3> power_{{0.0, 0.0, 0.0}};
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
