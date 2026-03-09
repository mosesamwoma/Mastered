# Mastered

Professional audio mastering engine — analyzes a reference track and applies spectral EQ matching to your audio.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

**Requirements:** CMake 3.16+, C++17, FFTW3 (optional, auto-fallback to built-in FFT)

## Usage

```bash
./build/mastered_cli reference.wav input.wav [output.json]
```

Creates `mastered_input.wav` with EQ and loudness matched to reference (target: -14 LUFS).

## Library Usage

```cpp
#include "mastering_engine.h"
using namespace mastered;

MasteringEngine engine;
auto result = engine.analyzeTracks("reference.wav", "beat.wav");
AudioBuffer mastered = engine.applyMastering(beatBuffer, result.eqCurve, result.makeupGain);
AudioLoader::saveWAV("mastered_beat.wav", mastered);
```

## How It Works

1. Loads WAV files, computes FFT spectra
2. Calculates frequency-domain difference (A-weighted, smoothed)
3. Fits up to 8 parametric EQ bands via peak/dip detection
4. Applies IIR biquad filters + makeup gain
5. Exports mastered WAV + optional JSON report

## Output

```json
{
  "stats": {
    "correlation": 0.91,
    "spectralDifference": 2.4,
    "confidence": 0.87,
    "estimatedLUFS": -18.2,
    "makeupGain": 4.2
  },
  "eqBands": [{ "frequency": 120, "gain": 2.1, "q": 0.7 }]
}
```