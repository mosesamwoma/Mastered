#include "waveform.h"

#include <algorithm>
#include <cmath>

namespace mastered {

WaveformBuilder::WaveformBuilder(uint64_t totalFrames, size_t columns)
    : total_(std::max<uint64_t>(totalFrames, 1)),
      columns_(static_cast<size_t>(std::max<uint64_t>(1, std::min<uint64_t>(columns, std::max<uint64_t>(totalFrames, 1))))),
      peak_(columns_, 0.0),
      power_(columns_, 0.0),
      count_(columns_, 0) {}

void WaveformBuilder::process(const AudioBlock& block, size_t frames) {
    const size_t channels = block.size();
    if (channels == 0) {
        return;
    }
    for (size_t i = 0; i < frames; ++i) {
        size_t column = static_cast<size_t>((position_ * static_cast<uint64_t>(columns_)) / total_);
        if (column >= columns_) {
            column = columns_ - 1;
        }
        double loudest = 0.0;
        double power = 0.0;
        for (size_t ch = 0; ch < channels; ++ch) {
            const double x = block[ch][i];
            loudest = std::max(loudest, std::fabs(x));
            power += x * x;
        }
        peak_[column] = std::max(peak_[column], loudest);
        power_[column] += power / static_cast<double>(channels);
        ++count_[column];
        ++position_;
    }
}

Waveform WaveformBuilder::finish() const {
    Waveform result;
    result.peak.resize(columns_);
    result.rms.resize(columns_);
    for (size_t c = 0; c < columns_; ++c) {
        result.peak[c] = static_cast<float>(peak_[c]);
        result.rms[c] = count_[c] > 0 ? static_cast<float>(std::sqrt(power_[c] / static_cast<double>(count_[c]))) : 0.0f;
    }
    return result;
}

}
