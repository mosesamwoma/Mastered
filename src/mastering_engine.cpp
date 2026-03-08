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
      fftAnalyzer_(std::make_unique<FFTAnalyzer>(constants::DEFAULT_FFT_SIZE, constants::DEFAULT_SAMPLE_RATE)),
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
    
    try {
        // Validate inputs
        validateAudioBuffer(reference, "Reference");
        validateAudioBuffer(unmastered, "Unmastered");
        validateConfig(config_);
        
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
        
        auto refSpectrum = fftAnalyzer_->getAveragedSpectrum(reference.samples);
        auto targetSpectrum = fftAnalyzer_->getAveragedSpectrum(unmastered.samples);
        
        refSpectrum = fftAnalyzer_->smoothSpectrum(refSpectrum, 7);
        targetSpectrum = fftAnalyzer_->smoothSpectrum(targetSpectrum, 7);
        
        result.matchingStats = spectrumMatcher_->matchSpectra(refSpectrum, targetSpectrum);
        
        result.eqCurve = eqCalculator_->calculateEQFromReference(refSpectrum, targetSpectrum);
        
        for (auto& band : result.eqCurve.bands) {
            band.gain *= config_.aggressiveness;
        }
        for (float& val : result.eqCurve.curveResponse) {
            val *= config_.aggressiveness;
        }
        
        result.estimatedLUFS = calculateLUFS(unmastered);
        
        if (config_.autoGain) {
            result.makeupGain = calculateMakeupGain(unmastered, config_.targetLoudnessLUFS);
        }
        
        result.success = true;
        result.message = "Analysis completed successfully";
        
        if (config_.verbose) {
            std::cerr << "✓ FFT Size: " << constants::DEFAULT_FFT_SIZE << "\n";
            std::cerr << "✓ Sample Rate: " << reference.sampleRate << " Hz\n";
            std::cerr << "✓ EQ Bands: " << result.eqCurve.bands.size() << "\n";
        }
        
    } catch (const std::exception& e) {
        result.success = false;
        result.message = std::string("Analysis failed: ") + e.what();
    }
    
    return result;
}

AudioBuffer MasteringEngine::applyMastering(const AudioBuffer& input, const EQCurve& eqCurve, float makeupGain) {
    AudioBuffer output = input;
    
    if (output.samples.empty()) {
        throw std::runtime_error("Cannot apply mastering to empty buffer");
    }
    
    // Apply each parametric EQ band using second-order IIR filtering (in-place)
    for (const auto& band : eqCurve.bands) {
        if (band.gain != 0.f && band.frequency > 0.f && band.qFactor > 0.01f) {
            applyEQBandInPlace(output.samples, band, output.sampleRate);
        }
    }
    
    // Apply makeup gain
    if (makeupGain != 0.f) {
        float gainLinear = std::pow(10.f, makeupGain / 20.f);
        for (float& sample : output.samples) {
            sample *= gainLinear;
        }
    }
    
    // Prevent clipping with configurable headroom
    float maxAbs = 0.f;
    for (float sample : output.samples) {
        maxAbs = std::max(maxAbs, std::abs(sample));
    }
    
    if (maxAbs > 1.f) {
        float scale = config_.clippingHeadroom / maxAbs;
        for (float& sample : output.samples) {
            sample *= scale;
        }
        if (config_.verbose) {
            std::cerr << "⚠ Clipping prevention: scaled by " << scale << "\n";
        }
    }
    
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
    
    // Simplified A-weighting curve (dB)
    float numerator = 12194.217f * f4;
    float denominator = (f2 + 20.598997f * f2) * 
                       (f2 + 107.65265f * f2) * 
                       (f2 + 737.86223f * f2) * 
                       (f2 + 12194.217f * f2);
    
    if (denominator < 1e-10f) return 0.f;
    
    float Af = 20.f * std::log10(numerator / std::sqrt(denominator)) + 2.f;
    
    return std::pow(10.f, Af / 10.f);  // Convert back to linear
}

// K-weighting for LUFS calculation (simplified)
float MasteringEngine::getKWeighting(float frequencyHz) const {
    if (frequencyHz <= 0.f) return 0.f;
    
    // K-weighting emphasizes mids/highs more than A-weighting
    // Simplified implementation
    float f = frequencyHz;
    
    // High shelf: boost above 2kHz
    float highShelf = 1.f;
    if (f > 2000.f) {
        float ratio = f / 2000.f;
        highShelf = 1.f + 0.5f * (ratio - 1.f);  // Gradual boost
    }
    
    // Low shelf: attenuate below 100Hz
    float lowShelf = 1.f;
    if (f < 100.f) {
        float ratio = f / 100.f;
        lowShelf = ratio * ratio;  // Gentle rolloff
    }
    
    return highShelf * lowShelf;
}

float MasteringEngine::calculateLUFS(const AudioBuffer& buffer) const {
    if (buffer.samples.empty()) return -120.f;
    
    // ITU-R BS.1770-4 simplified implementation
    // Full implementation would use block-based analysis and proper gating
    
    float weightedSum = 0.f;
    uint32_t sampleRate = buffer.sampleRate;
    
    // Analyze in blocks for better accuracy
    uint32_t blockSize = sampleRate / 10;  // 100ms blocks
    if (blockSize < 1024) blockSize = 1024;
    
    int numBlocks = 0;
    float maxBlockLoudness = -120.f;
    
    for (size_t blockStart = 0; blockStart < buffer.samples.size(); blockStart += blockSize) {
        size_t blockEnd = std::min(blockStart + blockSize, buffer.samples.size());
        float blockSum = 0.f;
        
        // Calculate mean square with K-weighting
        for (size_t i = blockStart; i < blockEnd; ++i) {
            float sample = buffer.samples[i];
            
            // Approximate frequency from bin (simplified)
            // In practice, use FFT for proper frequency analysis
            float freq = 1000.f;  // Nominal frequency
            float weight = config_.perceptualWeighting ? getKWeighting(freq) : 1.f;
            
            blockSum += weight * sample * sample;
        }
        
        float blockMeanSquare = blockSum / (blockEnd - blockStart);
        if (blockMeanSquare < 1e-10f) continue;
        
        float blockLoudness = -23.f + 10.f * std::log10(blockMeanSquare);
        maxBlockLoudness = std::max(maxBlockLoudness, blockLoudness);
        
        weightedSum += blockMeanSquare;
        numBlocks++;
    }
    
    if (numBlocks == 0 || weightedSum < 1e-10f) {
        return -120.f;
    }
    
    float meanSquare = weightedSum / numBlocks;
    float lufs = -23.f + 10.f * std::log10(std::max(meanSquare, 1e-10f));
    
    return lufs;
}

float MasteringEngine::calculateMakeupGain(const AudioBuffer& original,
                                          float targetLUFS) const {
    float originalLUFS = calculateLUFS(original);
    float difference = targetLUFS - originalLUFS;
    
    return difference;
}

} // namespace mastered
