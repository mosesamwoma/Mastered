#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mastered {

struct Waveform {
    std::vector<float> peak;
    std::vector<float> rms;
};

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
