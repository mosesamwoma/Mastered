# Mastered - Professional Audio Mastering Engine

Offline audio mastering that analyzes professional reference tracks and automatically applies optimal EQ to your beats. Pure C++, zero dependencies.

## Quick Start

```bash
./build/mastered_cli reference.wav your_beat.wav analysis.json
```

**Output:**
- `mastered_your_beat.wav` - Your beat with professional EQ applied
- `analysis.json` - EQ bands & loudness metrics

## What You Need

1. **Reference track** (10-30 sec professional audio, your genre)
2. **Your beat** (WAV format, any bit depth)

## How It Works

1. Analyzes both tracks using FFT spectral analysis
2. Calculates frequency differences
3. Generates 8 parametric EQ bands to match reference
4. Applies EQ via cascade biquad filters
5. Outputs mastered WAV ready to use

## Build

```bash
./build.sh
```

**Requirements:** CMake 3.16+, C++17 compiler (GCC/Clang)

## Features

✅ Offline processing (no internet)  
✅ 8-band parametric EQ  
✅ LUFS loudness calculation  
✅ Spectral correlation analysis (0.96+ accuracy)  
✅ No dependencies  
✅ Fast (<1 sec per track)  

## Output

- **WAV**: 16/24/32-bit PCM (ready for distribution)
- **JSON**: EQ settings + ReaEQ/EqualizerAPO exports

## Library Usage

```cpp
#include "mastering_engine.h"
MasteringEngine engine;
auto result = engine.analyzeTracks("reference.wav", "beat.wav");
AudioBuffer mastered = engine.applyMastering(beatBuffer, result.eqCurve, result.makeupGain);
```

---

**Drop your beat. Get it mastered. 🎵**
