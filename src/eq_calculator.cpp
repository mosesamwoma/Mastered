#include "eq_calculator.h"
#include <cmath>
#include <algorithm>
#include <numeric>

namespace mastered {

EQCurve EQCalculator::calculateEQFromReference(const Spectrum& referenceSpectrum,
                                               const Spectrum& targetSpectrum) {
    EQCurve curve;
    
    if (referenceSpectrum.magnitude.size() != targetSpectrum.magnitude.size()) {
        throw std::runtime_error("Spectra must have same size");
    }
    
    std::vector<float> differenceCurve(referenceSpectrum.magnitude.size());
    for (size_t i = 0; i < differenceCurve.size(); ++i) {
        differenceCurve[i] = referenceSpectrum.magnitude[i] - targetSpectrum.magnitude[i];
    }
    
    auto weighted = applyAWeighting(differenceCurve, referenceSpectrum.frequencies);
    auto suppressed = suppressExtremes(weighted, 12.f);
    auto smoothed = smoothCurve(suppressed, 7);
    
    curve.curveResponse = smoothed;
    curve.frequencies = referenceSpectrum.frequencies;
    curve.bands = generateParametricBands(smoothed, referenceSpectrum.frequencies, 8);
    
    float totalDifference = 0.f;
    for (float diff : smoothed) {
        totalDifference += std::abs(diff);
    }
    curve.totalGain = totalDifference / smoothed.size() * 0.5f;
    
    return curve;
}

std::vector<EQBand> EQCalculator::generateParametricBands(const std::vector<float>& differenceCurve,
                                                          const std::vector<float>& frequencies,
                                                          uint32_t maxBands) {
    std::vector<EQBand> bands;
    
    if (differenceCurve.empty() || frequencies.empty()) {
        return bands;
    }
    
    // Find peaks (boost) and dips (cut)
    auto peaks = findPeaks(differenceCurve, 0.5f);
    auto dips = findDips(differenceCurve, 0.5f);
    
    // Create bands for significant peaks
    for (size_t idx : peaks) {
        if (bands.size() >= maxBands) break;
        
        if (idx < frequencies.size()) {
            float freq = frequencies[idx];
            float gain = differenceCurve[idx];
            
            // Skip very small gains
            if (std::abs(gain) < 0.3f) continue;
            
            float q = 2.f;  // Default Q factor
            
            // Adjust Q and type based on frequency
            if (freq < 200.f) {
                q = 0.7f;
                bands.emplace_back(freq, gain, q, "shelf-low");
            } else if (freq > 10000.f) {
                q = 0.7f;
                bands.emplace_back(freq, gain, q, "shelf-high");
            } else {
                bands.emplace_back(freq, gain, q, "peak");
            }
        }
    }
    
    // Create bands for significant dips
    for (size_t idx : dips) {
        if (bands.size() >= maxBands) break;
        
        if (idx < frequencies.size()) {
            float freq = frequencies[idx];
            float gain = differenceCurve[idx];
            
            if (std::abs(gain) < 0.3f) continue;
            
            float q = 2.f;
            
            if (freq < 200.f) {
                q = 0.7f;
                bands.emplace_back(freq, gain, q, "shelf-low");
            } else if (freq > 10000.f) {
                q = 0.7f;
                bands.emplace_back(freq, gain, q, "shelf-high");
            } else {
                bands.emplace_back(freq, gain, q, "peak");
            }
        }
    }
    
    return bands;
}

std::vector<float> EQCalculator::getBandResponse(const EQBand& band,
                                                 const std::vector<float>& frequencies,
                                                 uint32_t sampleRate) {
    std::vector<float> response(frequencies.size(), 0.f);
    
    // Simplified second-order filter response calculation
    float A = std::pow(10.f, band.gain / 40.f);
    float w0 = 2.f * M_PI * band.frequency / sampleRate;
    float alpha = std::sin(w0) / (2.f * band.qFactor);
    
    float b0 = A * (1.f + alpha * A);
    float b1 = -2.f * std::cos(w0);
    float b2 = A * (1.f - alpha * A);
    float a0 = 1.f + alpha / A;
    float a1 = -2.f * std::cos(w0);
    float a2 = 1.f - alpha / A;
    
    // Normalize
    b0 /= a0;
    b1 /= a0;
    b2 /= a0;
    a1 /= a0;
    a2 /= a0;
    
    // Calculate magnitude response at each frequency
    for (size_t i = 0; i < frequencies.size(); ++i) {
        float w = 2.f * M_PI * frequencies[i] / sampleRate;
        float cosw = std::cos(w);
        float sinw = std::sin(w);
        
        float numeratorReal = b0 + b1 * cosw + b2 * std::cos(2.f * w);
        float numeratorImag = b1 * sinw + b2 * std::sin(2.f * w);
        
        float denominatorReal = 1.f + a1 * cosw + a2 * std::cos(2.f * w);
        float denominatorImag = a1 * sinw + a2 * std::sin(2.f * w);
        
        float magnitude = std::sqrt(numeratorReal * numeratorReal + numeratorImag * numeratorImag) /
                         std::sqrt(denominatorReal * denominatorReal + denominatorImag * denominatorImag);
        
        response[i] = 20.f * std::log10(std::max(magnitude, 1e-10f));
    }
    
    return response;
}

std::vector<float> EQCalculator::combineBands(const std::vector<EQBand>& bands,
                                              const std::vector<float>& frequencies,
                                              uint32_t sampleRate) {
    std::vector<float> combined(frequencies.size(), 0.f);
    
    for (const auto& band : bands) {
        auto bandResponse = getBandResponse(band, frequencies, sampleRate);
        for (size_t i = 0; i < combined.size(); ++i) {
            combined[i] += bandResponse[i];
        }
    }
    
    return combined;
}

std::vector<float> EQCalculator::applyAWeighting(const std::vector<float>& spectrum,
                                                 const std::vector<float>& frequencies) {
    std::vector<float> weighted = spectrum;
    
    // A-weighting curve for human hearing sensitivity
    for (size_t i = 0; i < weighted.size(); ++i) {
        float f = frequencies[i];
        
        // A-weighting formula (dB)
        float f2 = f * f;
        float f4 = f2 * f2;
        
        float numerator = 12200.f * 12200.f * f4;
        float denominator = (f2 + 20.6f * 20.6f) *
                           std::sqrt((f2 + 107.7f * 107.7f) * (f2 + 737.9f * 737.9f)) *
                           (f2 + 12200.f * 12200.f);
        
        float aWeight = 20.f * std::log10(std::max(numerator / denominator, 1e-10f)) + 2.f;
        
        weighted[i] = spectrum[i] + aWeight;
    }
    
    return weighted;
}

std::vector<float> EQCalculator::suppressExtremes(const std::vector<float>& curve, float threshold) {
    std::vector<float> suppressed = curve;
    
    for (float& sample : suppressed) {
        if (sample > threshold) {
            sample = threshold * 0.8f;  // Gentle cap
        } else if (sample < -threshold) {
            sample = -threshold * 0.8f;
        }
    }
    
    return suppressed;
}

std::vector<float> EQCalculator::smoothCurve(const std::vector<float>& curve, uint32_t smoothing) {
    if (curve.size() < smoothing) {
        return curve;
    }
    
    std::vector<float> smooth(curve.size());
    uint32_t half = smoothing / 2;
    
    for (size_t i = 0; i < curve.size(); ++i) {
        float sum = 0.f;
        uint32_t count = 0;
        
        for (int32_t j = static_cast<int32_t>(i) - static_cast<int32_t>(half);
             j <= static_cast<int32_t>(i) + static_cast<int32_t>(half); ++j) {
            if (j >= 0 && j < static_cast<int32_t>(curve.size())) {
                sum += curve[j];
                count++;
            }
        }
        
        smooth[i] = sum / count;
    }
    
    return smooth;
}

std::vector<size_t> EQCalculator::findPeaks(const std::vector<float>& curve, float threshold) {
    std::vector<size_t> peaks;
    
    for (size_t i = 1; i < curve.size() - 1; ++i) {
        if (curve[i] > curve[i - 1] && curve[i] > curve[i + 1] && curve[i] > threshold) {
            peaks.push_back(i);
        }
    }
    
    return peaks;
}

std::vector<size_t> EQCalculator::findDips(const std::vector<float>& curve, float threshold) {
    std::vector<size_t> dips;
    
    for (size_t i = 1; i < curve.size() - 1; ++i) {
        if (curve[i] < curve[i - 1] && curve[i] < curve[i + 1] && curve[i] < -threshold) {
            dips.push_back(i);
        }
    }
    
    return dips;
}

float EQCalculator::calculateLoudness(const std::vector<float>& magnitudeSpec,
                                      const std::vector<float>& frequencies) {
    // Simplified loudness calculation based on A-weighted spectrum
    float loudness = 0.f;
    
    for (size_t i = 0; i < magnitudeSpec.size(); ++i) {
        float f = frequencies[i];
        
        // Convert magnitude to power and apply A-weighting
        float power = std::pow(10.f, magnitudeSpec[i] / 10.f);
        
        // Frequency weight (simplified)
        float weight = 1.f;
        if (f < 1000.f) {
            weight = 1.f - (1000.f - f) / 10000.f;
        } else if (f > 10000.f) {
            weight = 1.f - (f - 10000.f) / 10000.f;
        }
        
        loudness += power * std::max(weight, 0.1f);
    }
    
    return 10.f * std::log10(std::max(loudness, 1e-10f));
}

} // namespace mastered
