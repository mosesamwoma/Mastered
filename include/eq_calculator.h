#ifndef EQ_CALCULATOR_H
#define EQ_CALCULATOR_H

#include <vector>
#include <memory>
#include "fft_analyzer.h"

namespace mastered {

struct EQBand {
    float frequency;    // Center frequency in Hz
    float gain;         // Gain in dB (-24 to +24)
    float qFactor;      // Q factor (bandwidth control)
    std::string type;   // "peak", "shelf-low", "shelf-high", "notch"
    
    EQBand(float f = 1000.f, float g = 0.f, float q = 1.f, const std::string& t = "peak")
        : frequency(f), gain(g), qFactor(q), type(t) {}
};

struct EQCurve {
    std::vector<EQBand> bands;
    std::vector<float> curveResponse;  // Magnitude response at each frequency
    std::vector<float> frequencies;    // Frequency points
    
    float totalGain;  // Overall makeup gain in dB
    
    EQCurve() : totalGain(0.f) {}
};

class EQCalculator {
public:
    EQCalculator() = default;
    ~EQCalculator() = default;
    
    /**
     * Calculate optimal EQ curve by comparing reference and target spectra
     * Uses perceptual weighting and spectral matching
     */
    EQCurve calculateEQFromReference(const Spectrum& referenceSpectrum, 
                                      const Spectrum& targetSpectrum);
    
    /**
     * Generate parametric EQ bands from difference curve
     * Finds peaks and dips and creates isolated bands
     */
    std::vector<EQBand> generateParametricBands(const std::vector<float>& differenceCurve,
                                                 const std::vector<float>& frequencies,
                                                 uint32_t maxBands = 8);
    
    /**
     * Calculate magnitude response of a single EQ band at given frequencies
     */
    static std::vector<float> getBandResponse(const EQBand& band,
                                              const std::vector<float>& frequencies,
                                              uint32_t sampleRate);
    
    /**
     * Combine multiple EQ bands into single response curve
     */
    static std::vector<float> combineBands(const std::vector<EQBand>& bands,
                                           const std::vector<float>& frequencies,
                                           uint32_t sampleRate);
    
    /**
     * Apply A-weighting (human hearing sensitivity correction)
     */
    static std::vector<float> applyAWeighting(const std::vector<float>& spectrum,
                                              const std::vector<float>& frequencies);
    
    /**
     * Detect and suppress extreme peaks (prevent over-compensation)
     */
    static std::vector<float> suppressExtremes(const std::vector<float>& curve, float threshold = 12.f);
    
    /**
     * Smooth curve to prevent picky/thin results
     */
    static std::vector<float> smoothCurve(const std::vector<float>& curve, uint32_t smoothing = 5);

private:
    /**
     * Find local peaks in a curve
     */
    static std::vector<size_t> findPeaks(const std::vector<float>& curve, float threshold = 1.f);
    
    /**
     * Find local dips in a curve
     */
    static std::vector<size_t> findDips(const std::vector<float>& curve, float threshold = 1.f);
    
    /**
     * Calculate perceptual loudness from spectrum (loudness units)
     */
    static float calculateLoudness(const std::vector<float>& magnitudeSpec,
                                   const std::vector<float>& frequencies);
};

} // namespace mastered

#endif // EQ_CALCULATOR_H
