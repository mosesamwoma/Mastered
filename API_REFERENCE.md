# Mastered Engine - Complete API Reference

## Overview

The Mastered Engine is a professional-grade C++ audio mastering backend that performs automatic spectral analysis and EQ curve generation. All operations are offline and privacy-respecting.

## Core Classes

### AudioLoader

**Purpose**: Load and normalize WAV audio files

**Key Methods**:
```cpp
static AudioBuffer loadWAV(const std::string& filepath);
static bool saveWAV(const std::string& filepath, const AudioBuffer& buffer);
static std::vector<float> stereoToMono(const std::vector<float>& stereoSamples, uint16_t channels);
static void normalize(std::vector<float>& samples);
```

**Supported Formats**:
- 16-bit PCM
- 24-bit PCM
- 32-bit PCM (float)
- Mono and Stereo (auto-converted to mono)
- Sample rates: 44.1 kHz, 48 kHz, 96 kHz, etc.

---

### FFTAnalyzer

**Purpose**: Perform frequency-domain analysis using Fast Fourier Transform

**Key Methods**:
```cpp
Spectrum analyze(const std::vector<float>& samples);
std::vector<Spectrum> analyzeStreaming(const std::vector<float>& samples, float hopRatio = 0.5f);
Spectrum getAveragedSpectrum(const std::vector<float>& samples, uint32_t numFrames = 0);
static Spectrum smoothSpectrum(const Spectrum& spec, uint32_t filterSize = 5);
static std::vector<float> getCriticalBands(const Spectrum& spec);
```

**Configuration**:
```cpp
void setFFTSize(uint32_t size);      // Default: 8192
void setSampleRate(uint32_t rate);   // Default: 44100
```

**Spectrum Structure**:
```cpp
struct Spectrum {
    std::vector<float> magnitude;      // dB scale (0 to ~120 dB)
    std::vector<float> phase;          // Radians (-π to π)
    std::vector<float> frequencies;    // Hz
    uint32_t fftSize;
    uint32_t sampleRate;
};
```

**Technical Details**:
- Algorithm: Cooley-Tukey FFT (O(n log n))
- Window: Hann window for spectral leakage reduction
- Overlap: 50% default (configurable)
- Frequency resolution: ~5.4 Hz @ 44.1 kHz with 8192 FFT

---

### SpectrumMatcher

**Purpose**: Match target audio spectrum to reference spectrum

**Key Methods**:
```cpp
MatchingResult matchSpectra(const Spectrum& referenceSpectrum,
                            const Spectrum& targetSpectrum);
std::vector<float> calculateFrequencyCorrection(const std::vector<float>& referenceDb,
                                                const std::vector<float>& targetDb);
static float calculateSpectralCorrelation(const std::vector<float>& spec1,
                                         const std::vector<float>& spec2);
```

**MatchingResult Structure**:
```cpp
struct MatchingResult {
    float correlation;              // Correlation coefficient (0-1)
    float spectralDifference;       // RMS spectral difference (dB)
    std::vector<float> correction;  // Per-frequency gain adjustment
    float confidenceScore;          // 0-1, higher = more accurate
};
```

**Features**:
- Confidence-weighted corrections
- Frequency-dependent weighting (peak at 1-4 kHz)
- Aggressive correction limiting (±12 dB default)
- Spectral correlation analysis

---

### EQCalculator

**Purpose**: Generate parametric EQ bands from spectral analysis

**Key Methods**:
```cpp
EQCurve calculateEQFromReference(const Spectrum& referenceSpectrum,
                                 const Spectrum& targetSpectrum);
std::vector<EQBand> generateParametricBands(const std::vector<float>& differenceCurve,
                                            const std::vector<float>& frequencies,
                                            uint32_t maxBands = 8);
static std::vector<float> applyAWeighting(const std::vector<float>& spectrum,
                                          const std::vector<float>& frequencies);
static std::vector<float> suppressExtremes(const std::vector<float>& curve, float threshold = 12.f);
```

**EQBand Structure**:
```cpp
struct EQBand {
    float frequency;    // Center frequency in Hz
    float gain;         // Gain in dB (-24 to +24)
    float qFactor;      // Q factor (bandwidth control)
    std::string type;   // "peak", "shelf-low", "shelf-high", "notch"
};
```

**EQCurve Structure**:
```cpp
struct EQCurve {
    std::vector<EQBand> bands;
    std::vector<float> curveResponse;  // Magnitude response per frequency
    std::vector<float> frequencies;    // Frequency points
    float totalGain;                   // Overall makeup gain (dB)
};
```

**Processing Pipeline**:
1. Calculate per-frequency difference (reference - target)
2. Apply A-weighting for perceptual accuracy
3. Suppress extreme peaks (prevent artifacts)
4. Smooth curve for natural results
5. Detect peaks and dips
6. Generate parametric bands
7. Limit gains to ±12 dB

---

### MasteringEngine

**Purpose**: High-level orchestration of mastering analysis

**Key Methods**:
```cpp
MasteringResult analyzeTracks(const std::string& referenceTrackPath,
                              const std::string& unmasteredTrackPath);
MasteringResult analyzeBuffers(const AudioBuffer& reference,
                               const AudioBuffer& unmastered);
std::string exportEQasJSON(const MasteringResult& result) const;
std::string exportEQasReaEQ(const MasteringResult& result) const;
std::string exportEQasEqualizerAPO(const MasteringResult& result) const;
```

