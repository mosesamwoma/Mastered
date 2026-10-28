#include "eq_fitter.h"

#include "biquad.h"
#include "constants.h"
#include "error.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace mastered {

namespace {

double aWeightLinear(double f) {
    const double f2 = f * f;
    const double numerator = 12194.0 * 12194.0 * f2 * f2;
    const double denominator = (f2 + 20.6 * 20.6) * std::sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9)) * (f2 + 12194.0 * 12194.0);
    return numerator / denominator;
}

double interpolateTarget(const std::vector<double>& centres, const std::vector<double>& values, double frequency) {
    if (frequency <= centres.front()) {
        return values.front();
    }
    if (frequency >= centres.back()) {
        return values.back();
    }
    const auto upper = std::upper_bound(centres.begin(), centres.end(), frequency);
    const size_t i1 = static_cast<size_t>(upper - centres.begin());
    const size_t i0 = i1 - 1;
    const double t = (std::log(frequency) - std::log(centres[i0])) / (std::log(centres[i1]) - std::log(centres[i0]));
    return values[i0] * (1.0 - t) + values[i1] * t;
}

class Fitter {
public:
    Fitter(std::vector<double> grid, std::vector<double> target, std::vector<double> weight, const FitOptions& options, double maxHz)
        : grid_(std::move(grid)),
          target_(std::move(target)),
          weight_(std::move(weight)),
          sampleRate_(options.sampleRate),
          minHz_(options.minHz),
          maxHz_(maxHz),
          maxGain_(options.maxGainDb) {
        cos1_.resize(grid_.size());
        sin1_.resize(grid_.size());
        cos2_.resize(grid_.size());
        sin2_.resize(grid_.size());
        for (size_t i = 0; i < grid_.size(); ++i) {
            const double w = 2.0 * kPi * grid_[i] / sampleRate_;
            cos1_[i] = std::cos(w);
            sin1_[i] = std::sin(w);
            cos2_[i] = std::cos(2.0 * w);
            sin2_[i] = std::sin(2.0 * w);
            weightSum_ += weight_[i];
        }
    }

    std::vector<double> response(const std::vector<EQBand>& bands) const {
        std::vector<double> out(grid_.size(), 0.0);
        for (const EQBand& band : bands) {
            const BiquadCoefficients c = designBand(band, sampleRate_);
            for (size_t i = 0; i < grid_.size(); ++i) {
                out[i] += biquadResponseDb(c, cos1_[i], sin1_[i], cos2_[i], sin2_[i]);
            }
        }
        return out;
    }

    double weightedError(const std::vector<double>& response) const {
        double error = 0.0;
        for (size_t i = 0; i < grid_.size(); ++i) {
            const double d = target_[i] - response[i];
            error += weight_[i] * d * d;
        }
        return error / weightSum_;
    }

    double cost(const std::vector<EQBand>& bands) const {
        double value = weightedError(response(bands));
        for (const EQBand& band : bands) {
            const double lq = std::log(band.q);
            value += 0.004 * band.gainDb * band.gainDb + 0.03 * lq * lq;
        }
        return value;
    }

    void clampBand(EQBand& band) const {
        double lowHz = minHz_;
        double highHz = maxHz_;
        double lowQ = 0.4;
        double highQ = 6.0;
        if (band.type == BandType::LowShelf) {
            highHz = std::min(500.0, maxHz_);
            highQ = 1.0;
        } else if (band.type == BandType::HighShelf) {
            lowHz = std::min(1500.0, maxHz_ * 0.5);
            highQ = 1.0;
        }
        band.frequency = std::min(std::max(band.frequency, lowHz), highHz);
        band.q = std::min(std::max(band.q, lowQ), highQ);
        band.gainDb = std::min(std::max(band.gainDb, -maxGain_), maxGain_);
    }

    void refine(std::vector<EQBand>& bands, size_t first) const {
        double best = cost(bands);
        for (double scale = 1.0; scale >= 0.06; scale *= 0.5) {
            for (int round = 0; round < 6; ++round) {
                bool improved = false;
                for (size_t index = first; index < bands.size(); ++index) {
                    for (int dimension = 0; dimension < 3; ++dimension) {
                        for (int direction = -1; direction <= 1; direction += 2) {
                            std::vector<EQBand> trial = bands;
                            EQBand& band = trial[index];
                            const double step = static_cast<double>(direction) * scale;
                            if (dimension == 0) {
                                band.frequency *= std::pow(2.0, 0.25 * step);
                            } else if (dimension == 1) {
                                band.gainDb += 1.5 * step;
                            } else {
                                band.q *= std::exp(0.35 * step);
                            }
                            clampBand(band);
                            const double c = cost(trial);
                            if (c < best - 1e-9) {
                                best = c;
                                bands = std::move(trial);
                                improved = true;
                                break;
                            }
                        }
                    }
                }
                if (!improved) {
                    break;
                }
            }
        }
    }

