#pragma once

#include "loudness.h"
#include "types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace mastered {

struct LimiterStats {
    double maxReductionDb = 0.0;
    double averageReductionDb = 0.0;
    double reducedFraction = 0.0;
};

class TruePeakLimiter {
public:
    TruePeakLimiter(double sampleRate, size_t channels, double ceilingDb, double lookaheadMs, double releaseMs);

    size_t latency() const { return latency_; }
    void process(AudioBlock& block, size_t frames);

    // Gain reduction statistics are only collected when requested, because they need a logarithm per sample.
    void collectStatistics(bool enabled) { collect_ = enabled; }
    LimiterStats statistics() const;

private:
    size_t channels_;
    const OversamplingKernel* kernel_ = nullptr;
    size_t lookahead_;
    size_t latency_;
    double ceiling_;
    double fastReleaseCoefficient_;
    double slowReleaseCoefficient_;
    double slowAttackCoefficient_;
    std::vector<std::vector<double>> history_;
    std::vector<size_t> hot_;
    size_t historyPosition_ = 0;
    double hotThreshold_ = 0.0;
    std::vector<std::vector<double>> delay_;
    size_t delayPosition_ = 0;
    std::deque<std::pair<uint64_t, double>> minimumWindow_;
    uint64_t counter_ = 0;
    double previousPeak_ = 0.0;
    double fastEnvelope_ = 1.0;
    double slowEnvelope_ = 1.0;
    std::vector<double> smoothing_;
    size_t smoothingPosition_ = 0;
    double smoothingSum_ = 0.0;
    bool collect_ = false;
    uint64_t statFrames_ = 0;
    uint64_t statReduced_ = 0;
    double statSumDb_ = 0.0;
    double statMaxDb_ = 0.0;
};

}
