#include "eq_calculator.h"
#include "mastering_engine.h"
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
    
    // Optimized band generation algorithm with three improvements:
    // 1. Adaptive Q-factor calculation based on peak/dip width and magnitude
    // 2. Significance-based sorting to keep most important bands
    // 3. Filtering of adjacent redundant peaks/dips
    
    // Find peaks (boost) and dips (cut) using optimized detection
    auto peaks = findPeaks(differenceCurve, constants::PEAK_DETECTION_THRESHOLD);
    auto dips = findDips(differenceCurve, constants::PEAK_DETECTION_THRESHOLD);
    
    // Create candidate bands from peaks and dips with adaptive Q-factors
    std::vector<std::pair<float, EQBand>> candidateBands;  // (significance, band)
    
    // Process peaks (boosts) with adaptive Q-factor calculation
    for (size_t idx : peaks) {
        if (idx < frequencies.size()) {
            float freq = frequencies[idx];
            float gain = differenceCurve[idx];
            
            // Skip very small gains (below threshold)
            if (std::abs(gain) < constants::MIN_GAIN_THRESHOLD) continue;
            
            // Adaptive Q-factor: Q increases with gain magnitude for narrower bands
            // This creates focused EQ bands for strong resonances, broader bands for gentle curves
            // Formula: Q = 0.7 + (gain_magnitude / 10), clamped to 0.5-4.0 range
            float qFactor = 0.7f + (std::abs(gain) / 10.f);
            qFactor = std::max(0.5f, std::min(qFactor, 4.0f));
            
            // Determine band type: shelves for extreme frequencies, peaks for mid-range
            std::string bandType = "peak";
            if (freq < 200.f) {
                qFactor = 0.7f;  // Shelves use fixed low Q for smooth slopes
                bandType = "shelf-low";
            } else if (freq > 10000.f) {
                qFactor = 0.7f;
                bandType = "shelf-high";
            }
            
            EQBand band(freq, gain, qFactor, bandType);
            float significance = std::abs(gain);  // Significance = gain magnitude
            candidateBands.emplace_back(significance, band);
        }
    }
    
    // Process dips (cuts) with same adaptive Q-factor strategy
    for (size_t idx : dips) {
        if (idx < frequencies.size()) {
            float freq = frequencies[idx];
            float gain = differenceCurve[idx];
            
            if (std::abs(gain) < constants::MIN_GAIN_THRESHOLD) continue;
            
            // Same Q-factor adaptation for dips
            float qFactor = 0.7f + (std::abs(gain) / 10.f);
            qFactor = std::max(0.5f, std::min(qFactor, 4.0f));
            
            std::string bandType = "peak";
            if (freq < 200.f) {
                qFactor = 0.7f;
                bandType = "shelf-low";
            } else if (freq > 10000.f) {
                qFactor = 0.7f;
                bandType = "shelf-high";
            }
            
            EQBand band(freq, gain, qFactor, bandType);
            float significance = std::abs(gain);
            candidateBands.emplace_back(significance, band);
        }
    }
    
    // Sort candidate bands by significance (highest impact first)
    // This ensures we keep only the most important bands when limited by maxBands
    std::sort(candidateBands.begin(), candidateBands.end(),
        [](const auto& a, const auto& b) {
            return a.first > b.first;  // Descending order: highest significance first
        });
    
    // Select top maxBands bands, ranked by significance
    // This prioritizes correcting the most obvious tonal issues
    for (size_t i = 0; i < candidateBands.size() && bands.size() < maxBands; ++i) {
        bands.push_back(candidateBands[i].second);
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
    std::vector<float> weighted;
    weighted.reserve(spectrum.size());

    // A-weighting used as a normalised perceptual multiplier (not additive dB offset).
    // Normalised to 1.0 at 1 kHz so mid-range corrections pass through unchanged.
    // Low-frequency and extreme high-frequency corrections are de-emphasised.
    constexpr float F1 = 20.6f;
    constexpr float F2 = 107.7f;
    constexpr float F3 = 737.9f;
    constexpr float F4 = 12200.f;

    auto aWeightRaw = [&](float f) -> float {
        if (f <= 0.f) {
            return 0.f;
        }
        const float f2 = f * f;
        const float f4 = f2 * f2;
        const float num = (12200.f * 12200.f) * f4;
        const float den = (f2 + F1 * F1) * (f2 + F2 * F2) * (f2 + F3 * F3) * (f2 + F4 * F4);
        return (den > 1e-30f) ? (num / std::sqrt(den)) : 0.f;
    };

    // Normalise so weight = 1.0 at 1 kHz
    float ref = aWeightRaw(1000.f);
    if (ref <= 0.f) {
        ref = 1.f;
    }

    for (size_t i = 0; i < spectrum.size(); ++i) {
        float weight = aWeightRaw(frequencies[i]) / ref;
        // Clamp: allow up to 3x boost for high-freq corrections, floor at 0.05
        weight = std::max(0.05f, std::min(weight, 3.0f));
        weighted.push_back(spectrum[i] * weight);
    }

    return weighted;
}

