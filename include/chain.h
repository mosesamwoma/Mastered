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
    bool softClip = false;
    double softClipKneeDb = 3.0;
    bool limiterEnabled = true;
    double ceilingDb = -1.0;
    double lookaheadMs = 3.0;
    double releaseMs = 80.0;
};

class ProcessingChain {
public:
    ProcessingChain(const ChainPlan& plan, double sampleRate, size_t channels);

    size_t latency() const;
    void process(AudioBlock& block, size_t frames);

private:
    ChainPlan plan_;
    size_t channels_;
    bool useMidSide_;
    double gainLinear_;
    std::vector<std::vector<Biquad>> channelFilters_;
    std::vector<Biquad> midFilters_;
    std::vector<Biquad> sideFilters_;
    std::unique_ptr<MultibandCompressor> compressor_;
    std::unique_ptr<SoftClipper> clipper_;
    std::unique_ptr<TruePeakLimiter> limiter_;
    std::vector<double> mid_;
    std::vector<double> side_;
};

}
