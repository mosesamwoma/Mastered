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
    
    // Validate input parameters
    if (band.frequency <= 0.f) {
        return response;
    }
    if (band.qFactor <= 0.01f) {
        throw std::runtime_error("Q factor must be > 0.01");
    }
    if (sampleRate == 0) {
        throw std::runtime_error("Sample rate must be > 0");
    }
    
    // Second-order peaking EQ filter (Robert Bristow-Johnson formulas)
    float A = std::pow(10.f, band.gain / 40.f);
    float w0 = 2.f * M_PI * band.frequency / sampleRate;
    float sinW0 = std::sin(w0);
    float cosW0 = std::cos(w0);
    float alpha = sinW0 / (2.f * band.qFactor);
    
    // Correct peaking EQ filter coefficients
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
    
    // Calculate magnitude response at each frequency
    for (size_t i = 0; i < frequencies.size(); ++i) {
        if (frequencies[i] <= 0.f) {
            response[i] = 0.f;
            continue;
        }
        
        float w = 2.f * M_PI * frequencies[i] / sampleRate;
        float cosw = std::cos(w);
        float sinw = std::sin(w);
        float cos2w = std::cos(2.f * w);
        float sin2w = std::sin(2.f * w);
        
        float numeratorReal = b0 + b1 * cosw + b2 * cos2w;
        float numeratorImag = b1 * sinw + b2 * sin2w;
        
        float denominatorReal = 1.f + a1 * cosw + a2 * cos2w;
        float denominatorImag = a1 * sinw + a2 * sin2w;
        
        // Calculate magnitudes with numerical stability checks
        float numeratorMag = std::sqrt(numeratorReal * numeratorReal + numeratorImag * numeratorImag);
        float denominatorMag = std::sqrt(denominatorReal * denominatorReal + denominatorImag * denominatorImag);
        
        if (denominatorMag < 1e-10f) {
            response[i] = 0.f;
        } else {
            float magnitude = numeratorMag / denominatorMag;
            response[i] = 20.f * std::log10(std::max(magnitude, 1e-10f));
        }
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
    
    // A-weighting curve for human hearing sensitivity (with numerical stability)
    for (size_t i = 0; i < weighted.size(); ++i) {
        float f = frequencies[i];
        
        if (f < 1.f) {
            weighted[i] = spectrum[i] - 100.f;  // Very low frequencies get large attenuation
            continue;
        }
        
        // A-weighting formula (dB) - with improved numerical stability
        float f2 = f * f;
        float f4 = f2 * f2;
        
        // Use log-space calculation to avoid overflow/underflow
        float c1 = 20.6f;
        float c2 = 107.7f;
        float c3 = 737.9f;
        float c4 = 12200.f;
        
        // Calculate using safer formulation
        float numerator = c4 * c4 * f4;
        
        // Terms in denominator - compute carefully
        float term1 = f2 + c1 * c1;
        float term2_inner1 = f2 + c2 * c2;
        float term2_inner2 = f2 + c3 * c3;
        float term3 = f2 + c4 * c4;
        
        // Prevent overflow: use log-sum instead of direct multiplication
        float log_denominator = std::log(term1) + 0.5f * (std::log(term2_inner1) + std::log(term2_inner2)) + std::log(term3);
        float log_numerator = std::log(numerator + 1e-20f);
        
        float ratio = log_numerator - log_denominator;
        float aWeight = 20.f * ratio + 2.f;  // Equivalent to 20*log10(exp(ratio)) + 2
        
        // Clamp to reasonable range to prevent NaN propagation
        aWeight = std::max(-100.f, std::min(20.f, aWeight));
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
