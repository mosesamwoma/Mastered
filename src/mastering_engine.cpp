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

AudioBuffer MasteringEngine::applyMastering(const AudioBuffer& input, const EQCurve& eqCurve) {
    return input;
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
