#include "mastering_engine.h"
#include <cmath>
#include <sstream>
#include <iomanip>
#include <stdexcept>

namespace mastered {

MasteringEngine::MasteringEngine(const MasteringConfig& config)
    : config_(config),
      fftAnalyzer_(std::make_unique<FFTAnalyzer>(8192, 44100)),
      eqCalculator_(std::make_unique<EQCalculator>()),
      spectrumMatcher_(std::make_unique<SpectrumMatcher>()) {
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
    
    // Apply each parametric EQ band using second-order IIR filtering
    for (const auto& band : eqCurve.bands) {
        output.samples = applyEQBand(output.samples, band, output.sampleRate);
    }
    
    // Apply makeup gain
    if (makeupGain != 0.f) {
        float gainLinear = std::pow(10.f, makeupGain / 20.f);
        for (float& sample : output.samples) {
            sample *= gainLinear;
        }
    }
    
    // Prevent clipping
    float maxAbs = 0.f;
    for (float sample : output.samples) {
        maxAbs = std::max(maxAbs, std::abs(sample));
    }
    
    if (maxAbs > 1.f) {
        float scale = 0.99f / maxAbs;
        for (float& sample : output.samples) {
            sample *= scale;
        }
    }
    
    return output;
}

std::vector<float> MasteringEngine::applyEQBand(const std::vector<float>& samples,
                                                 const EQBand& band,
                                                 uint32_t sampleRate) {
    if (samples.empty() || band.gain == 0.f) {
        return samples;
    }
    
    // Calculate second-order filter coefficients
    float A = std::pow(10.f, band.gain / 40.f);
    float w0 = 2.f * M_PI * band.frequency / sampleRate;
    float sinW0 = std::sin(w0);
    float cosW0 = std::cos(w0);
    float alpha = sinW0 / (2.f * band.qFactor);
    
    // Peaking filter coefficients
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
    
    // Apply biquad filter (Direct Form II)
    std::vector<float> output(samples.size());
    float w_n1 = 0.f, w_n2 = 0.f;
    
    for (size_t i = 0; i < samples.size(); ++i) {
        float w_n = samples[i] - a1 * w_n1 - a2 * w_n2;
        output[i] = b0 * w_n + b1 * w_n1 + b2 * w_n2;
        
        w_n2 = w_n1;
        w_n1 = w_n;
    }
    
    return output;
}

std::string MasteringEngine::exportEQasJSON(const MasteringResult& result) const {
    std::ostringstream json;
    json << std::fixed << std::setprecision(2);
    
    json << "{\n";
    json << "  \"success\": " << (result.success ? "true" : "false") << ",\n";
    json << "  \"message\": \"" << result.message << "\",\n";
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
        json << "      \"frequency\": " << band.frequency << ",\n";
        json << "      \"gain\": " << band.gain << ",\n";
        json << "      \"q\": " << band.qFactor << ",\n";
        json << "      \"type\": \"" << band.type << "\"\n";
        json << "    }";
        if (i < result.eqCurve.bands.size() - 1) json << ",";
        json << "\n";
    }
    json << "  ],\n";
    
    json << "  \"curveResponse\": [";
    for (size_t i = 0; i < result.eqCurve.curveResponse.size(); ++i) {
        if (i % 10 == 0) json << "\n    ";
        json << result.eqCurve.curveResponse[i];
        if (i < result.eqCurve.curveResponse.size() - 1) json << ", ";
    }
    json << "\n  ]\n";
    json << "}\n";
    
    return json.str();
}

std::string MasteringEngine::exportEQasReaEQ(const MasteringResult& result) const {
    std::ostringstream reaEQ;
    
    reaEQ << "<ReaEQ " << result.eqCurve.bands.size() << " bands>\n";
    
    for (const auto& band : result.eqCurve.bands) {
        // ReaEQ format: freq=1000 gain=3.0 bw=1.0 type=peak
        reaEQ << "Band: freq=" << std::fixed << std::setprecision(1) << band.frequency
              << " gain=" << band.gain
              << " bw=" << band.qFactor
              << " type=" << band.type << "\n";
    }
    
    return reaEQ.str();
}

std::string MasteringEngine::exportEQasEqualizerAPO(const MasteringResult& result) const {
    std::ostringstream apoEQ;
    
    for (const auto& band : result.eqCurve.bands) {
        apoEQ << "Filter: ON PK Fc=" << std::fixed << std::setprecision(1) << band.frequency
              << " Gain=" << band.gain
              << " Q=" << band.qFactor << "\n";
    }
    
    return apoEQ.str();
}

float MasteringEngine::calculateLUFS(const AudioBuffer& buffer) const {
    if (buffer.samples.empty()) return 0.f;
    
    float sumSquares = 0.f;
    for (float sample : buffer.samples) {
        sumSquares += sample * sample;
    }
    
    float rms = std::sqrt(sumSquares / buffer.samples.size());
    float lufs = -0.691f + 10.f * std::log10(std::max(rms * rms, 1e-10f));
    
    return lufs;
}

float MasteringEngine::calculateMakeupGain(const AudioBuffer& original,
                                          float targetLUFS) const {
    float originalLUFS = calculateLUFS(original);
    float difference = targetLUFS - originalLUFS;
    
    return difference;
}

} // namespace mastered
