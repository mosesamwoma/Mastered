#include "spectrum_matcher.h"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace mastered {

MatchingResult SpectrumMatcher::matchSpectra(const Spectrum& referenceSpectrum,
                                            const Spectrum& targetSpectrum) {
    MatchingResult result;
    
    if (referenceSpectrum.magnitude.size() != targetSpectrum.magnitude.size()) {
        throw std::runtime_error("Spectra must have identical size");
    }
    
    // Calculate raw frequency-by-frequency correction
    auto correction = calculateFrequencyCorrection(referenceSpectrum.magnitude,
                                                  targetSpectrum.magnitude);
    
    // Apply confidence weighting based on frequency bands
    auto weighted = applyConfidenceWeighting(correction, referenceSpectrum.frequencies);
    
    // Limit aggressive corrections
    auto limited = limitCorrection(weighted, 12.f);
    
    result.correction = limited;
    
    // Calculate correlation coefficient
    result.correlation = calculateSpectralCorrelation(referenceSpectrum.magnitude,
                                                      targetSpectrum.magnitude);
    
    // Calculate RMS spectral difference
    float rmsSum = 0.f;
    for (size_t i = 0; i < correction.size(); ++i) {
        rmsSum += correction[i] * correction[i];
    }
    result.spectralDifference = std::sqrt(rmsSum / correction.size());
    
    // Confidence score based on correlation and difference
    result.confidenceScore = result.correlation * (1.f - result.spectralDifference / 20.f);
    result.confidenceScore = std::max(0.f, std::min(1.f, result.confidenceScore));
    
    return result;
}

std::vector<float> SpectrumMatcher::calculateFrequencyCorrection(const std::vector<float>& referenceDb,
                                                                 const std::vector<float>& targetDb) {
    std::vector<float> correction(referenceDb.size());
    
    for (size_t i = 0; i < referenceDb.size(); ++i) {
        correction[i] = referenceDb[i] - targetDb[i];
    }
    
    return correction;
}

std::vector<float> SpectrumMatcher::interpolateCorrection(const std::vector<float>& correction,
                                                          const std::vector<float>& fromFreqs,
                                                          const std::vector<float>& toFreqs) {
    std::vector<float> interpolated(toFreqs.size());
    
    if (correction.empty() || fromFreqs.empty() || toFreqs.empty() || 
        correction.size() != fromFreqs.size()) {
        return interpolated;  // Return empty/zero values for invalid input
    }
    
    // Validate that fromFreqs is sorted
    for (size_t j = 1; j < fromFreqs.size(); ++j) {
        if (fromFreqs[j] < fromFreqs[j-1]) {
            throw std::runtime_error("Frequency array must be sorted in ascending order");
        }
    }
    
    for (size_t i = 0; i < toFreqs.size(); ++i) {
        float targetFreq = toFreqs[i];
        
        // Clamp to boundaries
        if (targetFreq <= fromFreqs.front()) {
            interpolated[i] = correction.front();
            continue;
        }
        if (targetFreq >= fromFreqs.back()) {
            interpolated[i] = correction.back();
            continue;
        }
        
        // Binary search for efficiency (O(log n) instead of O(n))
        auto it = std::lower_bound(fromFreqs.begin(), fromFreqs.end(), targetFreq);
        
        // Handle boundary cases
        if (it == fromFreqs.end()) {
            interpolated[i] = correction.back();
            continue;
        }
        if (it == fromFreqs.begin()) {
            interpolated[i] = correction.front();
            continue;
        }
        
        // Linear interpolation between two points
        size_t idx = std::distance(fromFreqs.begin(), it) - 1;
        float denom = fromFreqs[idx + 1] - fromFreqs[idx];
        
        if (denom < 1e-10f) {
            interpolated[i] = correction[idx];
        } else {
            float ratio = (targetFreq - fromFreqs[idx]) / denom;
            interpolated[i] = correction[idx] * (1.f - ratio) + correction[idx + 1] * ratio;
        }
    }
    
    return interpolated;
}

float SpectrumMatcher::calculateSpectralCorrelation(const std::vector<float>& spec1,
                                                   const std::vector<float>& spec2) {
    if (spec1.size() != spec2.size() || spec1.empty()) {
        return 0.f;
    }
    
    // Calculate means
    float mean1 = std::accumulate(spec1.begin(), spec1.end(), 0.f) / spec1.size();
    float mean2 = std::accumulate(spec2.begin(), spec2.end(), 0.f) / spec2.size();
    
    // Calculate covariance and standard deviations
    float covariance = 0.f;
    float var1 = 0.f;
    float var2 = 0.f;
    
    for (size_t i = 0; i < spec1.size(); ++i) {
        float diff1 = spec1[i] - mean1;
        float diff2 = spec2[i] - mean2;
        
        covariance += diff1 * diff2;
        var1 += diff1 * diff1;
        var2 += diff2 * diff2;
    }
    
    covariance /= spec1.size();
    var1 /= spec1.size();
    var2 /= spec2.size();
    
    // Calculate correlation coefficient
    float stddev1 = std::sqrt(var1);
    float stddev2 = std::sqrt(var2);
    
    if (stddev1 < 1e-10f || stddev2 < 1e-10f) {
        return 0.f;
    }
    
    float correlation = covariance / (stddev1 * stddev2);
    return std::max(-1.f, std::min(1.f, correlation));
}

std::vector<float> SpectrumMatcher::applyConfidenceWeighting(const std::vector<float>& correction,
                                                             const std::vector<float>& frequencies) {
    std::vector<float> weighted = correction;
    
    // Weight corrections based on frequency (more confident in mid-range)
    for (size_t i = 0; i < weighted.size(); ++i) {
        float weight = getFrequencyWeight(frequencies[i]);
        weighted[i] *= weight;
    }
    
    return weighted;
}

std::vector<float> SpectrumMatcher::limitCorrection(const std::vector<float>& correction,
                                                    float maxGain) {
    std::vector<float> limited = correction;
    
    for (float& value : limited) {
        if (value > maxGain) {
            value = maxGain;
        } else if (value < -maxGain) {
            value = -maxGain;
        }
    }
    
    return limited;
}

float SpectrumMatcher::getFrequencyWeight(float frequency) {
    // Bell curve weight, peak confidence at 1-4 kHz
    float optimalFreq = 2000.f;
    float bandwidth = 3000.f;
    
    float exponent = -2.f * ((frequency - optimalFreq) * (frequency - optimalFreq)) / 
                     (bandwidth * bandwidth);
    
    return std::max(0.2f, std::exp(exponent));
}

} // namespace mastered
