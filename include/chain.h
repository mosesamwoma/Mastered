#pragma once

#include "biquad.h"
#include "dynamics.h"
#include "limiter.h"
#include "types.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace mastered {

struct ChainPlan {
    std::vector<EQBand> midBands;
    std::vector<EQBand> sideBands;
    bool midSide = false;
    double sideGain = 1.0;
    DynamicsSettings dynamics;
    double gainDb = 0.0;
    double monoBassHz = 0.0;
    bool softClip = false;
    double softClipKneeDb = 3.0;
    bool limiterEnabled = true;
    double ceilingDb = -1.0;
    double lookaheadMs = 5.0;
    double releaseMs = 80.0;
    bool bassLimiter = false;
    double bassLimiterHz = 140.0;
    double bassCeilingOffsetDb = 3.0;
};

enum class ChainStage {
    Full,
    Pre,
    Post
};

class ProcessingChain {
public:
    ProcessingChain(const ChainPlan& plan, double sampleRate, size_t channels, ChainStage stage = ChainStage::Full);

    size_t latency() const;
    void process(AudioBlock& block, size_t frames);
    void processPre(AudioBlock& block, size_t frames);
    void processPost(AudioBlock& block, size_t frames);
    void collectLimiterStatistics(bool enabled);
    LimiterStats limiterStatistics() const;

private:
    struct BassSplit {
        Biquad lowA;
        Biquad lowB;
        Biquad highA;
        Biquad highB;
    };

    ChainPlan plan_;
    size_t channels_;
    ChainStage stage_;
    bool useMidSide_;
    double gainLinear_;
    std::vector<std::vector<Biquad>> channelFilters_;
    std::vector<Biquad> midFilters_;
    std::vector<Biquad> sideFilters_;
    std::vector<Biquad> sideHighPass_;
    std::unique_ptr<MultibandCompressor> compressor_;
    std::unique_ptr<SoftClipper> clipper_;
    std::unique_ptr<TruePeakLimiter> limiter_;
    std::unique_ptr<TruePeakLimiter> bassLimiter_;
    std::vector<BassSplit> bassSplit_;
    std::vector<std::vector<double>> bassDelay_;
    size_t bassDelayPosition_ = 0;
    AudioBlock bassLow_;
    AudioBlock bassHigh_;
    std::vector<double> mid_;
    std::vector<double> side_;
};

}
