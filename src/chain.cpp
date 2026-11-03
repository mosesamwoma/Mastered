#include "chain.h"

#include "constants.h"
#include "error.h"

#include <algorithm>
#include <cmath>

namespace mastered {

namespace {

std::vector<Biquad> makeFilters(const std::vector<EQBand>& bands, double sampleRate) {
    std::vector<Biquad> filters;
    filters.reserve(bands.size());
    for (const EQBand& band : bands) {
        filters.emplace_back(designBand(band, sampleRate));
    }
    return filters;
}

}

ProcessingChain::ProcessingChain(const ChainPlan& plan, double sampleRate, size_t channels, ChainStage stage)
    : plan_(plan),
      channels_(channels),
      stage_(stage),
      useMidSide_((plan.midSide || plan.monoBassHz > 0.0) && channels == 2),
      gainLinear_(std::pow(10.0, plan.gainDb / 20.0)) {
    if (channels == 0 || channels > kMaxChannels) {
        throw MasteredError("Unsupported channel count");
    }
    if (stage != ChainStage::Post) {
        if (useMidSide_) {
            midFilters_ = makeFilters(plan.midBands, sampleRate);
            sideFilters_ = makeFilters(plan.midSide ? plan.sideBands : plan.midBands, sampleRate);
            if (plan.monoBassHz > 0.0) {
                const BiquadCoefficients highPass = designButterworth(true, plan.monoBassHz, sampleRate);
                sideHighPass_.assign(2, Biquad(highPass));
            }
        } else {
            for (size_t ch = 0; ch < channels; ++ch) {
                channelFilters_.push_back(makeFilters(plan.midBands, sampleRate));
            }
        }
        if (plan.dynamics.enabled) {
            compressor_ = std::make_unique<MultibandCompressor>(plan.dynamics, sampleRate, channels);
        }
    }
    if (stage != ChainStage::Pre && plan.limiterEnabled) {
        const double ceilingLinear = std::pow(10.0, (plan.ceilingDb - kLimiterMarginDb) / 20.0);
        if (plan.softClip) {
            clipper_ = std::make_unique<SoftClipper>(ceilingLinear, plan.softClipKneeDb);
        }
        if (plan.bassLimiter && plan.bassLimiterHz * 2.0 < sampleRate * 0.45) {
            const BiquadCoefficients lowPass = designButterworth(false, plan.bassLimiterHz, sampleRate);
            const BiquadCoefficients highPass = designButterworth(true, plan.bassLimiterHz, sampleRate);
            bassSplit_.resize(channels);
            for (BassSplit& split : bassSplit_) {
                split.lowA.setCoefficients(lowPass);
                split.lowB.setCoefficients(lowPass);
                split.highA.setCoefficients(highPass);
                split.highB.setCoefficients(highPass);
            }
            const double bassRelease = std::min(std::max(plan.releaseMs * 2.0, 150.0), 1000.0);
            bassLimiter_ = std::make_unique<TruePeakLimiter>(sampleRate, channels, plan.ceilingDb - plan.bassCeilingOffsetDb,
                                                             std::max(plan.lookaheadMs * 2.0, 8.0), bassRelease);
            bassDelay_.assign(channels, std::vector<double>(bassLimiter_->latency(), 0.0));
        }
        limiter_ = std::make_unique<TruePeakLimiter>(sampleRate, channels, plan.ceilingDb, plan.lookaheadMs, plan.releaseMs);
    }
}

size_t ProcessingChain::latency() const {
    size_t total = 0;
    if (bassLimiter_) {
        total += bassLimiter_->latency();
    }
    if (limiter_) {
        total += limiter_->latency();
    }
    return total;
}

void ProcessingChain::collectLimiterStatistics(bool enabled) {
    if (limiter_) {
        limiter_->collectStatistics(enabled);
    }
}

LimiterStats ProcessingChain::limiterStatistics() const {
    return limiter_ ? limiter_->statistics() : LimiterStats();
}

void ProcessingChain::process(AudioBlock& block, size_t frames) {
    processPre(block, frames);
    processPost(block, frames);
}

void ProcessingChain::processPre(AudioBlock& block, size_t frames) {
    if (stage_ == ChainStage::Post) {
        throw MasteredError("Internal error: the processing chain was built for the post stage only");
    }
    if (useMidSide_) {
        mid_.resize(frames);
        side_.resize(frames);
        for (size_t i = 0; i < frames; ++i) {
            mid_[i] = 0.5 * (block[0][i] + block[1][i]);
            side_[i] = 0.5 * (block[0][i] - block[1][i]);
        }
        for (Biquad& filter : midFilters_) {
            filter.processBlock(mid_.data(), frames);
        }
        for (Biquad& filter : sideFilters_) {
            filter.processBlock(side_.data(), frames);
        }
        for (Biquad& filter : sideHighPass_) {
            filter.processBlock(side_.data(), frames);
        }
        const double sideGain = plan_.midSide ? plan_.sideGain : 1.0;
        for (size_t i = 0; i < frames; ++i) {
            const double s = side_[i] * sideGain;
            block[0][i] = mid_[i] + s;
            block[1][i] = mid_[i] - s;
        }
    } else {
        for (size_t ch = 0; ch < channels_; ++ch) {
            for (Biquad& filter : channelFilters_[ch]) {
                filter.processBlock(block[ch].data(), frames);
            }
        }
    }
    if (compressor_) {
        compressor_->process(block, frames);
    }
}

void ProcessingChain::processPost(AudioBlock& block, size_t frames) {
    if (stage_ == ChainStage::Pre) {
        throw MasteredError("Internal error: the processing chain was built for the pre stage only");
    }
    if (gainLinear_ != 1.0) {
        for (size_t ch = 0; ch < channels_; ++ch) {
            for (size_t i = 0; i < frames; ++i) {
                block[ch][i] *= gainLinear_;
            }
        }
    }
    if (clipper_) {
        for (size_t ch = 0; ch < channels_; ++ch) {
            for (size_t i = 0; i < frames; ++i) {
                block[ch][i] = clipper_->apply(block[ch][i]);
            }
        }
    }
    if (bassLimiter_) {
        bassLow_.resize(channels_);
        bassHigh_.resize(channels_);
        for (size_t ch = 0; ch < channels_; ++ch) {
            bassLow_[ch].resize(frames);
            bassHigh_[ch].resize(frames);
            BassSplit& split = bassSplit_[ch];
            for (size_t i = 0; i < frames; ++i) {
                const double x = block[ch][i];
                bassLow_[ch][i] = split.lowB.process(split.lowA.process(x));
                bassHigh_[ch][i] = split.highB.process(split.highA.process(x));
            }
        }
        bassLimiter_->process(bassLow_, frames);
        const size_t delayLength = bassDelay_[0].size();
        for (size_t i = 0; i < frames; ++i) {
            for (size_t ch = 0; ch < channels_; ++ch) {
                const double delayedHigh = bassDelay_[ch][bassDelayPosition_];
                bassDelay_[ch][bassDelayPosition_] = bassHigh_[ch][i];
                block[ch][i] = bassLow_[ch][i] + delayedHigh;
            }
            bassDelayPosition_ = (bassDelayPosition_ + 1) % delayLength;
        }
    }
    if (limiter_) {
        limiter_->process(block, frames);
    }
}

}
