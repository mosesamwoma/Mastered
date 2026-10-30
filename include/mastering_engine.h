#pragma once

#include "chain.h"
#include "eq_fitter.h"
#include "loudness.h"
#include "spectrum.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace mastered {

struct MasteringConfig {
    double targetLufs = -14.0;
    double aggressiveness = 0.85;
    double ceilingDb = -1.0;
    size_t maxEQBands = 8;
    size_t maxSideBands = 4;
    bool smoothing = true;
    bool perceptualWeighting = true;
    bool autoGain = true;
    bool midSide = true;
    bool matchWidth = true;
    bool dynamics = false;
    bool dynamicsMatch = true;
    double dynamicsRatio = 2.0;
    double dynamicsThresholdOffsetDb = 0.0;
    bool softClip = false;
    double softClipKneeDb = 3.0;
    bool accuratePeak = true;
    double limiterLookaheadMs = 3.0;
    double limiterReleaseMs = 80.0;
    uint16_t bitDepth = 0;
    bool dither = true;
    bool noiseShaping = false;
    double loudnessToleranceLu = 0.1;
    int maxLoudnessIterations = 10;
    size_t fftSize = 8192;
};

struct CurveFit {
    std::vector<EQBand> bands;
    std::vector<double> frequenciesHz;
    std::vector<double> targetDb;
    std::vector<double> responseDb;
    double residualRmsDb = 0.0;
};

struct Analysis {
    ChainPlan plan;
    CurveFit mid;
    CurveFit side;
    double sideGainDb = 0.0;
    std::array<double, 3> inputSpreadDb{{0.0, 0.0, 0.0}};
    std::array<double, 3> referenceSpreadDb{{0.0, 0.0, 0.0}};
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint64_t frames = 0;
    uint16_t sourceBitDepth = 0;
    bool sourceFloat = false;
    size_t referenceCount = 0;
    Measurements input;
    Measurements reference;
    std::vector<std::string> warnings;
};

struct MasteringResult {
    Analysis analysis;
    Measurements output;
    std::string inputPath;
    std::string outputPath;
    uint16_t outputBitDepth = 0;
    double makeupGainDb = 0.0;
    int iterations = 0;
    bool targetReached = false;
    std::vector<std::string> warnings;
};

using ProgressCallback = std::function<void(const std::string&, double)>;

class MasteringEngine {
public:
    explicit MasteringEngine(const MasteringConfig& config = MasteringConfig());

    void setConfig(const MasteringConfig& config);
    const MasteringConfig& config() const { return config_; }
    void setProgressCallback(ProgressCallback callback) { progress_ = std::move(callback); }

    Analysis analyze(const std::vector<std::string>& references, const std::string& input);
    MasteringResult render(const Analysis& analysis, const std::string& input, const std::string& output);
    MasteringResult run(const std::vector<std::string>& references, const std::string& input, const std::string& output);

    static std::string exportJSON(const MasteringResult& result);
    static std::string exportReaEQ(const Analysis& analysis);
    static std::string exportEqualizerAPO(const Analysis& analysis);

private:
    void emit(const std::string& message, double percent) const;

    MasteringConfig config_;
    ProgressCallback progress_;
};

}
