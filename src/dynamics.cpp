#include "dynamics.h"

#include "constants.h"
#include "error.h"

#include <algorithm>
#include <cmath>

namespace mastered {

ThreeBandSplitter::ThreeBandSplitter(double lowHz, double highHz, double sampleRate) {
    if (!(lowHz > 0.0) || !(highHz > lowHz) || !(highHz < sampleRate * 0.5)) {
        throw MasteredError("Invalid crossover frequencies");
    }
    const BiquadCoefficients lowLow = designButterworth(false, lowHz, sampleRate);
    const BiquadCoefficients lowHigh = designButterworth(true, lowHz, sampleRate);
    const BiquadCoefficients highLow = designButterworth(false, highHz, sampleRate);
    const BiquadCoefficients highHigh = designButterworth(true, highHz, sampleRate);
    first_.configure(lowLow, lowHigh);
    second_.configure(highLow, highHigh);
    align_.configure(highLow, highHigh);
}

void ThreeBandSplitter::split(double x, double* out) {
    double low;
    double rest;
    first_.process(x, low, rest);
    double mid;
    double high;
    second_.process(rest, mid, high);
    double alignedLow;
    double alignedHigh;
    align_.process(low, alignedLow, alignedHigh);
    out[0] = alignedLow + alignedHigh;
    out[1] = mid;
    out[2] = high;
}

namespace {

double percentileOf(const std::vector<double>& sorted, double fraction) {
    const double position = fraction * static_cast<double>(sorted.size() - 1);
    const size_t lower = static_cast<size_t>(std::floor(position));
    const size_t upper = std::min(lower + 1, sorted.size() - 1);
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

}

BandLevelMeter::BandLevelMeter(double sampleRate, double lowHz, double highHz)
    : splitter_(lowHz, highHz, sampleRate), blockFrames_(static_cast<size_t>(std::llround(sampleRate * 0.2))) {}

void BandLevelMeter::process(const AudioBlock& block, size_t frames) {
    const size_t channels = block.size();
    for (size_t i = 0; i < frames; ++i) {
        double x = 0.0;
        for (size_t ch = 0; ch < channels; ++ch) {
            x += block[ch][i];
        }
        x /= static_cast<double>(channels);
        double bands[3];
        splitter_.split(x, bands);
        for (size_t b = 0; b < 3; ++b) {
            const double power = bands[b] * bands[b];
            blockSums_[b] += power;
            totalSums_[b] += power;
        }
        totalFrames_ += 1.0;
        if (++blockFilled_ == blockFrames_) {
            for (size_t b = 0; b < 3; ++b) {
                levels_[b].push_back(10.0 * std::log10(std::max(blockSums_[b] / static_cast<double>(blockFrames_), 1e-12)));
                blockSums_[b] = 0.0;
            }
            blockFilled_ = 0;
        }
    }
}

std::array<BandStatistics, 3> BandLevelMeter::statistics() const {
    std::array<BandStatistics, 3> result;
    for (size_t b = 0; b < 3; ++b) {
        if (totalFrames_ > 0.0) {
            result[b].meanDb = std::max(-90.0, 10.0 * std::log10(std::max(totalSums_[b] / totalFrames_, 1e-12)));
        }
        std::vector<double> audible;
        for (double level : levels_[b]) {
            if (level > -80.0) {
                audible.push_back(level);
            }
        }
        if (audible.empty()) {
            continue;
        }
        std::sort(audible.begin(), audible.end());
        const double gate = percentileOf(audible, 0.95) - 40.0;
        std::vector<double> gated;
        for (double level : audible) {
            if (level >= gate) {
                gated.push_back(level);
            }
        }
        const std::vector<double>& used = gated.size() >= 5 ? gated : audible;
        result[b].p10Db = percentileOf(used, 0.10);
        result[b].p50Db = percentileOf(used, 0.50);
        result[b].p95Db = percentileOf(used, 0.95);
    }
    return result;
}

MultibandCompressor::MultibandCompressor(const DynamicsSettings& settings, double sampleRate, size_t channels)
    : settings_(settings), channels_(channels) {
    if (channels == 0 || channels > kMaxChannels) {
        throw MasteredError("Unsupported channel count for dynamics processing");
    }
    for (size_t ch = 0; ch < channels; ++ch) {
        splitters_.emplace_back(settings.lowCrossoverHz, settings.highCrossoverHz, sampleRate);
    }
    for (size_t b = 0; b < 3; ++b) {
        attack_[b] = std::exp(-1.0 / (std::max(settings.bands[b].attackMs, 0.01) * 0.001 * sampleRate));
        release_[b] = std::exp(-1.0 / (std::max(settings.bands[b].releaseMs, 0.01) * 0.001 * sampleRate));
    }
}

double MultibandCompressor::gainFor(size_t band, double level) const {
    const CompressorBand& s = settings_.bands[band];
    const double levelDb = 20.0 * std::log10(std::max(level, 1e-9));
    const double over = levelDb - s.thresholdDb;
    double reductionDb = 0.0;
    if (s.kneeDb > 0.0 && 2.0 * std::fabs(over) <= s.kneeDb) {
        const double x = over + s.kneeDb / 2.0;
        reductionDb = (1.0 / s.ratio - 1.0) * x * x / (2.0 * s.kneeDb);
    } else if (over > 0.0) {
        reductionDb = over / s.ratio - over;
    }
    return std::pow(10.0, reductionDb / 20.0);
}

void MultibandCompressor::process(AudioBlock& block, size_t frames) {
    double bandSamples[kMaxChannels][3];
    for (size_t i = 0; i < frames; ++i) {
        double level[3] = {0.0, 0.0, 0.0};
        for (size_t ch = 0; ch < channels_; ++ch) {
            splitters_[ch].split(block[ch][i], bandSamples[ch]);
            for (size_t b = 0; b < 3; ++b) {
                level[b] = std::max(level[b], bandSamples[ch][b] * bandSamples[ch][b]);
            }
        }
        double gain[3];
        for (size_t b = 0; b < 3; ++b) {
            const double coefficient = level[b] > power_[b] ? attack_[b] : release_[b];
            power_[b] = coefficient * power_[b] + (1.0 - coefficient) * level[b];
            gain[b] = gainFor(b, std::sqrt(power_[b]));
        }
        for (size_t ch = 0; ch < channels_; ++ch) {
            block[ch][i] = bandSamples[ch][0] * gain[0] + bandSamples[ch][1] * gain[1] + bandSamples[ch][2] * gain[2];
        }
    }
}

SoftClipper::SoftClipper(double ceilingLinear, double kneeDb)
    : ceiling_(ceilingLinear), threshold_(ceilingLinear * std::pow(10.0, -std::max(kneeDb, 0.1) / 20.0)) {}

double SoftClipper::apply(double x) const {
    const double a = std::fabs(x);
    if (a <= threshold_) {
        return x;
    }
    const double span = ceiling_ - threshold_;
    const double y = threshold_ + span * std::tanh((a - threshold_) / span);
    return x < 0.0 ? -y : y;
}

}