std::vector<float> EQCalculator::suppressExtremes(const std::vector<float>& curve, float maxGainDb) {
    // Hard cap to prevent over-compensation
    // Caller passing 12.f expects a 12 dB cap, not 9.6 dB
    std::vector<float> suppressed = curve;
    
    for (float& s : suppressed) {
        s = std::max(-maxGainDb, std::min(maxGainDb, s));
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
    
    if (curve.size() < 3) return peaks;
    
    // Optimized peak detection algorithm:
    // 1. Find all local maxima (where center > both neighbors and > threshold)
    // 2. Filter out redundant adjacent peaks (keep only the strongest)
    // This prevents multiple EQ bands from being placed on broadband resonances
    
    // First pass: find all local maxima with improved robustness
    // Check wider neighborhood (±1 sample) for more stable detection
    for (size_t i = 1; i < curve.size() - 1; ++i) {
        float center = curve[i];
        
        // Robust local maximum detection: center > neighbors AND meets threshold
        if (center > curve[i - 1] && center > curve[i + 1] && center > threshold) {
            peaks.push_back(i);
        }
    }
    
    // Second pass: remove adjacent peaks (keep only the stronger one)
    // This prevents redundant bands for broadband resonances
    std::vector<size_t> filteredPeaks;
    for (size_t pk : peaks) {
        bool isRedundant = false;
        
        // Check if this peak is adjacent to a stronger peak
        for (size_t other : filteredPeaks) {
            if (std::abs((int)pk - (int)other) <= 1 && curve[other] > curve[pk]) {
                isRedundant = true;
                break;
            }
        }
        
        if (!isRedundant) {
            // Remove any weaker adjacent peaks already added
            filteredPeaks.erase(
                std::remove_if(filteredPeaks.begin(), filteredPeaks.end(),
                    [pk, &curve](size_t other) {
                        return std::abs((int)pk - (int)other) <= 1 && curve[pk] > curve[other];
                    }),
                filteredPeaks.end()
            );
            filteredPeaks.push_back(pk);
        }
    }
    
    return filteredPeaks;
}

std::vector<size_t> EQCalculator::findDips(const std::vector<float>& curve, float threshold) {
    std::vector<size_t> dips;
    
    if (curve.size() < 3) return dips;
    
    // Optimized dip detection: find local minima and filter adjacent ones
    // Similar to peak detection but for negative peaks (antiresonances/holes)
    // This ensures clean EQ curves with well-spaced bands
    
    // First pass: find all local minima
    for (size_t i = 1; i < curve.size() - 1; ++i) {
        float center = curve[i];
        
        // Robust local minimum detection: center < neighbors AND meets threshold
        if (center < curve[i - 1] && center < curve[i + 1] && center < -threshold) {
            dips.push_back(i);
        }
    }
    
    // Second pass: remove adjacent dips (keep only the deeper one)
    std::vector<size_t> filteredDips;
    for (size_t dip : dips) {
        bool isRedundant = false;
        
        // Check if this dip is adjacent to a deeper dip
        for (size_t other : filteredDips) {
            if (std::abs((int)dip - (int)other) <= 1 && curve[other] < curve[dip]) {
                isRedundant = true;
                break;
            }
        }
        
        if (!isRedundant) {
            // Remove any shallower adjacent dips already added
            filteredDips.erase(
                std::remove_if(filteredDips.begin(), filteredDips.end(),
                    [dip, &curve](size_t other) {
                        return std::abs((int)dip - (int)other) <= 1 && curve[dip] < curve[other];
                    }),
                filteredDips.end()
            );
            filteredDips.push_back(dip);
        }
    }
    
    return filteredDips;
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