**Configuration**:
```cpp
struct MasteringConfig {
    bool autoGain = true;
    bool perceptualWeighting = true;
    uint32_t maxEQBands = 8;
    float aggressiveness = 0.8f;      // 0-1 (0.8 = 80% correction)
    bool smoothing = true;
    float targetLoudnessLUFS = -14.f;
};
```

**MasteringResult Structure**:
```cpp
struct MasteringResult {
    EQCurve eqCurve;
    float estimatedLUFS;
    float makeupGain;
    MatchingResult matchingStats;
    bool success;
    std::string message;
};
```

---

## Usage Example

### Basic Analysis

```cpp
#include "mastering_engine.h"
using namespace mastered;

int main() {
    // Configure
    MasteringConfig config;
    config.aggressiveness = 0.85f;
    config.maxEQBands = 6;
    
    // Create engine
    MasteringEngine engine(config);
    
    // Analyze tracks
    auto result = engine.analyzeTracks("reference.wav", "unmastered.wav");
    
    // Check results
    if (result.success) {
        std::cout << "Confidence: " << result.matchingStats.confidenceScore << "\n";
        std::cout << "Bands: " << result.eqCurve.bands.size() << "\n";
        
        // Print bands
        for (const auto& band : result.eqCurve.bands) {
            std::cout << band.frequency << " Hz: " << band.gain << " dB\n";
        }
        
        // Export JSON
        std::string json = engine.exportEQasJSON(result);
        std::cout << json << "\n";
    } else {
        std::cerr << "Error: " << result.message << "\n";
    }
    
    return 0;
}
```

### Advanced: Custom Analysis

```cpp
// Load audio manually
AudioBuffer ref = AudioLoader::loadWAV("reference.wav");
AudioBuffer target = AudioLoader::loadWAV("unmastered.wav");

// Create analyzer
FFTAnalyzer analyzer(8192, 44100);

// Get averaged spectra
auto refSpec = analyzer.getAveragedSpectrum(ref.samples);
auto targetSpec = analyzer.getAveragedSpectrum(target.samples);

// Match spectra
SpectrumMatcher matcher;
auto matching = matcher.matchSpectra(refSpec, targetSpec);

// Generate EQ
EQCalculator eqCalc;
auto eqCurve = eqCalc.calculateEQFromReference(refSpec, targetSpec);

// Use results
std::cout << "Correction needed at each frequency band\n";
for (size_t i = 0; i < matching.correction.size(); i += 100) {
    std::cout << refSpec.frequencies[i] << " Hz: " 
              << matching.correction[i] << " dB\n";
}
```

---

## Export Formats

### JSON Format

```json
{
  "success": true,
  "message": "Analysis completed successfully",
  "stats": {
    "correlation": 0.96,
    "spectralDifference": 6.81,
    "confidence": 0.63,
    "estimatedLUFS": -10.89,
    "makeupGain": -3.11
  },
  "eqBands": [
    {
      "frequency": 1000.0,
      "gain": 3.2,
      "q": 2.0,
      "type": "peak"
    }
  ],
  "curveResponse": [-0.5, -0.3, 0.1, 0.8, ...]
}
```

### ReaEQ Format

```
<ReaEQ 8 bands>
Band: freq=120.0 gain=-2.5 bw=0.7 type=shelf-low
Band: freq=2500.0 gain=3.2 bw=2.0 type=peak
```

### EqualizerAPO Format

```
Filter: ON PK Fc=1000.0 Gain=3.2 Q=2.0
Filter: ON PK Fc=2500.0 Gain=1.5 Q=1.5
```

---

## Performance Characteristics

| Metric | Value |
|--------|-------|
| Analysis time (5 min @ 44.1 kHz) | ~500 ms |
| Memory usage (full track) | ~50 MB |
| Frequency resolution | ~5.4 Hz |
| Max correction range | ±24 dB |
| Recommended FFT size | 8192 |
| Default max bands | 8 |

---

## Design Rationale

### Why Cooley-Tukey FFT?
- O(n log n) complexity
- No external dependencies
- Portable across platforms
- FFTW available as optional optimization

### Why Hann Window?
- Excellent spectral leakage suppression
- Suitable for overlapping frame analysis
- Standard in audio DSP

### Why A-Weighting?
- Matches human hearing sensitivity
- Prevents over-correction in inaudible bands
- Industry standard in mastering

### Why Parametric EQ?
- More intuitive than graphic EQ
- Fewer bands needed for accurate correction
- Compatible with professional DAWs
- Export to standard formats (ReaEQ, EqualizerAPO)

---

## Integration Checklist

- [ ] Include header files in your project
- [ ] Link against `libmastered_engine.a`
- [ ] Ensure WAV files are properly normalized
- [ ] Configure `MasteringConfig` for your needs
- [ ] Handle `MasteringResult.success` flag
- [ ] Export results in appropriate format
- [ ] Integrate feedback from confidence scores

---

## License

Proprietary. All rights reserved.
