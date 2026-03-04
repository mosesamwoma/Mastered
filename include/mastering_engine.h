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

namespace constants {
    constexpr float DEFAULT_Q_FACTOR = 2.0f;
    constexpr float PEAK_DETECTION_THRESHOLD = 0.5f;
    constexpr float MIN_GAIN_THRESHOLD = 0.3f;
    constexpr float CLIPPING_HEADROOM = 0.99f;
    constexpr uint32_t DEFAULT_FFT_SIZE = 8192;
    constexpr uint32_t DEFAULT_SAMPLE_RATE = 44100;
    constexpr float DEFAULT_CLIPPING_THRESHOLD = 0.99f;
}

struct MasteringConfig {
    bool autoGain = true;
    bool perceptualWeighting = true;
    uint32_t maxEQBands = 8;
    float aggressiveness = 0.8f;
    bool smoothing = true;
    float targetLoudnessLUFS = -14.f;
    float clippingHeadroom = constants::DEFAULT_CLIPPING_THRESHOLD;
    bool verbose = false;
};

inline MasteringConfig createDefaultConfig() {
    MasteringConfig config;
    config.autoGain = true;
    config.perceptualWeighting = true;
    config.maxEQBands = 8;
    config.aggressiveness = 0.85f;
    config.smoothing = true;
    config.targetLoudnessLUFS = -14.f;
    config.clippingHeadroom = constants::DEFAULT_CLIPPING_THRESHOLD;
    config.verbose = false;
    return config;
}

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
    std::string exportEQasReaEQ(const MasteringResult& result) const;
    std::string exportEQasEqualizerAPO(const MasteringResult& result) const;
    
    /**
     * Get/set configuration
     */
    const MasteringConfig& getConfig() const { return config_; }
    void setConfig(const MasteringConfig& cfg) { 
        validateConfig(cfg);
        config_ = cfg; 
    }

private:
    MasteringConfig config_;
    std::unique_ptr<FFTAnalyzer> fftAnalyzer_;
    std::unique_ptr<EQCalculator> eqCalculator_;
    std::unique_ptr<SpectrumMatcher> spectrumMatcher_;
    
    /**
     * Calculate LUFS (Loudness Units relative to Full Scale)
     * ITU-R BS.1770-4 simplified implementation with K-weighting
     */
    float calculateLUFS(const AudioBuffer& buffer) const;
    
    /**
     * Apply A-weighting curve at given frequency (Hz)
     * Used for perceptual loudness calculation
     */
    float getAWeighting(float frequencyHz) const;
    
    /**
     * Apply K-weighting for LUFS calculation
     */
    float getKWeighting(float frequencyHz) const;
    
    float calculateMakeupGain(const AudioBuffer& original,
                             float targetLUFS) const;
    
    /**
     * Apply a single parametric EQ band using biquad filter (in-place)
     */
    void applyEQBandInPlace(std::vector<float>& samples,
                            const EQBand& band,
                            uint32_t sampleRate);
    
    bool validateAudioBuffer(const AudioBuffer& buffer, const std::string& bufferName) const {
        if (buffer.samples.empty()) {
            throw std::runtime_error(bufferName + " buffer is empty");
        }
        if (buffer.sampleRate == 0) {
            throw std::runtime_error(bufferName + " has invalid sample rate: 0");
        }
        if (buffer.sampleRate < 8000 || buffer.sampleRate > 192000) {
            throw std::runtime_error(bufferName + " sample rate out of range (8kHz-192kHz)");
        }
        uint32_t minSamples = buffer.sampleRate * 1;  // 1 second minimum
        if (buffer.samples.size() < minSamples) {
            throw std::runtime_error(bufferName + " must be at least 1 second long");
        }
        
        // Check for NaN or Inf
        for (float sample : buffer.samples) {
            if (std::isnan(sample) || std::isinf(sample)) {
                throw std::runtime_error(bufferName + " contains invalid samples (NaN or Inf)");
            }
        }
        return true;
    }
    
    bool validateConfig(const MasteringConfig& cfg) const {
        if (cfg.maxEQBands == 0 || cfg.maxEQBands > 32) {
            throw std::runtime_error("maxEQBands must be between 1 and 32");
        }
        if (cfg.aggressiveness <= 0.f || cfg.aggressiveness > 1.f) {
            throw std::runtime_error("aggressiveness must be between 0 and 1");
        }
        if (cfg.targetLoudnessLUFS > 0.f) {
            throw std::runtime_error("targetLoudnessLUFS must be negative (e.g., -14.0)");
        }
        if (cfg.clippingHeadroom <= 0.f || cfg.clippingHeadroom > 1.f) {
            throw std::runtime_error("clippingHeadroom must be between 0 and 1");
        }
        return true;
    }
};

} // namespace mastered

#endif // MASTERING_ENGINE_H
