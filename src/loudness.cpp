#include "loudness.h"

#include "constants.h"
#include "error.h"

#include <algorithm>
#include <cmath>

namespace mastered {

namespace {

double besselI0(double x) {
    double sum = 1.0;
    double term = 1.0;
    const double q = x * x / 4.0;
    for (int k = 1; k < 64; ++k) {
        term *= q / (static_cast<double>(k) * static_cast<double>(k));
        sum += term;
        if (term < sum * 1e-18) {
            break;
        }
    }
    return sum;
}

OversamplingKernel buildKernel(size_t factor, size_t tapsPerPhase, double beta) {
    const size_t taps = factor * tapsPerPhase;
    const long centre = static_cast<long>(taps / 2);
    const long step = static_cast<long>(factor);
    const double norm = besselI0(beta);
    std::vector<double> h(taps, 0.0);
    for (size_t n = 0; n < taps; ++n) {
        const long offset = static_cast<long>(n) - centre;
        if (offset % step == 0) {
            h[n] = offset == 0 ? 1.0 : 0.0;
            continue;
        }
        const double x = static_cast<double>(offset) / static_cast<double>(factor);
        const double sinc = std::sin(kPi * x) / (kPi * x);
        const double r = static_cast<double>(offset) / static_cast<double>(centre);
        const double window = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / norm;
        h[n] = sinc * window;
    }
    OversamplingKernel kernel;
    kernel.factor = factor;
    kernel.tapsPerPhase = tapsPerPhase;
    kernel.delay = tapsPerPhase / 2;
    kernel.firstPhase = 1;
    kernel.bound = 1.0;
    kernel.coefficients.assign(taps, 0.0);
    for (size_t p = 0; p < factor; ++p) {
        double sum = 0.0;
        for (size_t m = 0; m < tapsPerPhase; ++m) {
            sum += h[factor * m + p];
        }
        double absolute = 0.0;
        for (size_t m = 0; m < tapsPerPhase; ++m) {
            const double value = h[factor * m + p] / sum;
            kernel.coefficients[p * tapsPerPhase + (tapsPerPhase - 1 - m)] = value;
            absolute += std::fabs(value);
        }
        kernel.bound = std::max(kernel.bound, absolute);
    }
    return kernel;
}

BiquadCoefficients kShelf(double sampleRate) {
    const double f0 = 1681.974450955533;
    const double gain = 3.999843853973347;
    const double q = 0.7071752369554196;
    const double k = std::tan(kPi * f0 / sampleRate);
    const double vh = std::pow(10.0, gain / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;
    BiquadCoefficients c;
    c.b0 = (vh + vb * k / q + k * k) / a0;
    c.b1 = 2.0 * (k * k - vh) / a0;
    c.b2 = (vh - vb * k / q + k * k) / a0;
    c.a1 = 2.0 * (k * k - 1.0) / a0;
    c.a2 = (1.0 - k / q + k * k) / a0;
    return c;
}

BiquadCoefficients kHighPass(double sampleRate) {
    const double f0 = 38.13547087602444;
    const double q = 0.5003270373238773;
    const double k = std::tan(kPi * f0 / sampleRate);
    const double a0 = 1.0 + k / q + k * k;
    BiquadCoefficients c;
    c.b0 = 1.0;
    c.b1 = -2.0;
    c.b2 = 1.0;
    c.a1 = 2.0 * (k * k - 1.0) / a0;
    c.a2 = (1.0 - k / q + k * k) / a0;
    return c;
}

double powerToLufs(double power) {
    return -0.691 + 10.0 * std::log10(std::max(power, 1e-12));
}

double percentile(const std::vector<double>& sorted, double fraction) {
    const double position = fraction * static_cast<double>(sorted.size() - 1);
    const size_t lower = static_cast<size_t>(std::floor(position));
    const size_t upper = std::min(lower + 1, sorted.size() - 1);
    const double weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

}

const OversamplingKernel& standardKernel() {
    static const OversamplingKernel kernel = buildKernel(4, 24, 6.0);
    return kernel;
}

const OversamplingKernel& accurateKernel() {
    static const OversamplingKernel kernel = buildKernel(8, 64, 5.0);
    return kernel;
}

double truePeakOfWindow(const double* window, const OversamplingKernel& kernel) {
    const size_t taps = kernel.tapsPerPhase;
    double peak = std::fabs(window[taps - 1 - kernel.delay]);
    for (size_t p = kernel.firstPhase; p < kernel.factor; ++p) {
        const double* coefficients = kernel.phase(p);
        double sum = 0.0;
        for (size_t j = 0; j < taps; ++j) {
            sum += coefficients[j] * window[j];
        }
        peak = std::max(peak, std::fabs(sum));
    }
    return peak;
}

double channelWeight(size_t index, size_t channels) {
    if (channels >= 6 && index == 3) {
        return 0.0;
    }
    if (index >= 4) {
        return 1.41253754462275;
    }
    if (channels == 4 && index >= 2) {
        return 1.41253754462275;
    }
    return 1.0;
}

LoudnessMeter::LoudnessMeter(double sampleRate, size_t channels)
    : channels_(channels),
      hop_(static_cast<size_t>(std::llround(sampleRate * 0.1))),
      filters_(channels),
      weights_(channels) {
    if (channels == 0 || channels > kMaxChannels) {
        throw MasteredError("Unsupported channel count for loudness measurement");
    }
    const BiquadCoefficients shelf = kShelf(sampleRate);
    const BiquadCoefficients highPass = kHighPass(sampleRate);
    for (size_t ch = 0; ch < channels; ++ch) {
        filters_[ch].shelf.setCoefficients(shelf);
        filters_[ch].highPass.setCoefficients(highPass);
        weights_[ch] = channelWeight(ch, channels);
    }
}

void LoudnessMeter::process(const AudioBlock& block, size_t frames) {
    for (size_t i = 0; i < frames; ++i) {
        for (size_t ch = 0; ch < channels_; ++ch) {
            if (weights_[ch] == 0.0) {
                continue;
            }
            const double y = filters_[ch].process(block[ch][i]);
            accumulator_ += weights_[ch] * y * y;
        }
        if (++accumulated_ == hop_) {
            subBlocks_.push_back(accumulator_ / static_cast<double>(hop_));
            accumulator_ = 0.0;
            accumulated_ = 0;
        }
    }
}

LoudnessResult LoudnessMeter::finish() const {
    LoudnessResult result;
    const size_t n = subBlocks_.size();
    if (n < 4) {
        return result;
    }
    std::vector<double> prefix(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) {
        prefix[i + 1] = prefix[i] + subBlocks_[i];
    }

    std::vector<double> momentary;
    momentary.reserve(n);
    for (size_t j = 3; j < n; ++j) {
        momentary.push_back((prefix[j + 1] - prefix[j - 3]) / 4.0);
    }
    std::vector<double> gated;
    double sum = 0.0;
    for (double z : momentary) {
        if (powerToLufs(z) >= -70.0) {
            gated.push_back(z);
            sum += z;
        }
    }
    if (gated.empty()) {
        return result;
    }
    const double relativeGate = powerToLufs(sum / static_cast<double>(gated.size())) - 10.0;
    double finalSum = 0.0;
    size_t finalCount = 0;
    for (double z : gated) {
        if (powerToLufs(z) >= relativeGate) {
            finalSum += z;
            ++finalCount;
        }
    }
    if (finalCount == 0) {
        return result;
    }
    result.integratedLufs = powerToLufs(finalSum / static_cast<double>(finalCount));

    if (n >= 30) {
        std::vector<double> shortTerm;
        shortTerm.reserve(n);
        for (size_t j = 29; j < n; ++j) {
            shortTerm.push_back((prefix[j + 1] - prefix[j - 29]) / 30.0);
        }
        std::vector<double> absolute;
        double absoluteSum = 0.0;
        for (double z : shortTerm) {
            if (powerToLufs(z) >= -70.0) {
                absolute.push_back(z);
                absoluteSum += z;
            }
        }
        if (!absolute.empty()) {
            const double gate = powerToLufs(absoluteSum / static_cast<double>(absolute.size())) - 20.0;
            std::vector<double> levels;
            for (double z : absolute) {
                const double l = powerToLufs(z);
                if (l >= gate) {
                    levels.push_back(l);
                }
            }
            if (levels.size() >= 2) {
                std::sort(levels.begin(), levels.end());
                result.loudnessRangeLu = percentile(levels, 0.95) - percentile(levels, 0.10);
            }
        }
    }
    return result;
}

TruePeakMeter::TruePeakMeter(size_t channels, const OversamplingKernel& kernel)
    : channels_(channels),
      kernel_(kernel),
      history_(channels, std::vector<double>(kernel.tapsPerPhase - 1, 0.0)),
      hot_(channels, 0) {}

void TruePeakMeter::process(const AudioBlock& block, size_t frames) {
    const size_t history = kernel_.tapsPerPhase - 1;
    for (size_t ch = 0; ch < channels_; ++ch) {
        work_.resize(history + frames);
        std::copy(history_[ch].begin(), history_[ch].end(), work_.begin());
        std::copy(block[ch].begin(), block[ch].begin() + static_cast<std::ptrdiff_t>(frames), work_.begin() + static_cast<std::ptrdiff_t>(history));
        for (size_t i = 0; i < frames; ++i) {
            if (std::fabs(block[ch][i]) * kernel_.bound > peak_ * (1.0 + 1e-9)) {
                hot_[ch] = kernel_.tapsPerPhase;
            }
            if (hot_[ch] > 0) {
                --hot_[ch];
                peak_ = std::max(peak_, truePeakOfWindow(work_.data() + i, kernel_));
            }
        }
        std::copy(work_.begin() + static_cast<std::ptrdiff_t>(frames), work_.begin() + static_cast<std::ptrdiff_t>(frames + history), history_[ch].begin());
    }
}

void TruePeakMeter::finish() {
    AudioBlock tail = makeBlock(channels_, kernel_.delay);
    process(tail, kernel_.delay);
}

double TruePeakMeter::peakDb() const {
    return 20.0 * std::log10(std::max(peak_, 1e-10));
}

Meter::Meter(double sampleRate, size_t channels, PeakMode peakMode)
    : channels_(channels),
      peakMode_(peakMode),
      loudness_(sampleRate, channels),
      truePeak_(channels, peakMode == PeakMode::Accurate ? accurateKernel() : standardKernel()) {}

void Meter::process(const AudioBlock& block, size_t frames) {
    loudness_.process(block, frames);
    if (peakMode_ != PeakMode::Off) {
        truePeak_.process(block, frames);
    }
    if (channels_ >= 2) {
        for (size_t i = 0; i < frames; ++i) {
            const double l = block[0][i];
            const double r = block[1][i];
            leftEnergy_ += l * l;
            rightEnergy_ += r * r;
            crossEnergy_ += l * r;
            const double m = 0.5 * (l + r);
            const double s = 0.5 * (l - r);
            midEnergy_ += m * m;
            sideEnergy_ += s * s;
        }
    }
}

Measurements Meter::finish() {
    if (peakMode_ != PeakMode::Off) {
        truePeak_.finish();
    }
    const LoudnessResult loudness = loudness_.finish();
    Measurements m;
    m.integratedLufs = loudness.integratedLufs;
    m.loudnessRangeLu = loudness.loudnessRangeLu;
    if (peakMode_ != PeakMode::Off) {
        m.truePeakDb = truePeak_.peakDb();
    }
    if (channels_ >= 2) {
        m.hasStereo = true;
        if (leftEnergy_ <= 0.0 && rightEnergy_ <= 0.0) {
            m.correlation = 1.0;
        } else if (leftEnergy_ <= 0.0 || rightEnergy_ <= 0.0) {
            m.correlation = 0.0;
        } else {
            m.correlation = crossEnergy_ / std::sqrt(leftEnergy_ * rightEnergy_);
        }
        m.sideToMidDb = 10.0 * std::log10(std::max(sideEnergy_, 1e-20) / std::max(midEnergy_, 1e-20));
    }
    return m;
}

}
