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
    double dynamicsRatio = 2.0;
    double dynamicsThresholdOffsetDb = 0.0;
    double monoBassHz = 0.0;
    bool softClip = false;
    double softClipKneeDb = 3.0;
    bool accuratePeak = true;
    double limiterLookaheadMs = 5.0;
    double limiterReleaseMs = 80.0;
    uint16_t bitDepth = 0;
    bool dither = true;
    bool noiseShaping = false;
    double loudnessToleranceLu = 0.02;
    int maxLoudnessIterations = 12;
    // When the makeup gain would force the limiter to remove more than about 3 dB, a soft clipper takes the tallest
    // peaks first so that the limiter does not have to pull the whole mix down on every kick.
    bool autoClip = true;
    // Turns on mono bass automatically when the low end of the input is clearly out of phase between left and right.
    bool autoMonoBass = true;
    // Measure single-channel files as dual mono (+3 LU), as some meters do.
    bool dualMono = false;
    // Experimental: limit the band below about 140 Hz separately. In measurements it shifted loudness from the bass to
    // the highs and did not reduce modulation of the highs, so it is off unless asked for.
    bool bassLimiter = false;
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

    // Measures a finished file with the same meter that the engine uses, for example to check a decoded MP3.
    static Measurements measureFile(const std::string& path, bool dualMono = false, bool accuratePeak = true);
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
