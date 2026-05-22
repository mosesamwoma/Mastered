#ifndef FFT_ANALYZER_H
#define FFT_ANALYZER_H

#include <vector>
#include <complex>
#include <memory>
#include <cstdint>

namespace mastered {

struct Spectrum {
    std::vector<float> magnitude;      // Magnitude in dB (0 to ~120 dB)
    std::vector<float> phase;          // Phase in radians (-PI to PI)
    std::vector<float> frequencies;    // Frequency bins in Hz
    uint32_t fftSize;
    uint32_t sampleRate;
    
    Spectrum() : fftSize(0), sampleRate(0) {}
};

class FFTAnalyzer {
public:
    FFTAnalyzer(uint32_t fftSize = 4096, uint32_t sampleRate = 44100);
    ~FFTAnalyzer();
    
    /**
     * Perform FFT analysis on audio samples
     * Uses Hann window for spectral leakage reduction
     */
    Spectrum analyze(const std::vector<float>& samples);
    
    /**
     * Perform continuous analysis with overlapping windows
     * Returns multiple spectra for time-frequency representation
     */
    std::vector<Spectrum> analyzeStreaming(const std::vector<float>& samples, float hopRatio = 0.5f);
    
    /**
     * Get averaged spectrum across multiple frames
     */
    Spectrum getAveragedSpectrum(const std::vector<float>& samples, uint32_t numFrames = 0);
    
    /**
     * Smooth spectrum using median filter
     */
    Spectrum smoothSpectrum(const Spectrum& spec, uint32_t filterSize = 5);
    
    /**
     * Get critical band grouping (auditory model)
     */
    std::vector<float> getCriticalBands(const Spectrum& spec);
    
    void setFFTSize(uint32_t size);
    void setSampleRate(uint32_t rate);
    
    uint32_t getFFTSize() const { return fftSize_; }
    uint32_t getSampleRate() const { return sampleRate_; }

private:
    uint32_t fftSize_;
    uint32_t sampleRate_;
    
    // Hann window coefficients
    std::vector<float> hannWindow_;
    
    void generateHannWindow();
    
    /**
     * Apply Hann window to time-domain samples
     */
    std::vector<float> applyWindow(const std::vector<float>& samples);
    
    /**
     * Compute FFT using Cooley-Tukey algorithm (fallback if FFTW not available)
     */
    std::vector<std::complex<float>> computeFFT(const std::vector<float>& samples);
    
    /**
     * Convert complex FFT output to magnitude spectrum (dB)
     */
    Spectrum complexToSpectrum(const std::vector<std::complex<float>>& fftResult);
};

} // namespace mastered

#endif // FFT_ANALYZER_H