    const std::vector<double>& grid() const { return grid_; }
    const std::vector<double>& target() const { return target_; }
    const std::vector<double>& weight() const { return weight_; }

private:
    std::vector<double> grid_;
    std::vector<double> target_;
    std::vector<double> weight_;
    double sampleRate_;
    double minHz_;
    double maxHz_;
    double maxGain_;
    double weightSum_ = 0.0;
    std::vector<double> cos1_;
    std::vector<double> sin1_;
    std::vector<double> cos2_;
    std::vector<double> sin2_;
};

}

std::vector<double> combinedResponseDb(const std::vector<EQBand>& bands, const std::vector<double>& gridHz, double sampleRate) {
    std::vector<double> out(gridHz.size(), 0.0);
    for (const EQBand& band : bands) {
        const BiquadCoefficients c = designBand(band, sampleRate);
        for (size_t i = 0; i < gridHz.size(); ++i) {
            out[i] += biquadMagnitudeDb(c, gridHz[i], sampleRate);
        }
    }
    return out;
}

FitResult fitEQ(const std::vector<double>& centresHz, const std::vector<double>& targetDb, const FitOptions& options) {
    if (centresHz.size() < 2 || centresHz.size() != targetDb.size()) {
        throw MasteredError("EQ fit requires matching frequency and target arrays");
    }
    if (!(options.sampleRate > 0.0) || !(options.minHz > 0.0) || !(options.maxGainDb > 0.0)) {
        throw MasteredError("Invalid EQ fit options");
    }
    const double maxHz = std::min(options.maxHz, 0.45 * options.sampleRate);
    if (!(maxHz > options.minHz * 2.0)) {
        throw MasteredError("EQ fit frequency range is too narrow");
    }

    std::vector<double> grid;
    for (int i = 0;; ++i) {
        const double f = options.minHz * std::pow(2.0, static_cast<double>(i) / 12.0);
        if (f > maxHz * (1.0 + 1e-9)) {
            break;
        }
        grid.push_back(std::min(f, maxHz));
    }
    std::vector<double> target(grid.size());
    std::vector<double> weight(grid.size(), 1.0);
    const double reference = aWeightLinear(1000.0);
    for (size_t i = 0; i < grid.size(); ++i) {
        target[i] = interpolateTarget(centresHz, targetDb, grid[i]);
        if (options.perceptualWeighting) {
            weight[i] = std::min(1.0, std::max(0.35, aWeightLinear(grid[i]) / reference));
        }
    }

    Fitter fitter(grid, target, weight, options, maxHz);
    std::vector<EQBand> bands;
    double current = fitter.cost(bands);
    const std::array<BandType, 3> types = {BandType::Peak, BandType::LowShelf, BandType::HighShelf};

    for (size_t iteration = 0; iteration < options.maxBands; ++iteration) {
        const std::vector<double> response = fitter.response(bands);
        std::vector<double> residual(grid.size());
        for (size_t i = 0; i < grid.size(); ++i) {
            residual[i] = target[i] - response[i];
        }
        std::vector<double> smoothed(grid.size());
        for (size_t i = 0; i < grid.size(); ++i) {
            const size_t lo = i >= 2 ? i - 2 : 0;
            const size_t hi = std::min(i + 2, grid.size() - 1);
            double sum = 0.0;
            for (size_t j = lo; j <= hi; ++j) {
                sum += residual[j];
            }
            smoothed[i] = sum / static_cast<double>(hi - lo + 1);
        }
        size_t pick = 0;
        double significance = 0.0;
        for (size_t i = 0; i < grid.size(); ++i) {
            const double s = weight[i] * std::fabs(smoothed[i]);
            if (s > significance) {
                significance = s;
                pick = i;
            }
        }
        if (significance < 0.4) {
            break;
        }
        const double initialGain = std::min(std::max(smoothed[pick], -options.maxGainDb), options.maxGainDb);
        std::vector<EQBand> bestTrial;
        double bestCost = std::numeric_limits<double>::infinity();
        for (BandType type : types) {
            EQBand candidate;
            candidate.type = type;
            candidate.frequency = grid[pick];
            candidate.gainDb = initialGain;
            candidate.q = type == BandType::Peak ? 1.0 : 0.7;
            fitter.clampBand(candidate);
            std::vector<EQBand> trial = bands;
            trial.push_back(candidate);
            fitter.refine(trial, trial.size() - 1);
            const double c = fitter.cost(trial);
            if (c < bestCost) {
                bestCost = c;
                bestTrial = std::move(trial);
            }
        }
        if (!(bestCost < current - 1e-3)) {
            break;
        }
        bands = std::move(bestTrial);
        fitter.refine(bands, 0);
        current = fitter.cost(bands);
    }

    const size_t before = bands.size();
    bands.erase(std::remove_if(bands.begin(), bands.end(), [](const EQBand& b) { return std::fabs(b.gainDb) < 0.3; }), bands.end());
    if (bands.size() != before && !bands.empty()) {
        fitter.refine(bands, 0);
    }
    std::sort(bands.begin(), bands.end(), [](const EQBand& a, const EQBand& b) { return a.frequency < b.frequency; });

    FitResult result;
    result.bands = bands;
    result.gridHz = grid;
    result.targetDb = target;
    result.responseDb = fitter.response(bands);
    result.residualRmsDb = std::sqrt(fitter.weightedError(result.responseDb));
    return result;
}

}
