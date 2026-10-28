#pragma once

#include "constants.h"
#include "types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace mastered {

class TruePeakLimiter {
public:
    TruePeakLimiter(double sampleRate, size_t channels, double ceilingDb, double lookaheadMs, double releaseMs);

    size_t latency() const { return latency_; }
    void process(AudioBlock& block, size_t frames);

private:
    size_t channels_;
    size_t lookahead_;
    size_t latency_;
    double ceiling_;
    double releaseCoefficient_;
    std::vector<std::vector<double>> history_;
    std::vector<size_t> hot_;
    size_t historyPosition_ = 0;
    double hotThreshold_ = 0.0;
    std::vector<std::vector<double>> delay_;
    size_t delayPosition_ = 0;
    std::deque<std::pair<uint64_t, double>> minimumWindow_;
    uint64_t counter_ = 0;
    double previousPeak_ = 0.0;
    double envelope_ = 1.0;
    std::vector<double> smoothing_;
    size_t smoothingPosition_ = 0;
    double smoothingSum_ = 0.0;
};

}
