#pragma once

#include "biquad.h"
#include "types.h"

#include <array>
#include <cstddef>
#include <cstdint>
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
    double reductionDbFor(size_t band, double level) const;

    DynamicsSettings settings_;
    size_t channels_;
    std::vector<ThreeBandSplitter> splitters_;
    std::array<double, 3> attack_{{0.0, 0.0, 0.0}};
    std::array<double, 3> release_{{0.0, 0.0, 0.0}};
    std::array<double, 3> detector_{{0.0, 0.0, 0.0}};
    std::array<double, 3> power_{{0.0, 0.0, 0.0}};
    std::array<double, 3> reductionDb_{{0.0, 0.0, 0.0}};
};

class SoftClipper {
public:
    SoftClipper(double ceilingLinear, double kneeDb);

    double apply(double x) const;
    double threshold() const { return threshold_; }

private:
    double ceiling_;
    double threshold_;
};

class OversampledClipper {
public:
    static constexpr size_t kLatency = 16;

    OversampledClipper(double ceilingLinear, double kneeDb, size_t channels);

    size_t latency() const { return kLatency; }
    void process(AudioBlock& block, size_t frames);

private:
    static constexpr size_t kFactor = 4;
    static constexpr size_t kTaps = 16;
    static constexpr size_t kLength = kFactor * kTaps + 1;
    static constexpr size_t kInputRing = 64;
    static constexpr size_t kClipRing = 128;
    static constexpr size_t kHold = 48;

    struct ChannelState {
        std::array<double, kInputRing> input{};
        std::array<double, kClipRing> clipped{};
        size_t hold = 0;
        bool active = false;
    };

    double upsampled(const ChannelState& state, uint64_t time, size_t phase) const;
    void store(ChannelState& state, uint64_t time) const;

    SoftClipper clipper_;
    double gate_;
    std::array<double, kLength> filter_{};
    std::array<std::array<double, kTaps + 1>, kFactor> interpolation_{};
    std::vector<ChannelState> channels_;
    uint64_t position_ = 64;
};

}
