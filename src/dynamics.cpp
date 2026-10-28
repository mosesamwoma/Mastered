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

BandLevelMeter::BandLevelMeter(double sampleRate, double lowHz, double highHz) : splitter_(lowHz, highHz, sampleRate) {}

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
            sums_[b] += bands[b] * bands[b];
        }
        count_ += 1.0;
    }
}

std::array<double, 3> BandLevelMeter::rmsDb() const {
    std::array<double, 3> out{{-90.0, -90.0, -90.0}};
    if (count_ <= 0.0) {
        return out;
    }
    for (size_t b = 0; b < 3; ++b) {
        out[b] = std::max(-90.0, 10.0 * std::log10(std::max(sums_[b] / count_, 1e-12)));
    }
    return out;
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
                level[b] = std::max(level[b], std::fabs(bandSamples[ch][b]));
            }
        }
        double gain[3];
        for (size_t b = 0; b < 3; ++b) {
            const double coefficient = level[b] > envelope_[b] ? attack_[b] : release_[b];
            envelope_[b] = coefficient * envelope_[b] + (1.0 - coefficient) * level[b];
            gain[b] = gainFor(b, envelope_[b]);
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
