#include "fft_analyzer.h"
#include <cmath>
#include <algorithm>
#include <numeric>
#include <stdexcept>

#ifdef USE_FFTW
    #include <fftw3f.h>
#endif

namespace mastered {

FFTAnalyzer::FFTAnalyzer(uint32_t fftSize, uint32_t sampleRate)
    : fftSize_(fftSize), sampleRate_(sampleRate) {
    if (fftSize_ == 0 || (fftSize_ & (fftSize_ - 1)) != 0) {
        throw std::runtime_error("FFT size must be a power of 2");
    }
    generateHannWindow();
}

FFTAnalyzer::~FFTAnalyzer() = default;

void FFTAnalyzer::generateHannWindow() {
    hannWindow_.resize(fftSize_);
    for (uint32_t i = 0; i < fftSize_; ++i) {
        hannWindow_[i] = 0.5f * (1.f - std::cos(2.f * M_PI * i / (fftSize_ - 1)));
    }
}

std::vector<float> FFTAnalyzer::applyWindow(const std::vector<float>& samples) {
    std::vector<float> windowed;
    windowed.reserve(fftSize_);
    
    for (uint32_t i = 0; i < fftSize_; ++i) {
        if (i < samples.size()) {
            windowed.push_back(samples[i] * hannWindow_[i]);
        } else {
            windowed.push_back(0.f);
        }
    }
    
    return windowed;
}

Spectrum FFTAnalyzer::analyze(const std::vector<float>& samples) {
    if (samples.empty()) {
        throw std::runtime_error("Cannot analyze empty sample buffer");
    }
    
    std::vector<float> windowed = applyWindow(samples);
    auto fftResult = computeFFT(windowed);
    return complexToSpectrum(fftResult);
}

std::vector<Spectrum> FFTAnalyzer::analyzeStreaming(const std::vector<float>& samples, float hopRatio) {
    std::vector<Spectrum> spectra;
    
    if (samples.empty() || hopRatio <= 0.f || hopRatio >= 1.f) {
        return spectra;
    }
    
    uint32_t hopSize = static_cast<uint32_t>(fftSize_ * hopRatio);
    if (hopSize == 0) hopSize = 1;
    
    for (size_t offset = 0; offset + fftSize_ <= samples.size(); offset += hopSize) {
        std::vector<float> frame(samples.begin() + offset, samples.begin() + offset + fftSize_);
        spectra.push_back(analyze(frame));
    }
    
    return spectra;
}

Spectrum FFTAnalyzer::getAveragedSpectrum(const std::vector<float>& samples, uint32_t numFrames) {
    auto spectra = analyzeStreaming(samples);
    
    if (spectra.empty()) {
        throw std::runtime_error("No frames available for averaging");
    }
    
    if (numFrames == 0 || numFrames > spectra.size()) {
        numFrames = spectra.size();
    }
    
    Spectrum averaged = spectra[0];
    averaged.magnitude.assign(averaged.magnitude.size(), 0.f);
    
    for (uint32_t i = 0; i < numFrames; ++i) {
        for (size_t j = 0; j < spectra[i].magnitude.size(); ++j) {
            averaged.magnitude[j] += spectra[i].magnitude[j];
        }
    }
    
    for (float& mag : averaged.magnitude) {
        mag /= numFrames;
    }
    
    return averaged;
}

Spectrum FFTAnalyzer::smoothSpectrum(const Spectrum& spec, uint32_t filterSize) {
    Spectrum smoothed = spec;
    
    if (filterSize < 2 || filterSize > spec.magnitude.size()) {
        return smoothed;
    }
    
    std::vector<float> smooth(spec.magnitude.size());
    uint32_t halfFilter = filterSize / 2;
    
    for (size_t i = 0; i < spec.magnitude.size(); ++i) {
        float sum = 0.f;
        uint32_t count = 0;
        
        for (int32_t j = static_cast<int32_t>(i) - static_cast<int32_t>(halfFilter);
             j <= static_cast<int32_t>(i) + static_cast<int32_t>(halfFilter); ++j) {
            if (j >= 0 && j < static_cast<int32_t>(spec.magnitude.size())) {
                sum += spec.magnitude[j];
                count++;
            }
        }
        
        if (count > 0) {
            smooth[i] = sum / count;
        } else {
            smooth[i] = spec.magnitude[i];
        }
    }
    
    smoothed.magnitude = smooth;
    return smoothed;
}

std::vector<float> FFTAnalyzer::getCriticalBands(const Spectrum& spec) {
    // Simplified critical band grouping (Bark scale approximation)
    std::vector<float> bands;
    
    // Critical band centers in Hz for human hearing
    const std::vector<float> criticalBands = {
        50, 150, 250, 350, 450, 570, 700, 840, 1000, 1170, 1370, 1600,
        1850, 2150, 2500, 2900, 3400, 4000, 4800, 5800, 7000, 8500, 10500, 13500
    };
    
    for (float bandFreq : criticalBands) {
        // Find closest frequency in spectrum
        float minDist = 1e9f;
        float bandMagnitude = 0.f;
        
        for (size_t i = 0; i < spec.frequencies.size(); ++i) {
            float dist = std::abs(spec.frequencies[i] - bandFreq);
            if (dist < minDist) {
                minDist = dist;
                bandMagnitude = spec.magnitude[i];
            }
        }
        
        bands.push_back(bandMagnitude);
    }
    
    return bands;
}

void FFTAnalyzer::setFFTSize(uint32_t size) {
    if (size == 0 || (size & (size - 1)) != 0) {
        throw std::runtime_error("FFT size must be a power of 2");
    }
    fftSize_ = size;
    generateHannWindow();
}

void FFTAnalyzer::setSampleRate(uint32_t rate) {
    if (rate == 0) {
        throw std::runtime_error("Sample rate must be positive");
    }
    sampleRate_ = rate;
}

// Cooley-Tukey FFT implementation (fallback)
std::vector<std::complex<float>> FFTAnalyzer::computeFFT(const std::vector<float>& samples) {
    uint32_t n = fftSize_;
    std::vector<std::complex<float>> result(n);
    
    for (uint32_t i = 0; i < n; ++i) {
        float sample = (i < samples.size()) ? samples[i] : 0.f;
        result[i] = std::complex<float>(sample, 0.f);
    }
    
    // Cooley-Tukey FFT algorithm
    for (uint32_t s = 1; s <= static_cast<uint32_t>(std::log2(n)); ++s) {
        uint32_t m = 1 << s;
        uint32_t m2 = m >> 1;
        std::complex<float> w(1.f, 0.f);
        std::complex<float> wm(std::cos(-2.f * M_PI / m), std::sin(-2.f * M_PI / m));
        
        for (uint32_t k = 0; k < m2; ++k) {
            for (uint32_t j = k; j < n; j += m) {
                uint32_t t = j + m2;
                std::complex<float> u = result[j];
                std::complex<float> v = result[t] * w;
                result[j] = u + v;
                result[t] = u - v;
            }
            w *= wm;
        }
    }
    
    // Bit-reversal permutation
    uint32_t j = 0;
    for (uint32_t i = 0; i < n - 1; ++i) {
        if (i < j) {
            std::swap(result[i], result[j]);
        }
        
        uint32_t k = n >> 1;
        while (k <= j) {
            j -= k;
            k >>= 1;
        }
        j += k;
    }
    
    return result;
}

Spectrum FFTAnalyzer::complexToSpectrum(const std::vector<std::complex<float>>& fftResult) {
    Spectrum spec;
    spec.fftSize = fftSize_;
    spec.sampleRate = sampleRate_;
    
    uint32_t numBins = fftSize_ / 2 + 1;
    spec.magnitude.resize(numBins);
    spec.phase.resize(numBins);
    spec.frequencies.resize(numBins);
    
    float freqResolution = static_cast<float>(sampleRate_) / fftSize_;
    
    for (uint32_t i = 0; i < numBins; ++i) {
        spec.frequencies[i] = i * freqResolution;
        
        float mag = std::abs(fftResult[i]);
        
        // Convert to dB (with floor to prevent -inf)
        spec.magnitude[i] = 20.f * std::log10(std::max(mag, 1e-10f));
        spec.phase[i] = std::arg(fftResult[i]);
    }
    
    return spec;
}

} // namespace mastered
