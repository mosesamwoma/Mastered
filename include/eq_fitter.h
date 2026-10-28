#pragma once

#include "types.h"

#include <cstddef>
#include <vector>

namespace mastered {

struct FitOptions {
    double sampleRate = 44100.0;
    double minHz = 30.0;
    double maxHz = 18000.0;
    size_t maxBands = 8;
    double maxGainDb = 12.0;
    bool perceptualWeighting = true;
};

struct FitResult {
    std::vector<EQBand> bands;
    std::vector<double> gridHz;
    std::vector<double> targetDb;
    std::vector<double> responseDb;
    double residualRmsDb = 0.0;
};

FitResult fitEQ(const std::vector<double>& centresHz, const std::vector<double>& targetDb, const FitOptions& options);
std::vector<double> combinedResponseDb(const std::vector<EQBand>& bands, const std::vector<double>& gridHz, double sampleRate);

}
