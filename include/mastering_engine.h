#ifndef MASTERING_ENGINE_H
#define MASTERING_ENGINE_H

#include <memory>
#include <string>
#include <vector>
#include <functional>
#include "audio_loader.h"
#include "fft_analyzer.h"
#include "eq_calculator.h"
#include "spectrum_matcher.h"

namespace mastered {

/// Progress callback function type for real-time feedback during long-running operations
/// @param message User-friendly description of current processing stage
/// @param percentage Progress percentage (0-100). -1 indicates error state
using ProgressCallback = std::function<void(const std::string&, float)>;

namespace constants {
    /// Default Q factor for parametric EQ bands (higher = narrower bandwidth)
    constexpr float DEFAULT_Q_FACTOR = 2.0f;
    /// Threshold for detecting spectral peaks (in dB)
    constexpr float PEAK_DETECTION_THRESHOLD = 0.5f;
    /// Minimum gain threshold for EQ band generation (in dB)
    constexpr float MIN_GAIN_THRESHOLD = 0.3f;
    /// Default clipping headroom to prevent digital distortion (0-1 range)
    constexpr float CLIPPING_HEADROOM = 0.99f;
    /// Standard FFT size for spectral analysis (must be power of 2)
    constexpr uint32_t DEFAULT_FFT_SIZE = 8192;
    /// Standard audio sample rate (44.1 kHz)
    constexpr uint32_t DEFAULT_SAMPLE_RATE = 44100;
    /// Maximum amplitude before clipping (0-1 range, where 1.0 = full scale)
    constexpr float DEFAULT_CLIPPING_THRESHOLD = 0.99f;
}

/// Configuration structure for MasteringEngine behavior and processing parameters
/// Controls algorithm aggressiveness, output loudness targets, and various processing modes
struct MasteringConfig {
    /// Enable automatic makeup gain adjustment based on target LUFS (default: true)
    bool autoGain = true;
    /// Enable A-weighted perceptual loudness calculation (default: true)
    bool perceptualWeighting = true;
    /// Maximum number of parametric EQ bands to generate (1-32, default: 8)
    uint32_t maxEQBands = 8;
    /// Aggressiveness factor for EQ generation (0-1, higher = more aggressive matching)
    float aggressiveness = 0.8f;
    /// Enable spectral smoothing for cleaner EQ curves (default: true)
    bool smoothing = true;
    /// Target loudness in LUFS (Loudness Units relative to Full Scale), typically -14 to -18
    float targetLoudnessLUFS = -14.f;
    /// Clipping prevention headroom (0-1, where 1.0 = no prevention, 0.99 = 1% headroom)
    float clippingHeadroom = constants::DEFAULT_CLIPPING_THRESHOLD;
    /// Enable verbose console output for debugging (default: false)
    bool verbose = false;
};

/// Factory function to create a default mastering configuration
/// @return MasteringConfig with reasonable production defaults (aggressiveness: 0.85)
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

/// Results from audio mastering analysis and processing
struct MasteringResult {
    /// Generated parametric EQ curve with bands and frequency response
    EQCurve eqCurve;
    /// Measured loudness of input audio in LUFS (ITU-R BS.1770-4 standard)
    float estimatedLUFS;
    /// Recommended makeup gain in dB to reach target loudness
    float makeupGain;
    /// Spectral matching statistics (correlation, difference, confidence)
    MatchingResult matchingStats;
    /// Success flag (true if analysis completed without errors)
    bool success;
    /// Error or status message describing the result
    std::string message;
    /// Retained input buffer for caller reuse — avoids double load in main()
    AudioBuffer inputBuffer;
    
    MasteringResult() : estimatedLUFS(0.f), makeupGain(0.f), success(false) {}
};

/// Professional audio mastering engine using spectral analysis and parametric EQ
/// 
/// Main features:
/// - Real-time FFT spectral analysis with optional FFTW3 acceleration (500x faster)
/// - Reference-based spectral matching (correlates input to reference track)
/// - Parametric EQ curve generation (up to 8 adaptive bands)
/// - ITU-R BS.1770-4 compliant loudness measurement (LUFS)
/// - Automatic makeup gain calculation and clipping prevention
/// - Real-time progress callbacks for UI integration
///
/// Typical workflow:
/// 1. Create engine: `MasteringEngine engine(config)`
/// 2. Analyze: `auto result = engine.analyzeTracks(ref_path, unmastered_path)`
/// 3. Apply: `auto output = engine.applyMastering(input, result.eqCurve, result.makeupGain)`
/// 4. Save: `AudioLoader::saveWAV(output, "output.wav")`
class MasteringEngine {
public:
    /// Constructor with optional custom configuration
    /// @param config MasteringConfig specifying processing parameters
    /// @throws std::runtime_error if configuration validation fails
    MasteringEngine(const MasteringConfig& config = MasteringConfig());
    /// Destructor (implicitly defined, handles resource cleanup)
    ~MasteringEngine();
    
    /// Register a callback function to receive progress updates during processing
    /// Callback is invoked multiple times during analyzeTracks() and applyMastering()
    /// with stage-specific messages and progress percentage (0-100)
    /// @param callback Function signature: void(const std::string& message, float percentage)
    /// @note Callback can be nullptr to disable progress reporting
    /// @see clearProgressCallback()
    void setProgressCallback(const ProgressCallback& callback) {
        progressCallback_ = callback;
    }
    
