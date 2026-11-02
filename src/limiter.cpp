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
    kernel_ = &standardKernel();
    latency_ = lookahead_ + kernel_->delay;
    ceiling_ = std::pow(10.0, (ceilingDb - kLimiterMarginDb) / 20.0);
    // Program-dependent release. A fast follower recovers quickly after an isolated peak, while a slow follower
    // tracks how deep and how long the limiting has been; the smaller of the two is used. A single kick therefore
    // lets go quickly, whereas sustained limiting recovers over the full release time. The fast time is kept
    // above one cycle of low bass so that the gain does not follow the waveform of a bass note.
    const double fastReleaseMs = std::min(std::max(releaseMs * 0.4, 20.0), 120.0);
    fastReleaseCoefficient_ = std::exp(-1.0 / (std::min(fastReleaseMs, releaseMs) * 0.001 * sampleRate));
    slowReleaseCoefficient_ = std::exp(-1.0 / (releaseMs * 0.001 * sampleRate));
    slowAttackCoefficient_ = std::exp(-1.0 / (std::max(releaseMs * 0.25, 5.0) * 0.001 * sampleRate));
    history_.assign(channels, std::vector<double>(2 * kernel_->tapsPerPhase, 0.0));
    hot_.assign(channels, 0);
    hotThreshold_ = ceiling_ / kernel_->bound * (1.0 - 1e-9);
    delay_.assign(channels, std::vector<double>(latency_, 0.0));
    smoothing_.assign(lookahead_ + 1, 1.0);
    smoothingSum_ = static_cast<double>(smoothing_.size());
}

void TruePeakLimiter::process(AudioBlock& block, size_t frames) {
    const size_t taps = kernel_->tapsPerPhase;
    for (size_t i = 0; i < frames; ++i) {
        double peak = 0.0;
        for (size_t ch = 0; ch < channels_; ++ch) {
            const double x = block[ch][i];
            std::vector<double>& h = history_[ch];
            h[historyPosition_] = x;
            h[historyPosition_ + taps] = x;
            if (std::fabs(x) > hotThreshold_) {
                hot_[ch] = taps;
            }
            if (hot_[ch] > 0) {
                --hot_[ch];
                peak = std::max(peak, truePeakOfWindow(h.data() + historyPosition_ + 1, *kernel_));
            }
        }
        historyPosition_ = (historyPosition_ + 1) % taps;
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

        if (minimum < fastEnvelope_) {
            fastEnvelope_ = minimum;
        } else {
            fastEnvelope_ = fastReleaseCoefficient_ * fastEnvelope_ + (1.0 - fastReleaseCoefficient_) * minimum;
        }
        const double slowCoefficient = fastEnvelope_ < slowEnvelope_ ? slowAttackCoefficient_ : slowReleaseCoefficient_;
        slowEnvelope_ = slowCoefficient * slowEnvelope_ + (1.0 - slowCoefficient) * fastEnvelope_;
        const double envelope = std::min(fastEnvelope_, slowEnvelope_);
        smoothingSum_ += envelope - smoothing_[smoothingPosition_];
        smoothing_[smoothingPosition_] = envelope;
        smoothingPosition_ = (smoothingPosition_ + 1) % smoothing_.size();
        const double gain = smoothingSum_ / static_cast<double>(smoothing_.size());
        if (collect_) {
            ++statFrames_;
            if (gain < 0.9999) {
                const double reduction = -20.0 * std::log10(std::max(gain, 1e-6));
                ++statReduced_;
                statSumDb_ += reduction;
                statMaxDb_ = std::max(statMaxDb_, reduction);
            }
        }

        for (size_t ch = 0; ch < channels_; ++ch) {
            const double delayed = delay_[ch][delayPosition_];
            delay_[ch][delayPosition_] = block[ch][i];
            block[ch][i] = delayed * gain;
        }
        delayPosition_ = (delayPosition_ + 1) % latency_;
    }
}

LimiterStats TruePeakLimiter::statistics() const {
    LimiterStats stats;
    if (statFrames_ > 0) {
        stats.maxReductionDb = statMaxDb_;
        stats.averageReductionDb = statSumDb_ / static_cast<double>(statFrames_);
        stats.reducedFraction = static_cast<double>(statReduced_) / static_cast<double>(statFrames_);
    }
    return stats;
}

}
