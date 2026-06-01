# Mastered

Professional C++ audio mastering engine. Feed it a reference track and a raw beat — it matches the spectrum, hits the loudness target, and outputs a streaming-ready WAV.

---

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

**Requirements:** CMake 3.16+, C++17, FFTW3 (optional — `sudo apt install libfftw3-dev`)

---

## CLI

```bash
./build/mastered_cli <reference.wav> <input.wav> [output.json] [options]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--target N` | `-14` | Target LUFS (Spotify = −14, Apple Music = −16) |
| `--bitdepth N` | source | Output bit depth: `16`, `24`, or `32` |
| `--aggressive N` | `0.85` | EQ strength 0–1 |
| `--headroom N` | `0.99` | Clipping headroom 0–1 |
| `--verbose` | off | Print EQ bands, export ReaEQ + EqualizerAPO presets |
| `--help` | — | Show usage |

**Output:** `mastered_<name>.wav` + `mastering_result.json` (or your specified path).

```bash
# Spotify master
./build/mastered_cli ref.wav beat.wav --target -14 --bitdepth 24

# Apple Music master
./build/mastered_cli ref.wav beat.wav result.json --target -16 --bitdepth 24

# Full export with ReaEQ / EqualizerAPO presets
./build/mastered_cli ref.wav beat.wav result.json --verbose --bitdepth 24
```

---

## Library

```cpp
#include "mastering_engine.h"
using namespace mastered;

// Configure
MasteringConfig config = createDefaultConfig();
config.targetLoudnessLUFS = -14.f;
config.aggressiveness     = 0.85f;

// Analyze
MasteringEngine engine(config);
auto result = engine.analyzeTracks("reference.wav", "beat.wav");

// Apply
AudioBuffer mastered = engine.applyMastering(
    result.inputBuffer,
    result.eqCurve,
    result.makeupGain
);

// Save
AudioLoader::saveWAV("mastered_beat.wav", mastered);

// Export
engine.exportEQasJSON(result);           // full analysis JSON
engine.exportEQasReaEQ(result);          // Reaper / ReaEQ preset
engine.exportEQasEqualizerAPO(result);   // Windows APO preset
```

**Progress callback:**

```cpp
engine.setProgressCallback([](const std::string& msg, float pct) {
    std::cout << "[" << pct << "%] " << msg << "\n";
});
```

---

## How It Works

1. **FFT analysis** — 8192-point Hann-windowed FFT, 50% overlap, averaged across all frames. 1/3-octave log-scale smoothing applied to both spectra.
2. **EQ curve** — A-weighted frequency difference, suppressed to ±12 dB, peak/dip detection with ±3-bin plateau tolerance, up to 8 bands at 1/3-octave minimum spacing.
3. **Filter types** — RBJ shelving filters for `shelf-low` / `shelf-high`, peaking EQ for mid-range. Applied independently per stereo channel.
4. **Loudness** — ITU-R BS.1770-4 K-weighted LUFS with absolute gate (−70 LUFS) + relative gate (−10 LU). Per-channel stereo measurement.
5. **Limiting** — 4× oversampled true peak detection at −1 dBTP ceiling, then hard clip guard.

---

## JSON Output

```json
{
  "version": "1.1",
  "stats": {
    "correlation": 0.99,
    "spectralDifference": 2.17,
    "confidence": 0.88,
    "estimatedLUFS": -13.00,
    "makeupGain": -1.00
  },
  "eqBands": [
    { "id": 0, "frequency": 60.0,   "gain": -0.97, "q": 0.70, "type": "shelf-low",  "bandwidth": 85.71   },
    { "id": 1, "frequency": 2293.3, "gain":  9.37, "q": 1.80, "type": "peak",       "bandwidth": 1272.42 }
  ]
}
```

---

## Formats

| | Support |
|---|---|
| Bit depth | 16 / 24 / 32-bit PCM |
| Channels | Mono, Stereo |
| Sample rates | 8 kHz – 192 kHz |
| Container | WAV / RIFF (PCM only) |
| Max file size | 500 MB |