# Mastered Engine - Professional C++ Audio Mastering Backend

A pure-math, offline audio mastering engine that analyzes reference tracks and automatically generates precise EQ adjustments for unmastered beats. Built entirely in C++ with no dependencies on external AI services.

## Features

- **Spectral Analysis**: Fast Fourier Transform (FFT) with Hann windowing for accurate frequency analysis
- **Reference Matching**: Intelligent spectrum matching algorithm with perceptual weighting
- **Parametric EQ Generation**: Automatic calculation of up to 8 parametric EQ bands
- **Loudness Calculation**: LUFS-based loudness metering
- **Multi-format Export**: JSON, ReaEQ, EqualizerAPO formats
- **Perceptual Weighting**: A-weighting for human hearing sensitivity
- **Confidence Scoring**: Reports analysis quality and reliability
- **Completely Offline**: Zero cloud dependencies, full privacy

## Architecture

### Core Components

1. **AudioLoader** (`audio_loader.*`)
   - WAV file I/O (16/24/32-bit PCM)
   - Stereo to mono conversion
   - Audio normalization

2. **FFTAnalyzer** (`fft_analyzer.*`)
   - Cooley-Tukey FFT implementation (Cooley-Tukey algorithm)
   - Hann window application
   - Streaming frame-based analysis
   - Spectrum smoothing and critical band grouping

3. **SpectrumMatcher** (`spectrum_matcher.*`)
   - Per-frequency correction calculation
   - Confidence weighting by frequency
   - Spectral correlation analysis
   - Aggressive correction limiting

4. **EQCalculator** (`eq_calculator.*`)
   - Difference curve generation
   - Parametric band detection
   - Second-order filter response calculation
   - A-weighting application
   - Curve smoothing and extremes suppression

5. **MasteringEngine** (`mastering_engine.*`)
   - High-level orchestration
   - Configuration management
   - Export functionality
   - LUFS calculation and makeup gain

## Building

### Requirements
- C++17 compiler (GCC 7+, Clang 5+, MSVC 2017+)
- CMake 3.16+
- Optional: FFTW3 library (falls back to built-in FFT if not available)

### Build Steps

```bash
cd /home/moses-amwoma/projects/mastered
mkdir -p build
cd build
cmake ..
make -j$(nproc)
```

### Installation (Optional)

```bash
sudo make install
```

## Usage

### Command Line

```bash
./mastered_cli reference_track.wav unmastered_track.wav [output.json]
```

**Example:**
```bash
./mastered_cli reference.wav beat.wav mastering_result.json
```

### Library Integration

```cpp
#include "mastering_engine.h"
using namespace mastered;

// Configure
MasteringConfig config;
config.aggressiveness = 0.85f;
config.targetLoudnessLUFS = -14.f;

// Create engine and analyze
MasteringEngine engine(config);
auto result = engine.analyzeTracks("reference.wav", "unmastered.wav");

// Access results
std::cout << "EQ Bands: " << result.eqCurve.bands.size() << "\n";
std::cout << "Confidence: " << result.matchingStats.confidenceScore << "\n";
```

## Output Format

### JSON Output Example

```json
{
  "success": true,
  "message": "Analysis completed successfully",
  "stats": {
    "correlation": 0.89,
    "spectralDifference": 3.45,
    "confidence": 0.84,
    "estimatedLUFS": -13.2,
    "makeupGain": 1.2
  },
  "eqBands": [
    {
      "frequency": 120.0,
      "gain": -2.5,
      "q": 0.7,
      "type": "shelf-low"
    },
    {
      "frequency": 2500.0,
      "gain": 3.2,
      "q": 2.0,
      "type": "peak"
    }
  ],
  "curveResponse": [-0.5, -0.3, 0.1, 0.8, ...]
}
```

## Algorithm Details

### Spectral Matching Process

1. **Load & Normalize**: Read WAV files, convert to mono, normalize amplitude
2. **FFT Analysis**: Compute FFT with 8192-point windows (overlapping frames)
3. **Spectrum Averaging**: Average multiple frames for noise reduction
4. **Smoothing**: Apply median filter to reduce artifacts
5. **Difference Curve**: Calculate reference minus target in dB
6. **Perceptual Weighting**: Apply A-weighting to match human hearing
7. **Extremes Suppression**: Cap aggressive corrections (±12 dB default)
8. **Peak Detection**: Find local maxima and minima in difference curve
9. **Band Generation**: Create parametric EQ bands from detected peaks
10. **Confidence Scoring**: Calculate correlation and report analysis quality

### Mathematical Foundations

- **FFT**: Cooley-Tukey algorithm (O(n log n) complexity)
- **Windowing**: Hann window reduces spectral leakage
- **Perceptual Weighting**: A-weighting curve (ISO 226)
- **EQ Filters**: Second-order Butterworth/peak filters
- **LUFS**: Loudness calculation using ITU-R BS.1770

## Performance

- **Analysis Time**: ~500ms for 5-minute tracks (44.1 kHz)
- **Memory**: ~50MB typical for full-length mastering
- **Frequency Resolution**: ~5.4 Hz @ 44.1 kHz with 8192 FFT

## Limitations

- WAV files only (extend `AudioLoader` for MP3/FLAC)
- Mono analysis (stereo tracks converted to mono)
- Maximum ±24 dB EQ range per band
- Designed for mastering (not mixing)

## Future Enhancements

- [ ] Multi-format audio I/O (MP3, FLAC, OGG)
- [ ] Real-time streaming analysis
- [ ] Phase preservation techniques
- [ ] Multiband loudness compression
- [ ] Dynamic range analysis
- [ ] Metering plugins (VST/AU)
- [ ] GPU acceleration via CUDA

## License

Proprietary. All rights reserved.

## Technical Stack

- **Language**: C++17
- **Build System**: CMake
- **DSP**: Pure-math implementations (no external DSP libraries)
- **Dependencies**: Optional FFTW3 for performance

## Contact

For integration inquiries or technical support, refer to the main project documentation.
