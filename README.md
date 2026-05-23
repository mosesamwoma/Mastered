# Mastered

A C++ audio mastering engine that analyzes a professional reference track and applies spectral EQ matching to an unmastered WAV file.

## Requirements

- CMake 3.16+
- C++17 compiler (GCC or Clang)
- FFTW3 (optional — falls back to built-in FFT if not found)

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

## Usage

```bash
./build/mastered_cli reference.wav your_beat.wav [output.json]
```

| Argument | Description |
|---|---|
| `reference.wav` | Professional track in your target genre (10–30 sec recommended) |
| `your_beat.wav` | Unmastered input |
| `output.json` | Optional — EQ bands and loudness metrics (default: `mastering_result.json`) |

Output: `mastered_<input>.wav` in the current directory.

Alternatively, use the shell script:

```bash
./master.sh your_beat.wav reference.wav
```

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

1. Loads both WAV files and computes averaged FFT spectra
2. Calculates the frequency-domain difference between reference and input
3. Applies A-weighting and smoothing to the difference curve
4. Fits up to 8 parametric EQ bands (peaks and shelves) to the curve
5. Applies each band via a second-order IIR biquad filter
6. Calculates makeup gain to hit a target loudness of -14 LUFS
7. Exports the mastered WAV and an optional JSON report

## Output JSON

```json
{
  "stats": {
    "correlation": 0.91,
    "spectralDifference": 2.4,
    "confidence": 0.87,
    "estimatedLUFS": -18.2,
    "makeupGain": 4.2
  },
  "eqBands": [
    { "frequency": 120, "gain": 2.1, "q": 0.7, "type": "shelf-low" }
  ]
}
```
---