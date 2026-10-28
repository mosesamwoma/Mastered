#include "chain.h"

#include "constants.h"
#include "error.h"

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

ProcessingChain::ProcessingChain(const ChainPlan& plan, double sampleRate, size_t channels)
    : plan_(plan),
      channels_(channels),
      useMidSide_(plan.midSide && channels == 2),
      gainLinear_(std::pow(10.0, plan.gainDb / 20.0)) {
    if (channels == 0 || channels > kMaxChannels) {
        throw MasteredError("Unsupported channel count");
    }
    if (useMidSide_) {
        midFilters_ = makeFilters(plan.midBands, sampleRate);
        sideFilters_ = makeFilters(plan.sideBands, sampleRate);
    } else {
        for (size_t ch = 0; ch < channels; ++ch) {
            channelFilters_.push_back(makeFilters(plan.midBands, sampleRate));
        }
    }
    if (plan.dynamics.enabled) {
        compressor_ = std::make_unique<MultibandCompressor>(plan.dynamics, sampleRate, channels);
    }
    if (plan.limiterEnabled) {
        const double ceilingLinear = std::pow(10.0, (plan.ceilingDb - kLimiterMarginDb) / 20.0);
        if (plan.softClip) {
            clipper_ = std::make_unique<SoftClipper>(ceilingLinear, plan.softClipKneeDb);
        }
        limiter_ = std::make_unique<TruePeakLimiter>(sampleRate, channels, plan.ceilingDb, plan.lookaheadMs, plan.releaseMs);
    }
}

size_t ProcessingChain::latency() const {
    return limiter_ ? limiter_->latency() : 0;
}

void ProcessingChain::process(AudioBlock& block, size_t frames) {
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
        for (size_t i = 0; i < frames; ++i) {
            const double s = side_[i] * plan_.sideGain;
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
    if (limiter_) {
        limiter_->process(block, frames);
    }
}

}
