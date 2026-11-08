#pragma once

#include "biquad.h"
#include "dynamics.h"
#include "limiter.h"
#include "linear_phase.h"
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
    DeEsserSettings deEsser;
    ResonanceSettings resonance;
    GlueSettings glue;
    double gainDb = 0.0;
    double monoBassHz = 0.0;
    bool softClip = false;
    double softClipKneeDb = 3.0;
    bool strictPeak = false;
    bool linearPhase = false;
    double subsonicHz = 0.0;
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
    size_t preLatency() const { return preLatency_; }
    void process(AudioBlock& block, size_t frames);
    void processPre(AudioBlock& block, size_t frames);
    void processPost(AudioBlock& block, size_t frames);
    void collectLimiterStatistics(bool enabled);
    LimiterStats limiterStatistics() const;

private:
    struct BassSplit {
        Biquad lowA;
        Biquad lowB;
    };

    ChainPlan plan_;
    size_t channels_;
    ChainStage stage_;
    bool useMidSide_;
    double gainLinear_;
    size_t preLatency_ = 0;
    std::vector<Biquad> subsonic_;
    std::unique_ptr<LinearPhaseFilter> midLinear_;
    std::unique_ptr<LinearPhaseFilter> sideLinear_;
    std::vector<std::unique_ptr<LinearPhaseFilter>> channelLinear_;
    std::vector<std::vector<Biquad>> channelFilters_;
    std::vector<Biquad> midFilters_;
    std::vector<Biquad> sideFilters_;
    std::vector<Biquad> sideHighPass_;
    std::unique_ptr<DeEsser> deEsser_;
    std::unique_ptr<ResonanceTamer> resonance_;
    std::unique_ptr<GlueCompressor> glue_;
    std::unique_ptr<MultibandCompressor> compressor_;
    std::unique_ptr<OversampledClipper> clipper_;
    std::unique_ptr<TruePeakLimiter> limiter_;
    std::unique_ptr<TruePeakLimiter> bassLimiter_;
    std::vector<BassSplit> bassSplit_;
    std::vector<std::vector<double>> bassDryDelay_;
    std::vector<std::vector<double>> bassReferenceDelay_;
    size_t bassDelayPosition_ = 0;
    AudioBlock bassLow_;
    AudioBlock bassDry_;
    AudioBlock bassReference_;
    std::vector<double> mid_;
    std::vector<double> side_;
};

}
