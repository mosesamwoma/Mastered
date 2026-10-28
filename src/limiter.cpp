#include "limiter.h"

#include "constants.h"
#include "error.h"
#include "loudness.h"

#include <algorithm>
#include <cmath>

namespace mastered {

TruePeakLimiter::TruePeakLimiter(double sampleRate, size_t channels, double ceilingDb, double lookaheadMs, double releaseMs)
    : channels_(channels) {
    if (channels == 0 || channels > kMaxChannels) {
        throw MasteredError("Unsupported channel count for limiter");
    }
    if (!(lookaheadMs > 0.0) || !(releaseMs > 0.0) || !(ceilingDb <= 0.0)) {
        throw MasteredError("Invalid limiter settings");
    }
    lookahead_ = std::max<size_t>(8, static_cast<size_t>(std::llround(lookaheadMs * 0.001 * sampleRate)));
    latency_ = lookahead_ + kTruePeakDelay;
    ceiling_ = std::pow(10.0, (ceilingDb - kLimiterMarginDb) / 20.0);
    releaseCoefficient_ = std::exp(-1.0 / (releaseMs * 0.001 * sampleRate));
    history_.assign(channels, std::vector<double>(2 * kTruePeakTaps, 0.0));
    hot_.assign(channels, 0);
    hotThreshold_ = ceiling_ / oversamplingKernel().bound * (1.0 - 1e-9);
    delay_.assign(channels, std::vector<double>(latency_, 0.0));
    smoothing_.assign(lookahead_ + 1, 1.0);
    smoothingSum_ = static_cast<double>(smoothing_.size());
}

void TruePeakLimiter::process(AudioBlock& block, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        double peak = 0.0;
        for (size_t ch = 0; ch < channels_; ++ch) {
            const double x = block[ch][i];
            std::vector<double>& h = history_[ch];
            h[historyPosition_] = x;
            h[historyPosition_ + kTruePeakTaps] = x;
            if (std::fabs(x) > hotThreshold_) {
                hot_[ch] = kTruePeakTaps;
            }
            if (hot_[ch] > 0) {
                --hot_[ch];
                peak = std::max(peak, truePeakOfWindow(h.data() + historyPosition_ + 1));
            }
        }
        historyPosition_ = (historyPosition_ + 1) % kTruePeakTaps;
        const double required = std::max(previousPeak_, peak);
        previousPeak_ = peak;
        const double ratio = required > ceiling_ ? ceiling_ / required : 1.0;

        while (!minimumWindow_.empty() && minimumWindow_.back().second >= ratio) {
            minimumWindow_.pop_back();
        }
        minimumWindow_.emplace_back(counter_, ratio);
        while (minimumWindow_.front().first + lookahead_ < counter_) {
            minimumWindow_.pop_front();
        }
        ++counter_;
        const double minimum = minimumWindow_.front().second;

        if (minimum < envelope_) {
            envelope_ = minimum;
        } else {
            envelope_ = releaseCoefficient_ * envelope_ + (1.0 - releaseCoefficient_) * minimum;
        }
        smoothingSum_ += envelope_ - smoothing_[smoothingPosition_];
        smoothing_[smoothingPosition_] = envelope_;
        smoothingPosition_ = (smoothingPosition_ + 1) % smoothing_.size();
        const double gain = smoothingSum_ / static_cast<double>(smoothing_.size());

        for (size_t ch = 0; ch < channels_; ++ch) {
            const double delayed = delay_[ch][delayPosition_];
            delay_[ch][delayPosition_] = block[ch][i];
            block[ch][i] = delayed * gain;
        }
        delayPosition_ = (delayPosition_ + 1) % latency_;
    }
}

}
