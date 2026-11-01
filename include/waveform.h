#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mastered {

struct Waveform {
    // One value per column: the largest absolute sample and the RMS level, both linear.
    std::vector<float> peak;
    std::vector<float> rms;
};

// Condenses a stream of audio blocks into a fixed number of columns for drawing.
class WaveformBuilder {
public:
    WaveformBuilder(uint64_t totalFrames, size_t columns);

    void process(const AudioBlock& block, size_t frames);
    Waveform finish() const;

private:
    uint64_t total_;
    size_t columns_;
    uint64_t position_ = 0;
    std::vector<double> peak_;
    std::vector<double> power_;
    std::vector<uint64_t> count_;
};

}
