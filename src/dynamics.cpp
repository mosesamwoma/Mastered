#include "dynamics.h"

#include "constants.h"
#include "error.h"

#include <algorithm>
#include <cmath>

namespace mastered {

std::vector<double> defaultCrossovers(size_t bandCount, double sampleRate) {
    std::vector<double> wanted;
    switch (std::min<size_t>(std::max<size_t>(bandCount, 2), kMaxDynamicsBands)) {
        case 2: wanted = {400.0}; break;
        case 3: wanted = {150.0, 3000.0}; break;
        case 4: wanted = {120.0, 800.0, 5000.0}; break;
        default: wanted = {80.0, 300.0, 1800.0, 7000.0}; break;
    }
    std::vector<double> usable;
    for (double hz : wanted) {
        if (hz < sampleRate * 0.45) {
            usable.push_back(hz);
        }
    }
    return usable;
}

BandSplitter::BandSplitter(const std::vector<double>& crossoversHz, double sampleRate) {
    if (crossoversHz.empty() || crossoversHz.size() >= kMaxDynamicsBands) {
        throw MasteredError("Invalid crossover frequencies");
    }
    double previous = 0.0;
    for (double hz : crossoversHz) {
        if (!(hz > previous) || !(hz < sampleRate * 0.5)) {
            throw MasteredError("Invalid crossover frequencies");
        }
        previous = hz;
    }
    std::vector<BiquadCoefficients> lows;
    std::vector<BiquadCoefficients> highs;
    for (double hz : crossoversHz) {
        lows.push_back(designButterworth(false, hz, sampleRate));
        highs.push_back(designButterworth(true, hz, sampleRate));
    }
    stages_.resize(crossoversHz.size());
    for (size_t m = 0; m < stages_.size(); ++m) {
        stages_[m].configure(lows[m], highs[m]);
    }
    alignment_.resize(stages_.size());
    for (size_t k = 0; k + 1 < stages_.size(); ++k) {
        alignment_[k].resize(stages_.size() - 1 - k);
        for (size_t m = k + 1; m < stages_.size(); ++m) {
            alignment_[k][m - (k + 1)].configure(lows[m], highs[m]);
        }
    }
}

void BandSplitter::split(double x, double* out) {
    double rest = x;
    for (size_t m = 0; m < stages_.size(); ++m) {
        double low;
        double high;
        stages_[m].process(rest, low, high);
        out[m] = low;
        rest = high;
    }
    out[stages_.size()] = rest;
    for (size_t k = 0; k + 1 < stages_.size(); ++k) {
        for (LR4& stage : alignment_[k]) {
            double low;
            double high;
            stage.process(out[k], low, high);
            out[k] = low + high;
        }
    }
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

BandLevelMeter::BandLevelMeter(double sampleRate, const std::vector<double>& crossoversHz)
    : splitter_(crossoversHz, sampleRate),
      bands_(crossoversHz.size() + 1),
      blockFrames_(static_cast<size_t>(std::llround(sampleRate * 0.2))),
      blockSums_(bands_, 0.0),
      totalSums_(bands_, 0.0),
      levels_(bands_) {}

void BandLevelMeter::process(const AudioBlock& block, size_t frames) {
    const size_t channels = block.size();
    double bands[kMaxDynamicsBands];
    for (size_t i = 0; i < frames; ++i) {
        double x = 0.0;
        for (size_t ch = 0; ch < channels; ++ch) {
            x += block[ch][i];
        }
        x /= static_cast<double>(channels);
        splitter_.split(x, bands);
        for (size_t b = 0; b < bands_; ++b) {
            const double power = bands[b] * bands[b];
            blockSums_[b] += power;
            totalSums_[b] += power;
        }
        totalFrames_ += 1.0;
        if (++blockFilled_ == blockFrames_) {
            for (size_t b = 0; b < bands_; ++b) {
                levels_[b].push_back(10.0 * std::log10(std::max(blockSums_[b] / static_cast<double>(blockFrames_), 1e-12)));
                blockSums_[b] = 0.0;
            }
            blockFilled_ = 0;
        }
    }
}

std::vector<BandStatistics> BandLevelMeter::statistics() const {
    std::vector<BandStatistics> result(bands_);
    for (size_t b = 0; b < bands_; ++b) {
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
    : settings_(settings), channels_(channels), bands_(settings.bandCount) {
    if (channels == 0 || channels > kMaxChannels) {
        throw MasteredError("Unsupported channel count for dynamics processing");
    }
    if (bands_ < 2 || bands_ > kMaxDynamicsBands) {
        throw MasteredError("Unsupported number of dynamics bands");
    }
    for (size_t ch = 0; ch < channels; ++ch) {
        splitters_.emplace_back(settings.crossovers(), sampleRate);
    }
    for (size_t b = 0; b < bands_; ++b) {
        attack_[b] = std::exp(-1.0 / (std::max(settings.bands[b].attackMs, 0.01) * 0.001 * sampleRate));
        release_[b] = std::exp(-1.0 / (std::max(settings.bands[b].releaseMs, 0.01) * 0.001 * sampleRate));
    }
    static constexpr double kDetector2[2] = {20.0, 4.0};
    static constexpr double kDetector3[3] = {30.0, 10.0, 3.0};
    static constexpr double kDetector4[4] = {30.0, 15.0, 6.0, 3.0};
    static constexpr double kDetector5[5] = {30.0, 18.0, 10.0, 5.0, 2.5};
    const double* detectorMs = bands_ == 2 ? kDetector2 : (bands_ == 3 ? kDetector3 : (bands_ == 4 ? kDetector4 : kDetector5));
    for (size_t b = 0; b < bands_; ++b) {
        detector_[b] = std::exp(-1.0 / (detectorMs[b] * 0.001 * sampleRate));
    }
}

double MultibandCompressor::reductionDbFor(size_t band, double level) const {
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
    return reductionDb;
}

void MultibandCompressor::process(AudioBlock& block, size_t frames) {
    double bandSamples[kMaxChannels][kMaxDynamicsBands];
    for (size_t i = 0; i < frames; ++i) {
        double level[kMaxDynamicsBands] = {0.0, 0.0, 0.0, 0.0, 0.0};
        for (size_t ch = 0; ch < channels_; ++ch) {
            splitters_[ch].split(block[ch][i], bandSamples[ch]);
            for (size_t b = 0; b < bands_; ++b) {
                level[b] = std::max(level[b], bandSamples[ch][b] * bandSamples[ch][b]);
            }
        }
        double gain[kMaxDynamicsBands];
        for (size_t b = 0; b < bands_; ++b) {
            power_[b] = detector_[b] * power_[b] + (1.0 - detector_[b]) * level[b];
            const double target = reductionDbFor(b, std::sqrt(power_[b]));
            const double coefficient = target < reductionDb_[b] ? attack_[b] : release_[b];
            reductionDb_[b] = coefficient * reductionDb_[b] + (1.0 - coefficient) * target;
            gain[b] = std::pow(10.0, reductionDb_[b] / 20.0);
        }
        for (size_t ch = 0; ch < channels_; ++ch) {
            double sum = bandSamples[ch][0] * gain[0];
            for (size_t b = 1; b < bands_; ++b) {
                sum += bandSamples[ch][b] * gain[b];
            }
            block[ch][i] = sum;
        }
    }
}

bool deEsserBand(double sampleRate, double& lowHz, double& highHz) {
    lowHz = 4500.0;
    highHz = std::min(9500.0, sampleRate * 0.45);
    return highHz > lowHz * 1.3;
}

SibilanceMeter::SibilanceMeter(double sampleRate, double lowHz, double highHz)
    : highPass_(designButterworth(true, lowHz, sampleRate)),
      lowPass_(designButterworth(false, highHz, sampleRate)),
      blockFrames_(static_cast<size_t>(std::llround(sampleRate * 0.01))) {}

void SibilanceMeter::process(const AudioBlock& block, size_t frames) {
    const size_t channels = block.size();
    for (size_t i = 0; i < frames; ++i) {
        double x = 0.0;
        for (size_t ch = 0; ch < channels; ++ch) {
            x += block[ch][i];
        }
        x /= static_cast<double>(channels);
        const double band = lowPass_.process(highPass_.process(x));
        blockSum_ += band * band;
        if (++blockFilled_ == blockFrames_) {
            levels_.push_back(10.0 * std::log10(std::max(blockSum_ / static_cast<double>(blockFrames_), 1e-12)));
            blockSum_ = 0.0;
            blockFilled_ = 0;
        }
    }
}

BandStatistics SibilanceMeter::statistics() const {
    BandStatistics result;
    std::vector<double> audible;
    for (double level : levels_) {
        if (level > -80.0) {
            audible.push_back(level);
        }
    }
    if (audible.empty()) {
        return result;
    }
    std::sort(audible.begin(), audible.end());
    const double gate = percentileOf(audible, 0.95) - 30.0;
    std::vector<double> gated;
    for (double level : audible) {
        if (level >= gate) {
            gated.push_back(level);
        }
    }
    const std::vector<double>& used = gated.size() >= 5 ? gated : audible;
    result.p10Db = percentileOf(used, 0.10);
    result.p50Db = percentileOf(used, 0.50);
    result.p95Db = percentileOf(used, 0.95);
    return result;
}

DeEsser::DeEsser(const DeEsserSettings& settings, double sampleRate, size_t channels)
    : settings_(settings),
      channels_(channels),
      filters_(channels),
      detector_(std::exp(-1.0 / (0.005 * sampleRate))),
      attack_(std::exp(-1.0 / (std::max(settings.attackMs, 0.01) * 0.001 * sampleRate))),
      release_(std::exp(-1.0 / (std::max(settings.releaseMs, 0.01) * 0.001 * sampleRate))) {
    if (channels == 0 || channels > kMaxChannels) {
        throw MasteredError("Unsupported channel count for the de-esser");
    }
    for (Filters& f : filters_) {
        f.highPass.setCoefficients(designButterworth(true, settings.lowHz, sampleRate));
        f.lowPass.setCoefficients(designButterworth(false, settings.highHz, sampleRate));
    }
}

void DeEsser::process(AudioBlock& block, size_t frames) {
    double band[kMaxChannels];
    for (size_t i = 0; i < frames; ++i) {
        double level = 0.0;
        for (size_t ch = 0; ch < channels_; ++ch) {
            band[ch] = filters_[ch].lowPass.process(filters_[ch].highPass.process(block[ch][i]));
            level = std::max(level, band[ch] * band[ch]);
        }
        power_ = detector_ * power_ + (1.0 - detector_) * level;
        const double levelDb = 10.0 * std::log10(std::max(power_, 1e-12));
        const double over = levelDb - settings_.thresholdDb;
        double target = 0.0;
        if (settings_.kneeDb > 0.0 && 2.0 * std::fabs(over) <= settings_.kneeDb) {
            const double x = over + settings_.kneeDb / 2.0;
            target = (1.0 / settings_.ratio - 1.0) * x * x / (2.0 * settings_.kneeDb);
        } else if (over > 0.0) {
            target = over / settings_.ratio - over;
        }
        target = std::max(target, -settings_.maxReductionDb);
        const double coefficient = target < reductionDb_ ? attack_ : release_;
        reductionDb_ = coefficient * reductionDb_ + (1.0 - coefficient) * target;
        const double removed = 1.0 - std::pow(10.0, reductionDb_ / 20.0);
        if (removed != 0.0) {
            for (size_t ch = 0; ch < channels_; ++ch) {
                block[ch][i] -= removed * band[ch];
            }
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

namespace {

double besselI0(double x) {
    double sum = 1.0;
    double term = 1.0;
    const double q = x * x * 0.25;
    for (int k = 1; k < 60; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-17) {
            break;
        }
    }
    return sum;
}

}

OversampledClipper::OversampledClipper(double ceilingLinear, double kneeDb, size_t channels)
    : clipper_(ceilingLinear, kneeDb), gate_(clipper_.threshold() * 0.6), channels_(channels) {
    constexpr double kBeta = 8.0;
    const double centre = static_cast<double>(kLength - 1) / 2.0;
    double sum = 0.0;
    for (size_t n = 0; n < kLength; ++n) {
        const double offset = static_cast<double>(n) - centre;
        const double x = offset / static_cast<double>(kFactor);
        const double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
        const double r = offset / centre;
        const double window = besselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / besselI0(kBeta);
        filter_[n] = sinc * window;
        sum += filter_[n];
    }
    for (double& v : filter_) {
        v /= sum;
    }
    for (size_t p = 0; p < kFactor; ++p) {
        for (size_t m = 0; m <= kTaps; ++m) {
            const size_t index = kFactor * m + p;
            interpolation_[p][m] = index < kLength ? static_cast<double>(kFactor) * filter_[index] : 0.0;
        }
    }
}

double OversampledClipper::upsampled(const ChannelState& state, uint64_t time, size_t phase) const {
    double acc = 0.0;
    for (size_t m = 0; m <= kTaps; ++m) {
        acc += interpolation_[phase][m] * state.input[(time - m) & (kInputRing - 1)];
    }
    return acc;
}

void OversampledClipper::store(ChannelState& state, uint64_t time) const {
    for (size_t p = 0; p < kFactor; ++p) {
        state.clipped[(kFactor * time + p) & (kClipRing - 1)] = clipper_.apply(upsampled(state, time, p));
    }
}

void OversampledClipper::process(AudioBlock& block, size_t frames) {
    for (size_t ch = 0; ch < channels_.size(); ++ch) {
        ChannelState& state = channels_[ch];
        double* data = block[ch].data();
        for (size_t i = 0; i < frames; ++i) {
            const uint64_t time = position_ + i;
            const double x = data[i];
            state.input[time & (kInputRing - 1)] = x;
            if (std::fabs(x) > gate_) {
                state.hold = kHold;
                if (!state.active) {
                    for (uint64_t t = time - kLatency; t < time; ++t) {
                        store(state, t);
                    }
                    state.active = true;
                }
            }
            if (state.active) {
                store(state, time);
                const uint64_t k = kFactor * time;
                double acc = 0.0;
                for (size_t n = 0; n < kLength; ++n) {
                    acc += filter_[n] * state.clipped[(k - n) & (kClipRing - 1)];
                }
                data[i] = acc;
                if (--state.hold == 0) {
                    state.active = false;
                }
            } else {
                data[i] = state.input[(time - kLatency) & (kInputRing - 1)];
            }
        }
    }
    position_ += frames;
}

GlueCompressor::GlueCompressor(const GlueSettings& settings, double sampleRate, size_t channels)
    : settings_(settings), channels_(channels) {
    if (channels == 0 || channels > kMaxChannels) {
        throw MasteredError("Unsupported channel count for the glue compressor");
    }
    if (!(settings.ratio >= 1.0) || !(settings.attackMs > 0.0) || !(settings.releaseMs > 0.0) || !(settings.kneeDb >= 0.0)) {
        throw MasteredError("Invalid glue compressor settings");
    }
    detector_ = std::exp(-1.0 / (0.008 * sampleRate));
    attack_ = std::exp(-1.0 / (settings.attackMs * 0.001 * sampleRate));
    releaseFast_ = std::exp(-1.0 / (settings.releaseMs * 0.5 * 0.001 * sampleRate));
    releaseSlow_ = std::exp(-1.0 / (settings.releaseMs * 2.0 * 0.001 * sampleRate));
}

void GlueCompressor::process(AudioBlock& block, size_t frames) {
    const double threshold = settings_.thresholdDb;
    const double knee = settings_.kneeDb;
    const double slope = 1.0 / settings_.ratio - 1.0;
    for (size_t i = 0; i < frames; ++i) {
        double level = 0.0;
        for (size_t ch = 0; ch < channels_; ++ch) {
            const double x = block[ch][i];
            level = std::max(level, x * x);
        }
        power_ = detector_ * power_ + (1.0 - detector_) * level;
        const double levelDb = 10.0 * std::log10(std::max(power_, 1e-18)) + 3.0;
        const double over = levelDb - threshold;
        double target = 0.0;
        if (knee > 0.0 && 2.0 * std::fabs(over) <= knee) {
            const double x = over + knee / 2.0;
            target = slope * x * x / (2.0 * knee);
        } else if (over > 0.0) {
            target = slope * over;
        }
        double coefficient;
        if (target < reductionDb_) {
            coefficient = attack_;
        } else {
            const double mix = std::min(1.0, -reductionDb_ / 6.0);
            coefficient = releaseFast_ + (releaseSlow_ - releaseFast_) * mix;
        }
        reductionDb_ = coefficient * reductionDb_ + (1.0 - coefficient) * target;
        const double gain = std::pow(10.0, reductionDb_ / 20.0);
        for (size_t ch = 0; ch < channels_; ++ch) {
            block[ch][i] *= gain;
        }
    }
}

namespace {

BiquadCoefficients designBandPass(double frequency, double q, double sampleRate) {
    const double w0 = 2.0 * kPi * frequency / sampleRate;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    BiquadCoefficients c;
    c.b0 = alpha / a0;
    c.b1 = 0.0;
    c.b2 = -alpha / a0;
    c.a1 = -2.0 * std::cos(w0) / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}

constexpr double kResonanceNeighbourRatio = 1.41;
constexpr double kResonanceDetectorQ = 4.0;
constexpr double kResonanceEngageRatio = 0.8;

}

ResonanceTamer::ResonanceTamer(const ResonanceSettings& settings, double sampleRate, size_t channels)
    : settings_(settings), sampleRate_(sampleRate), channels_(channels) {
    if (channels == 0 || channels > kMaxChannels) {
        throw MasteredError("Unsupported channel count for the resonance tamer");
    }
    if (settings.count > kMaxResonanceBands) {
        throw MasteredError("Too many resonance bands");
    }
    detector_ = std::exp(-1.0 / (0.006 * sampleRate));
    attack_ = std::exp(-1.0 / (0.008 * sampleRate));
    release_ = std::exp(-1.0 / (0.12 * sampleRate));
    states_.resize(settings.count);
    for (size_t b = 0; b < settings.count; ++b) {
        const ResonanceBand& band = settings.bands[b];
        if (!(band.frequency * kResonanceNeighbourRatio < sampleRate * 0.48) || !(band.frequency > 50.0) || !(band.q > 0.0)) {
            throw MasteredError("Invalid resonance band");
        }
        const BiquadCoefficients centre = designBandPass(band.frequency, kResonanceDetectorQ, sampleRate);
        const BiquadCoefficients low = designBandPass(band.frequency / kResonanceNeighbourRatio, kResonanceDetectorQ, sampleRate);
        const BiquadCoefficients high = designBandPass(band.frequency * kResonanceNeighbourRatio, kResonanceDetectorQ, sampleRate);
        states_[b].detectors.resize(channels);
        for (Detector& detector : states_[b].detectors) {
            detector.centre.setCoefficients(centre);
            detector.low.setCoefficients(low);
            detector.high.setCoefficients(high);
        }
        states_[b].tone.assign(channels, Biquad());
    }
}

void ResonanceTamer::process(AudioBlock& block, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        ++counter_;
        for (size_t b = 0; b < states_.size(); ++b) {
            BandState& state = states_[b];
            const ResonanceBand& band = settings_.bands[b];
            double centrePower = 0.0;
            double neighbourPower = 0.0;
            for (size_t ch = 0; ch < channels_; ++ch) {
                const double x = block[ch][i];
                Detector& detector = state.detectors[ch];
                const double c = detector.centre.process(x);
                const double l = detector.low.process(x);
                const double h = detector.high.process(x);
                centrePower = std::max(centrePower, c * c);
                neighbourPower = std::max(neighbourPower, 0.5 * (l * l + h * h));
            }
            state.centrePower = detector_ * state.centrePower + (1.0 - detector_) * centrePower;
            state.neighbourPower = detector_ * state.neighbourPower + (1.0 - detector_) * neighbourPower;
            double target = 0.0;
            if (state.neighbourPower > 1e-14 && state.centrePower > 1e-14) {
                const double excess = 10.0 * std::log10(state.centrePower / state.neighbourPower);
                target = std::min(std::max((excess - band.thresholdDb) * kResonanceEngageRatio, 0.0), band.maxCutDb);
            }
            const double coefficient = target > state.cutDb ? attack_ : release_;
            state.cutDb = coefficient * state.cutDb + (1.0 - coefficient) * target;
            if ((counter_ & 15) == 0 && std::fabs(state.cutDb - state.appliedDb) > 0.05) {
                EQBand eq;
                eq.frequency = band.frequency;
                eq.gainDb = -state.cutDb;
                eq.q = band.q;
                eq.type = BandType::Peak;
                const BiquadCoefficients tone = designBand(eq, sampleRate_);
                for (Biquad& filter : state.tone) {
                    filter.setCoefficients(tone);
                }
                state.appliedDb = state.cutDb;
            }
            for (size_t ch = 0; ch < channels_; ++ch) {
                block[ch][i] = state.tone[ch].process(block[ch][i]);
            }
        }
    }
}

}
