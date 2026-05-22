#ifndef SPECTRUM_MATCHER_H
#define SPECTRUM_MATCHER_H

#include <vector>
#include "fft_analyzer.h"

namespace mastered {

struct MatchingResult {
    float correlation;          // Correlation coefficient (0-1)
    float spectralDifference;   // RMS spectral difference (dB)
    std::vector<float> correction; // Correction curve (dB)
    float confidenceScore;      // 0-1, higher = more accurate
};

class SpectrumMatcher {
public:
    SpectrumMatcher() = default;
    ~SpectrumMatcher() = default;
    
    /**
     * Match target spectrum to reference using spectral matching
     * Calculates the gain adjustment needed at each frequency
     */
    MatchingResult matchSpectra(const Spectrum& referenceSpectrum,
                                const Spectrum& targetSpectrum);
    
    /**
     * Calculate per-frequency correction (logarithmic scale)
     */
    std::vector<float> calculateFrequencyCorrection(const std::vector<float>& referenceDb,
                                                     const std::vector<float>& targetDb);
    
    /**
     * Interpolate correction curve to match frequency resolution
     */
    std::vector<float> interpolateCorrection(const std::vector<float>& correction,
                                             const std::vector<float>& fromFreqs,
                                             const std::vector<float>& toFreqs);
    
    /**
     * Calculate correlation between two spectra
     */
    float calculateSpectralCorrelation(const std::vector<float>& spec1,
                                       const std::vector<float>& spec2);
    
    /**
     * Apply confidence weighting based on frequency bands
     * Confidence varies by frequency (more confident in mid-range)
     */
    std::vector<float> applyConfidenceWeighting(const std::vector<float>& correction,
                                                 const std::vector<float>& frequencies);
    
    /**
     * Limit aggressive corrections to prevent artifacts
     */
    std::vector<float> limitCorrection(const std::vector<float>& correction,
                                       float maxGain = 12.f);

private:
    /**
     * Perceptual frequency weighting for confidence
     * Returns weight (0-1) for each frequency
     */
    float getFrequencyWeight(float frequency);
};

} // namespace mastered

#endif // SPECTRUM_MATCHER_H
