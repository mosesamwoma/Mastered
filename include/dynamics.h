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

constexpr size_t kMaxDynamicsBands = 5;

struct DynamicsSettings {
    bool enabled = false;
    size_t bandCount = 3;
    std::array<double, kMaxDynamicsBands - 1> crossoversHz{{150.0, 3000.0, 0.0, 0.0}};
    std::array<CompressorBand, kMaxDynamicsBands> bands;

    std::vector<double> crossovers() const {
        return std::vector<double>(crossoversHz.begin(), crossoversHz.begin() + static_cast<std::ptrdiff_t>(bandCount - 1));
    }
};

std::vector<double> defaultCrossovers(size_t bandCount, double sampleRate);

class BandSplitter {
public:
    BandSplitter(const std::vector<double>& crossoversHz, double sampleRate);

    size_t bandCount() const { return stages_.size() + 1; }
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

    std::vector<LR4> stages_;
    std::vector<std::vector<LR4>> alignment_;
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
    BandLevelMeter(double sampleRate, const std::vector<double>& crossoversHz);

    void process(const AudioBlock& block, size_t frames);
    std::vector<BandStatistics> statistics() const;

private:
    BandSplitter splitter_;
    size_t bands_;
    size_t blockFrames_;
    size_t blockFilled_ = 0;
    std::vector<double> blockSums_;
    std::vector<double> totalSums_;
    double totalFrames_ = 0.0;
    std::vector<std::vector<double>> levels_;
};

class MultibandCompressor {
public:
    MultibandCompressor(const DynamicsSettings& settings, double sampleRate, size_t channels);

    void process(AudioBlock& block, size_t frames);

private:
    double reductionDbFor(size_t band, double level) const;

    DynamicsSettings settings_;
    size_t channels_;
    size_t bands_;
    std::vector<BandSplitter> splitters_;
    std::array<double, kMaxDynamicsBands> attack_{};
    std::array<double, kMaxDynamicsBands> release_{};
    std::array<double, kMaxDynamicsBands> detector_{};
    std::array<double, kMaxDynamicsBands> power_{};
    std::array<double, kMaxDynamicsBands> reductionDb_{};
};

struct DeEsserSettings {
    bool enabled = false;
    double lowHz = 4500.0;
    double highHz = 9500.0;
    double thresholdDb = -40.0;
    double ratio = 4.0;
    double kneeDb = 4.0;
    double maxReductionDb = 8.0;
    double attackMs = 0.5;
    double releaseMs = 30.0;
};

bool deEsserBand(double sampleRate, double& lowHz, double& highHz);

class SibilanceMeter {
public:
    SibilanceMeter(double sampleRate, double lowHz, double highHz);

    void process(const AudioBlock& block, size_t frames);
    BandStatistics statistics() const;

private:
    Biquad highPass_;
    Biquad lowPass_;
    size_t blockFrames_;
    size_t blockFilled_ = 0;
    double blockSum_ = 0.0;
    std::vector<double> levels_;
};

class DeEsser {
public:
    DeEsser(const DeEsserSettings& settings, double sampleRate, size_t channels);

    void process(AudioBlock& block, size_t frames);

private:
    struct Filters {
        Biquad highPass;
        Biquad lowPass;
    };

    DeEsserSettings settings_;
    size_t channels_;
    std::vector<Filters> filters_;
    double detector_;
    double attack_;
    double release_;
    double power_ = 0.0;
    double reductionDb_ = 0.0;
};

struct GlueSettings {
    bool enabled = false;
    double thresholdDb = -12.0;
    double ratio = 1.5;
    double kneeDb = 8.0;
    double attackMs = 25.0;
    double releaseMs = 140.0;
};

class GlueCompressor {
public:
    GlueCompressor(const GlueSettings& settings, double sampleRate, size_t channels);

    void process(AudioBlock& block, size_t frames);

private:
    GlueSettings settings_;
    size_t channels_;
    double detector_;
    double attack_;
    double releaseFast_;
    double releaseSlow_;
    double power_ = 0.0;
    double reductionDb_ = 0.0;
};

constexpr size_t kMaxResonanceBands = 4;

struct ResonanceBand {
    double frequency = 3000.0;
    double q = 6.0;
    double maxCutDb = 3.0;
    double thresholdDb = 2.0;
};

struct ResonanceSettings {
    bool enabled = false;
    size_t count = 0;
    std::array<ResonanceBand, kMaxResonanceBands> bands;
};

class ResonanceTamer {
public:
    ResonanceTamer(const ResonanceSettings& settings, double sampleRate, size_t channels);

    void process(AudioBlock& block, size_t frames);

private:
    struct Detector {
        Biquad centre;
        Biquad low;
        Biquad high;
    };

    struct BandState {
        std::vector<Detector> detectors;
        std::vector<Biquad> tone;
        double centrePower = 0.0;
        double neighbourPower = 0.0;
        double cutDb = 0.0;
        double appliedDb = 0.0;
    };

    ResonanceSettings settings_;
    double sampleRate_;
    size_t channels_;
    double detector_;
    double attack_;
    double release_;
    size_t counter_ = 0;
    std::vector<BandState> states_;
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
