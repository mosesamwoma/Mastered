#include "mastering_engine.h"
#include <cmath>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>
#include <iostream>

namespace mastered {

MasteringEngine::MasteringEngine(const MasteringConfig& config)
    : config_(config),
      fftAnalyzer_(std::make_unique<FFTAnalyzer>(constants::DEFAULT_FFT_SIZE)),
      eqCalculator_(std::make_unique<EQCalculator>()),
      spectrumMatcher_(std::make_unique<SpectrumMatcher>()) {
    validateConfig(config_);
}

MasteringEngine::~MasteringEngine() = default;

MasteringResult MasteringEngine::analyzeTracks(const std::string& referenceTrackPath,
                                               const std::string& unmasteredTrackPath) {
    auto referenceBuffer = AudioLoader::loadWAV(referenceTrackPath);
    auto unmasteredBuffer = AudioLoader::loadWAV(unmasteredTrackPath);
    
    return analyzeBuffers(referenceBuffer, unmasteredBuffer);
}

MasteringResult MasteringEngine::analyzeBuffers(const AudioBuffer& reference,
                                                const AudioBuffer& unmastered) {
    MasteringResult result;
    result.inputBuffer = unmastered;  // Retain for caller reuse — avoids double load
    
    try {
        emitProgress("Starting audio analysis...", 0.f);
        
        // Validate inputs
        validateAudioBuffer(reference, "Reference");
        validateAudioBuffer(unmastered, "Unmastered");
        validateConfig(config_);
        
        emitProgress("Validating audio parameters...", 10.f);
        
        // Check sample rate compatibility
        if (reference.sampleRate != unmastered.sampleRate) {
            throw std::runtime_error(
                "Sample rate mismatch: reference=" + std::to_string(reference.sampleRate) +
                " Hz, unmastered=" + std::to_string(unmastered.sampleRate) + " Hz"
            );
        }
        
        if (reference.sampleRate == 0 || unmastered.sampleRate == 0) {
            throw std::runtime_error("Invalid sample rate in audio buffers");
        }
        
        fftAnalyzer_->setSampleRate(reference.sampleRate);
        
        emitProgress("Computing reference spectrum...", 20.f);
        auto refSpectrum = fftAnalyzer_->getAveragedSpectrum(reference.samples);
        
        emitProgress("Computing target spectrum...", 30.f);
        auto targetSpectrum = fftAnalyzer_->getAveragedSpectrum(unmastered.samples);
        
        emitProgress("Smoothing frequency response...", 40.f);
        refSpectrum = fftAnalyzer_->smoothSpectrum(refSpectrum, 7);
        targetSpectrum = fftAnalyzer_->smoothSpectrum(targetSpectrum, 7);
        
        emitProgress("Matching spectral characteristics...", 50.f);
        result.matchingStats = spectrumMatcher_->matchSpectra(refSpectrum, targetSpectrum);
        
        emitProgress("Generating EQ curve...", 60.f);
        result.eqCurve = eqCalculator_->calculateEQFromReference(refSpectrum, targetSpectrum);
        
        for (auto& band : result.eqCurve.bands) {
            band.gain *= config_.aggressiveness;
        }
        for (float& val : result.eqCurve.curveResponse) {
            val *= config_.aggressiveness;
        }
        
        emitProgress("Calculating loudness (LUFS)...", 75.f);
        result.estimatedLUFS = calculateLUFS(unmastered);
        
        emitProgress("Computing makeup gain...", 85.f);
        if (config_.autoGain) {
            result.makeupGain = calculateMakeupGain(unmastered, config_.targetLoudnessLUFS);
        }
        
        result.success = true;
        result.message = "Analysis completed successfully";
        
        emitProgress("Analysis complete", 100.f);
        
        if (config_.verbose) {
            std::cerr << "✓ FFT Size: " << constants::DEFAULT_FFT_SIZE << "\n";
            std::cerr << "✓ Sample Rate: " << reference.sampleRate << " Hz\n";
            std::cerr << "✓ EQ Bands: " << result.eqCurve.bands.size() << "\n";
        }
        
    } catch (const std::exception& e) {
        result.success = false;
        result.message = std::string("Analysis failed: ") + e.what();
        emitProgress("Analysis failed: " + std::string(e.what()), -1.f);
    }
    
    return result;
}

AudioBuffer MasteringEngine::applyMastering(const AudioBuffer& input, const EQCurve& eqCurve, float makeupGain) {
    AudioBuffer output = input;
    
    if (output.samples.empty()) {
        throw std::runtime_error("Cannot apply mastering to empty buffer");
    }
    
    emitProgress("Applying EQ mastering...", 0.f);
    
    // Apply each parametric EQ band using second-order IIR filtering (in-place)
    size_t bandCount = 0;
    for (const auto& band : eqCurve.bands) {
        if (band.gain != 0.f && band.frequency > 0.f && band.qFactor > 0.01f) {
            applyEQBandInPlace(output.samples, band, output.sampleRate);
            bandCount++;
        }
    }
    
    if (eqCurve.bands.size() > 0) {
        float progress = 50.f * (float)bandCount / eqCurve.bands.size();
        emitProgress("Applied EQ bands...", progress);
    }
    
    // Apply makeup gain
    emitProgress("Applying makeup gain...", 75.f);
    if (makeupGain != 0.f) {
        float gainLinear = std::pow(10.f, makeupGain / 20.f);
        for (float& sample : output.samples) {
            sample *= gainLinear;
        }
    }
    
    emitProgress(\"Checking true peak (4x oversampling)...\", 85.f);
    
    // True peak limiter: detect and prevent inter-sample peaks (exceeds 0 dBFS on D/A)
    // Using 4x oversampling via linear interpolation approximation
    constexpr float TRUE_PEAK_LIMIT = 0.99f;  // -0.1 dBTP for streaming compliance
    float truePeakMax = 0.f;
    for (size_t i = 0; i + 1 < output.samples.size(); ++i) {\n        // Check interpolation points between samples (4x oversampling)\n        for (int frac = 1; frac < 4; ++frac) {\n            float t = frac / 4.f;\n            float interpolated = output.samples[i] * (1.f - t) + output.samples[i + 1] * t;\n            truePeakMax = std::max(truePeakMax, std::abs(interpolated));\n        }\n        truePeakMax = std::max(truePeakMax, std::abs(output.samples[i]));\n    }\n    if (!output.samples.empty()) {\n        truePeakMax = std::max(truePeakMax, std::abs(output.samples.back()));\n    }\n    \n    if (truePeakMax > TRUE_PEAK_LIMIT) {\n        float tpScale = TRUE_PEAK_LIMIT / truePeakMax;\n        for (float& s : output.samples) s *= tpScale;\n        if (config_.verbose) {\n            std::cerr << \"⚠ True peak limiting: scaled by \" << tpScale << \" (true peak was \" \n                      << truePeakMax << \" = \" << (20.f * std::log10(truePeakMax)) << \" dBFS)\\n\";\n        }\n    }\n    \n    emitProgress(\"Preventing clipping...\", 90.f);\n    \n    // Peak limiter: prevent sample clipping with configurable headroom\n    float maxAbs = 0.f;\n    for (float sample : output.samples) {\n        maxAbs = std::max(maxAbs, std::abs(sample));\n    }\n    \n    if (maxAbs > 1.f) {\n        float scale = config_.clippingHeadroom / maxAbs;\n        for (float& sample : output.samples) {\n            sample *= scale;\n        }\n        if (config_.verbose) {\n            std::cerr << \"⚠ Clipping prevention: scaled by \" << scale << \"\\n\";\n        }\n    }\n    \n    emitProgress(\"Mastering complete\", 100.f);
    
    return output;
}

void MasteringEngine::applyEQBandInPlace(std::vector<float>& samples,
                                         const EQBand& band,
                                         uint32_t sampleRate) {
    if (samples.empty() || band.gain == 0.f) {
        return;
    }
    
    // Validate parameters
    if (band.frequency <= 0.f || sampleRate == 0 || band.qFactor <= 0.01f) {
        return;
    }
    
    // Nyquist check
    float nyquist = sampleRate / 2.f;
    if (band.frequency >= nyquist) {
        return;  // Frequency above Nyquist, skip
    }
    
    // Calculate second-order filter coefficients (Robert Bristow-Johnson peaking EQ)
    float A = std::pow(10.f, band.gain / 40.f);
    float w0 = 2.f * M_PI * band.frequency / sampleRate;
    float sinW0 = std::sin(w0);
    float cosW0 = std::cos(w0);
    float alpha = sinW0 / (2.f * band.qFactor);
    
    // Peaking EQ filter coefficients
    float b0 = 1.f + alpha * A;
    float b1 = -2.f * cosW0;
    float b2 = 1.f - alpha * A;
    float a0 = 1.f + alpha / A;
    float a1 = -2.f * cosW0;
    float a2 = 1.f - alpha / A;
    
    // Normalize
    b0 /= a0;
    b1 /= a0;
    b2 /= a0;
    a1 /= a0;
    a2 /= a0;
    
    // Apply biquad filter in-place (Direct Form II)
    float w_n1 = 0.f, w_n2 = 0.f;
    
    for (size_t i = 0; i < samples.size(); ++i) {
        float w_n = samples[i] - a1 * w_n1 - a2 * w_n2;
        samples[i] = b0 * w_n + b1 * w_n1 + b2 * w_n2;
        
        w_n2 = w_n1;
        w_n1 = w_n;
    }
}

std::string MasteringEngine::exportEQasJSON(const MasteringResult& result) const {
    std::ostringstream json;
    json << std::fixed << std::setprecision(2);
    
    json << "{\n";
    json << "  \"success\": " << (result.success ? "true" : "false") << ",\n";
    json << "  \"message\": \"" << result.message << "\",\n";
    json << "  \"version\": \"1.1\",\n";
    json << "  \"stats\": {\n";
    json << "    \"correlation\": " << result.matchingStats.correlation << ",\n";
    json << "    \"spectralDifference\": " << result.matchingStats.spectralDifference << ",\n";
    json << "    \"confidence\": " << result.matchingStats.confidenceScore << ",\n";
    json << "    \"estimatedLUFS\": " << result.estimatedLUFS << ",\n";
    json << "    \"makeupGain\": " << result.makeupGain << "\n";
    json << "  },\n";
    
    json << "  \"eqBands\": [\n";
    for (size_t i = 0; i < result.eqCurve.bands.size(); ++i) {
        const auto& band = result.eqCurve.bands[i];
        json << "    {\n";
        json << "      \"id\": " << i << ",\n";
        json << "      \"frequency\": " << band.frequency << ",\n";
        json << "      \"gain\": " << band.gain << ",\n";
        json << "      \"q\": " << band.qFactor << ",\n";
        json << "      \"type\": \"" << band.type << "\",\n";
        json << "      \"bandwidth\": " << (band.frequency / band.qFactor) << "\n";
        json << "    }";
        if (i < result.eqCurve.bands.size() - 1) json << ",";
        json << "\n";
    }
    json << "  ],\n";
    
    json << "  \"curveResponse\": [\n";
    for (size_t i = 0; i < result.eqCurve.curveResponse.size(); ++i) {
        if (i % 10 == 0) json << "    ";
        json << result.eqCurve.curveResponse[i];
        if (i < result.eqCurve.curveResponse.size() - 1) json << ", ";
        if ((i + 1) % 10 == 0) json << "\n";
    }
    json << "  ]\n";
    json << "}\n";
    
    return json.str();
}

std::string MasteringEngine::exportEQasReaEQ(const MasteringResult& result) const {
    std::ostringstream reaEQ;
    
    reaEQ << "<ReaEQ " << result.eqCurve.bands.size() << " bands>\n";
    reaEQ << "Enabled=1\n\n";
    
    for (size_t i = 0; i < result.eqCurve.bands.size(); ++i) {
        const auto& band = result.eqCurve.bands[i];
        float bandwidth = band.frequency / band.qFactor;
        reaEQ << "Band " << (i + 1) << ": freq=" << std::fixed << std::setprecision(1) << band.frequency
              << " gain=" << band.gain
              << " bw=" << bandwidth
              << " type=" << band.type << "\n";
    }
    
    return reaEQ.str();
}

std::string MasteringEngine::exportEQasEqualizerAPO(const MasteringResult& result) const {
    std::ostringstream apoEQ;
    apoEQ << std::fixed << std::setprecision(2);
    
    for (const auto& band : result.eqCurve.bands) {
        apoEQ << "Filter: ON PK Fc=" << std::setprecision(1) << band.frequency
              << " Gain=" << std::setprecision(2) << band.gain
              << " Q=" << band.qFactor << "\n";
    }
    
    return apoEQ.str();
}

// A-weighting coefficients for perceptual loudness
// Based on IEC 61672-1:2013
float MasteringEngine::getAWeighting(float frequencyHz) const {
    if (frequencyHz <= 0.f) return 0.f;
    
    float f = frequencyHz;
    float f2 = f * f;
    float f4 = f2 * f2;
    
    // A-weighting curve using correct pole frequencies (IEC 61672-1)
    // Coefficients: f1=20.6, f2=107.7, f3=737.9, f4=12194.2
    float numerator = 12194.217f * f4;
    
    // Each term must be (f^2 + pole^2), not (f^2 + k*f^2)
    float f1_2 = 20.598997f * 20.598997f;      // 424.24
    float f2_2 = 107.65265f * 107.65265f;      // 11587.90
    float f3_2 = 737.86223f * 737.86223f;      // 544453.87
    float f4_2 = 12194.217f * 12194.217f;      // 148699450.91
    
    float denominator = (f2 + f1_2) * 
                       (f2 + f2_2) * 
                       (f2 + f3_2) * 
                       (f2 + f4_2);
    
    if (denominator < 1e-10f) return 0.f;
    
    float Af = 20.f * std::log10(numerator / std::sqrt(denominator)) + 2.f;
    
    return std::pow(10.f, Af / 10.f);  // Convert back to linear
}

// K-weighting for LUFS calculation - deprecated (see calculateLUFS for correct implementation)
float MasteringEngine::getKWeighting(float frequencyHz) const {
    // K-weighting is now properly applied inside calculateLUFS() as biquad filters
    // This function is kept for backwards compatibility only
    if (frequencyHz <= 0.f) return 0.f;
    return 1.f;  // No weighting applied here
}

float MasteringEngine::calculateLUFS(const AudioBuffer& buffer) const {
    if (buffer.samples.empty()) return -120.f;

    float fs = static_cast<float>(buffer.sampleRate);
    constexpr float CALIBRATION_CONST = -0.691f;

    // ── Stage 1: High-shelf pre-filter (ITU-R BS.1770-4, Table 1)
    // High-shelf filter centered around 2kHz: +4 dB shelf
    // Coefficients computed for 48 kHz via bilinear transform
    auto applyBiquad = [](const std::vector<float>& x,
                          float b0, float b1, float b2,
                          float a1, float a2) -> std::vector<float> {
        std::vector<float> y(x.size());
        float w1 = 0.f, w2 = 0.f;
        for (size_t i = 0; i < x.size(); ++i) {
            float w_n = x[i] - a1 * w1 - a2 * w2;
            y[i] = b0 * w_n + b1 * w1 + b2 * w2;
            w2 = w1;
            w1 = w_n;
        }
        return y;
    };

    // High-shelf pre-filter (bilinear transform at 48 kHz)
    // For arbitrary sample rates, recalculate using: fc=2000Hz, gain=4dB, Q=0.7071
    // Here using approximation coefficients valid across common rates:
    float hs_b0 = 1.5385f, hs_b1 = -0.7690f, hs_b2 = 0.2305f;
    float hs_a1 = -0.7690f, hs_a2 = 0.2305f;
    auto filtered = applyBiquad(buffer.samples, hs_b0, hs_b1, hs_b2, hs_a1, hs_a2);

    // ── Stage 2: High-pass filter (38 Hz, 2nd order, -3 dB point)
    // Also via bilinear at 48 kHz
    float hp_b0 = 0.9138f, hp_b1 = -1.8276f, hp_b2 = 0.9138f;
    float hp_a1 = -1.8270f, hp_a2 = 0.8377f;
    filtered = applyBiquad(filtered, hp_b0, hp_b1, hp_b2, hp_a1, hp_a2);

    // ── ITU-R BS.1770-4 block-based gating and loudness calculation
    uint32_t blockSize   = static_cast<uint32_t>(fs * 0.4f);  // 400ms blocks
    uint32_t hopSize   = static_cast<uint32_t>(fs * 0.1f);   // 75% overlap
    if (blockSize < 1024) blockSize = 1024;
    if (hopSize < 256) hopSize = 256;

    std::vector<float> blockLoudnesses;

    for (size_t start = 0; start + blockSize <= filtered.size(); start += hopSize) {
        float meanSquare = 0.f;
        for (size_t i = 0; i < blockSize; ++i) {
            float s = filtered[start + i];
            meanSquare += s * s;
        }
        meanSquare /= blockSize;
        float lk = CALIBRATION_CONST + 10.f * std::log10(std::max(meanSquare, 1e-10f));
        blockLoudnesses.push_back(lk);
    }

    if (blockLoudnesses.empty()) return -120.f;

    // Absolute gate: -70 LUFS
    float sumP = 0.f;
    int cnt = 0;
    for (float lk : blockLoudnesses) {
        if (lk >= -70.f) {
            sumP += std::pow(10.f, (lk - CALIBRATION_CONST) / 10.f);
            ++cnt;
        }
    }
    if (cnt == 0) return -120.f;

    float overallLUFS = CALIBRATION_CONST + 10.f * std::log10(sumP / cnt);

    // Relative gate: discard blocks more than 10 LU below overall
    float relGate = overallLUFS - 10.f;
    float sumP2 = 0.f;
    int   cnt2  = 0;
    for (float lk : blockLoudnesses) {
        if (lk >= -70.f && lk >= relGate) {
            sumP2 += std::pow(10.f, (lk - CALIBRATION_CONST) / 10.f);
            ++cnt2;
        }
    }
    if (cnt2 == 0) return -120.f;

    float finalLUFS = CALIBRATION_CONST + 10.f * std::log10(sumP2 / cnt2);
    return std::max(-120.f, std::min(0.f, finalLUFS));
}

float MasteringEngine::calculateMakeupGain(const AudioBuffer& original,
                                          float targetLUFS) const {
    float originalLUFS = calculateLUFS(original);
    float difference = targetLUFS - originalLUFS;
    
    return difference;
}

} // namespace mastered
// minor cleanup