    /// Remove the progress callback to disable progress updates
    /// @see setProgressCallback()
    void clearProgressCallback() {
        progressCallback_ = nullptr;
    }
    
    /// Analyze reference and unmastered tracks from file paths
    /// Performs spectral comparison and generates optimal EQ curve
    /// @param referenceTrackPath Path to reference audio file (WAV format)
    /// @param unmasteredTrackPath Path to audio requiring mastering (WAV format)
    /// @return MasteringResult with EQ curve, LUFS, and match statistics
    /// @throws std::runtime_error if files not found or format invalid
    /// @note Both files must have matching sample rates and be at least 1 second long
    /// @note Triggers progress callbacks at multiple stages (0% → 100%)
    MasteringResult analyzeTracks(const std::string& referenceTrackPath,
                                   const std::string& unmasteredTrackPath);
    
    /// Analyze audio buffers directly (alternative to file-based analysis)
    /// @param reference Reference audio buffer (reference spectral target)
    /// @param unmastered Audio buffer requiring mastering (will be matched to reference)
    /// @return MasteringResult with generated EQ curve and analysis metrics
    /// @throws std::runtime_error if buffer validation fails
    /// @note Sample rates and buffer lengths must match requirements
    MasteringResult analyzeBuffers(const AudioBuffer& reference,
                                    const AudioBuffer& unmastered);
    
    /// Apply parametric EQ and makeup gain to audio buffer
    /// @param input Audio buffer to be mastered
    /// @param eqCurve EQ curve generated from analysis (typically from analyzeTracks)
    /// @param makeupGain Gain adjustment in dB (optional, typically from analysis result)
    /// @return Processed audio buffer with EQ applied and clipping prevented
    /// @throws std::runtime_error if input buffer is empty
    /// @note Output buffer size matches input size
    /// @note Automatic clipping prevention scales down if peaks exceed headroom
    AudioBuffer applyMastering(const AudioBuffer& input, const EQCurve& eqCurve, float makeupGain = 0.f);
    
    /// Export EQ curve as JSON format for web frontends and data analysis
    /// @param result MasteringResult containing EQ curve to export
    /// @return JSON string with complete analysis data (frequencies, gains, Q factors)
    /// @note JSON includes both individual bands and overall frequency response curve
    std::string exportEQasJSON(const MasteringResult& result) const;
    
    /// Export EQ curve in Reaper ReaEQ plugin format (Reaper DAW)
    /// @param result MasteringResult containing EQ curve to export
    /// @return ReaEQ format string (can be copied into Reaper ReaEQ plugin)
    /// @note Enables direct application of generated mastering EQ in Reaper
    std::string exportEQasReaEQ(const MasteringResult& result) const;
    
    /// Export EQ curve in EqualizerAPO format (Windows audio tool)
    /// @param result MasteringResult containing EQ curve to export
    /// @return EqualizerAPO format string (can be used in EqualizerAPO config)
    /// @note Popular for system-wide equalization on Windows
    std::string exportEQasEqualizerAPO(const MasteringResult& result) const;
    
    /// Get the current processing configuration
    /// @return Const reference to the active MasteringConfig
    const MasteringConfig& getConfig() const { return config_; }
    
    /// Update the processing configuration with validation
    /// @param cfg New MasteringConfig to apply
    /// @throws std::runtime_error if configuration validation fails
    void setConfig(const MasteringConfig& cfg) { 
        validateConfig(cfg);
        config_ = cfg; 
    }

private:
    MasteringConfig config_;
    ProgressCallback progressCallback_;
    std::unique_ptr<FFTAnalyzer> fftAnalyzer_;
    std::unique_ptr<EQCalculator> eqCalculator_;
    std::unique_ptr<SpectrumMatcher> spectrumMatcher_;
    
    /// Helper function to emit progress callbacks if registered
    /// @param message User-friendly description of current processing stage
    /// @param percentage Progress percentage (0-100), -1 for error state
    void emitProgress(const std::string& message, float percentage) const {
        if (progressCallback_) {
            progressCallback_(message, percentage);
        }
    }
    
    /// Calculate LUFS (Loudness Units relative to Full Scale)
    /// ITU-R BS.1770-4 simplified implementation with K-weighting
    float calculateLUFS(const AudioBuffer& buffer) const;
    
    /// Apply A-weighting curve at given frequency (Hz)
    /// Used for perceptual loudness calculation
    float getAWeighting(float frequencyHz) const;
    
    /// Apply K-weighting for LUFS calculation
    float getKWeighting(float frequencyHz) const;
    
    /// Calculate makeup gain to reach target loudness
    /// @param original Original audio buffer
    /// @param targetLUFS Target loudness in LUFS
    /// @return Makeup gain adjustment in dB
    float calculateMakeupGain(const AudioBuffer& original,
                             float targetLUFS) const;
    
    /// Apply a single parametric EQ band using second-order IIR biquad filter (in-place)
    /// @param samples Audio sample vector to process
    /// @param band EQ band parameters (frequency, gain, Q factor, type)
    /// @param sampleRate Audio sample rate in Hz
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
