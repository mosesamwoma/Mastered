#pragma once

#include "chain.h"
#include "eq_fitter.h"
#include "loudness.h"
#include "spectrum.h"
#include "waveform.h"

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
    size_t dynamicsBands = 3;
    bool adaptiveCrossovers = false;
    double dynamicsRatio = 2.0;
    double dynamicsThresholdOffsetDb = 0.0;
    double monoBassHz = 0.0;
    bool softClip = false;
    double softClipKneeDb = 3.0;
    bool accuratePeak = true;
    bool strictPeak = false;
    bool linearPhase = false;
    bool deess = false;
    std::string genre;
    double subsonicHz = 0.0;
    bool autoSubsonic = true;
    double limiterLookaheadMs = 5.0;
    double limiterReleaseMs = 80.0;
    uint16_t bitDepth = 0;
    bool dither = true;
    bool noiseShaping = false;
    double loudnessToleranceLu = 0.02;
    int maxLoudnessIterations = 12;
    bool autoClip = true;
    bool autoMonoBass = true;
    bool dualMono = false;
    bool bassLimiter = false;
    bool glue = false;
    bool resonance = false;
    double punchGuardPlrDb = 0.0;
    size_t fftSize = 8192;
    double maxBoostDb = 9.0;
    double maxCutDb = 12.0;
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
    std::vector<double> inputSpreadDb;
    std::vector<double> referenceSpreadDb;
    std::vector<double> dynamicsCrossoversHz;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint64_t frames = 0;
    uint16_t sourceBitDepth = 0;
    bool sourceFloat = false;
    size_t referenceCount = 0;
    Measurements input;
    Measurements reference;
    std::vector<double> shapeCentresHz;
    std::vector<double> inputShapeDb;
    std::vector<double> referenceShapeDb;
    std::vector<size_t> shapeRange;
    double shapeWidthOctaves = 1.0 / 3.0;
    Waveform inputWaveform;
    std::vector<std::string> warnings;
    std::vector<std::string> notes;
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
    Waveform outputWaveform;
    LimiterStats limiter;
    std::vector<double> outputShapeDb;
    double matchBeforeDb = 0.0;
    double matchAfterDb = 0.0;
    bool matchMeasured = false;
    double ceilingTrimDb = 0.0;
    double effectiveTargetLufs = 0.0;
    double safeMaxLufs = 0.0;
    uint64_t framesWritten = 0;
    std::vector<std::string> warnings;
    std::vector<std::string> notes;
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

    static Measurements measureFile(const std::string& path, bool dualMono = false, bool strictPeak = false);
    static std::string measurementsJsonText(const Measurements& measurements);

    static std::string exportJSON(const MasteringResult& result, const MasteringConfig& config = MasteringConfig());
    static std::string exportReaEQ(const Analysis& analysis);
    static std::string exportEqualizerAPO(const Analysis& analysis);

private:
    void emit(const std::string& message, double percent) const;

    MasteringConfig config_;
    ProgressCallback progress_;
};

}
