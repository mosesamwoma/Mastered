#ifndef MASTERING_ENGINE_H
#define MASTERING_ENGINE_H

#include <memory>
#include <string>
#include <vector>
#include "audio_loader.h"
#include "fft_analyzer.h"
#include "eq_calculator.h"
#include "spectrum_matcher.h"

namespace mastered {

struct MasteringConfig {
    bool autoGain = true;           // Calculate makeup gain
    bool perceptualWeighting = true; // Use A-weighting
    uint32_t maxEQBands = 8;        // Maximum parametric bands
    float aggressiveness = 0.8f;    // 0-1, how much to correct (0.8 = 80%)
    bool smoothing = true;          // Smooth EQ curve
    float targetLoudnessLUFS = -14.f; // Target loudness (LUFS)
};

struct MasteringResult {
    EQCurve eqCurve;
    float estimatedLUFS;
    float makeupGain;
    MatchingResult matchingStats;
    bool success;
    std::string message;
    
    MasteringResult() : estimatedLUFS(0.f), makeupGain(0.f), success(false) {}
};

class MasteringEngine {
public:
    MasteringEngine(const MasteringConfig& config = MasteringConfig());
    ~MasteringEngine();
    
    /**
     * Analyze a reference track and unmastered track
     * Returns optimal EQ curve to match unmastered to reference
     */
    MasteringResult analyzeTracks(const std::string& referenceTrackPath,
                                   const std::string& unmasteredTrackPath);
    
    /**
     * Analyze audio buffers directly
     */
    MasteringResult analyzeBuffers(const AudioBuffer& reference,
                                    const AudioBuffer& unmastered);
    
    /**
     * Apply mastering EQ to audio and return result
     */
    AudioBuffer applyMastering(const AudioBuffer& input, const EQCurve& eqCurve, float makeupGain = 0.f);
    
    /**
     * Export EQ curve as JSON for frontend display/application
     */
    std::string exportEQasJSON(const MasteringResult& result) const;
    
    /**
     * Export EQ curve in DAW-compatible formats
     */
    std::string exportEQasReaEQ(const MasteringResult& result) const;  // ReaEQ format
    std::string exportEQasEqualizerAPO(const MasteringResult& result) const; // EqualizerAPO format
    
    /**
     * Get configuration
     */
    const MasteringConfig& getConfig() const { return config_; }
    void setConfig(const MasteringConfig& cfg) { config_ = cfg; }

private:
    MasteringConfig config_;
    std::unique_ptr<FFTAnalyzer> fftAnalyzer_;
    std::unique_ptr<EQCalculator> eqCalculator_;
    std::unique_ptr<SpectrumMatcher> spectrumMatcher_;
    
    /**
     * Calculate LUFS (Loudness Units relative to Full Scale)
     */
    float calculateLUFS(const AudioBuffer& buffer) const;
    
    float calculateMakeupGain(const AudioBuffer& original,
                             float targetLUFS) const;
    
    /**
     * Apply a single parametric EQ band using biquad filter
     */
    std::vector<float> applyEQBand(const std::vector<float>& samples,
                                   const EQBand& band,
                                   uint32_t sampleRate);
};

} // namespace mastered

#endif // MASTERING_ENGINE_H
