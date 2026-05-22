# Mastered - Professional Audio Mastering Engine

Offline audio mastering that analyzes professional reference tracks and automatically applies optimal EQ to your beats. Pure C++, zero dependencies.

## Clone & Setup

```bash
git clone https://github.com/mosesamwoma/Mastered.git
cd Mastered
```

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Or use the build script:
```bash
./build.sh
```

## Running the Project

### Option 1: Using Script (Easiest) 🚀

```bash
./master.sh your_beat.wav reference.wav
```

Outputs to `mastered_output/`

### Option 2: Direct CLI (No Script)

```bash
./build/mastered_cli reference.wav your_beat.wav analysis.json
```

**Inputs:**
- `reference.wav` - Professional track in your genre (10-30 sec)
- `your_beat.wav` - Your unmastered beat

**Outputs:**
- `mastered_your_beat.wav` - Your mastered beat 🎵
- `analysis.json` - EQ bands & loudness metrics

## How It Works

1. Analyzes both tracks using FFT spectral analysis
2. Calculates frequency differences
3. Generates 8 parametric EQ bands to match reference
4. Applies EQ via cascade biquad filters
5. Outputs mastered WAV ready to use

## Requirements

CMake 3.16+, C++17 compiler (GCC/Clang)

## Library Usage

```cpp
#include "mastering_engine.h"
MasteringEngine engine;
auto result = engine.analyzeTracks("reference.wav", "beat.wav");
AudioBuffer mastered = engine.applyMastering(beatBuffer, result.eqCurve, result.makeupGain);
```

---

**Drop your beat. Get it mastered. 🎵**
