# Mastered Engine - Quick Start Guide

## 5-Minute Setup

### 1. Build

```bash
cd /home/moses-amwoma/projects/mastered
./build.sh
```

Or manually:
```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

### 2. Generate Test Files

```bash
python3 generate_test_audio.py
```

### 3. Run Analysis

```bash
./build/mastered_cli test_audio/reference.wav test_audio/unmastered.wav result.json
```

### 4. View Results

```bash
cat result.json
```

---

## How It Works

1. **Load Audio**: Read WAV files (mono or stereo)
2. **Analyze Frequency Content**: Perform FFT on both tracks
3. **Compare Spectra**: Calculate difference between reference and unmastered
4. **Generate EQ**: Extract parametric EQ bands from difference
5. **Export**: Save results in JSON/DAW formats

---

## Your Tracks

### Reference Track (Well-Mastered)
- Should represent your target sound
- Genre-matching is important
- Professional mastered files work best

### Unmastered Track
- Your raw production
- Can have any frequency imbalance
- Engine will auto-detect issues

---

## Understanding Results

### Key Metrics

**Correlation** (0-1)
- How similar the reference and unmastered tracks are
- 0.8+ = good similarity
- Lower = very different tonality

**Spectral Difference** (dB)
- RMS difference between spectra
- 0-3 dB = very similar
- 5+ dB = significant differences

**Confidence** (0-1)
- How reliable the analysis is
- 0.7+ = high confidence
- Used to weight corrections

**LUFS** (Loudness Units)
- Perceived loudness of track
- Industry standard: -14 to -16 LUFS
- Critical for streaming platforms

### Generated EQ Bands

Each band has:
- **Frequency**: Center frequency in Hz
- **Gain**: Boost (+) or cut (-) in dB
- **Q**: Bandwidth (higher Q = narrower)
- **Type**: peak (narrow), shelf (broad), notch (narrow cut)

---

## Configuration Options

```cpp
MasteringConfig config;

// How aggressively to correct (0.0-1.0)
config.aggressiveness = 0.85f;

// Maximum number of EQ bands to create
config.maxEQBands = 8;

// Calculate makeup gain automatically
config.autoGain = true;

// Target loudness for makeup gain
config.targetLoudnessLUFS = -14.f;

// Use A-weighting (human hearing curve)
config.perceptualWeighting = true;

// Smooth EQ curve for natural sound
config.smoothing = true;
```

---

## Command Line Usage

```bash
./build/mastered_cli <reference.wav> <unmastered.wav> [output.json]
```

### Examples

```bash
# Basic analysis
./build/mastered_cli reference.wav unmastered.wav

# With specific output file
./build/mastered_cli reference.wav unmastered.wav my_mastering.json

# Analyze different genres
./build/mastered_cli hip_hop_ref.wav my_beat.wav hip_hop_analysis.json
./build/mastered_cli electronic_ref.wav my_track.wav electronic_analysis.json
```

---

## Integration with DAWs

### ReaEQ (REAPER)

```bash
./build/mastered_cli reference.wav unmastered.wav result.json
# Then load result in REAPER's ReaEQ as shown in output
```

### EqualizerAPO (Windows/Linux)

```bash
./build/mastered_cli reference.wav unmastered.wav result.json
# Copy EqualizerAPO format to config file
```

### Any DAW

Use the JSON output to manually create equivalent parametric EQ:
1. For each band in `eqBands` array
2. Create a new EQ band in your DAW
3. Set frequency, gain, and Q to match
4. Apply the EQ and A/B test

---

## Troubleshooting

### "File not found"
- Ensure audio files exist and paths are correct
- Use absolute paths if relative paths fail

### "Not a valid WAV file"
- Only standard PCM WAV files supported
- Convert MP3/FLAC to WAV first
- Try: `ffmpeg -i input.mp3 output.wav`

### "Only PCM audio format is supported"
- Reencoded WAV files may be in wrong format
- Convert to 16-bit PCM: `ffmpeg -i input.wav -acodec pcm_s16le output.wav`

### Low confidence score
- Tracks are very different tonality
- Use genre-matching references
- Ensure both tracks are normalized

### Too many/few EQ bands
- Adjust `maxEQBands` in config
- More bands = more precise but might over-fit
- 6-8 bands usually optimal

---

## Best Practices

1. **Match Genres**: Reference should be same genre as target
2. **Match Duration**: Longer tracks (2+ min) give better analysis
3. **Monitor Levels**: Ensure both tracks are normalized
4. **Review Results**: Always A/B test before mastering
5. **Start Conservative**: Use 0.6-0.7 aggressiveness first
6. **Use Multiple References**: Analyze against 2-3 references
7. **Trust Confidence Scores**: Low confidence = use caution

---

## Advanced Usage

### Analyze Stereo Files
```cpp
// Automatically converted to mono internally
AudioBuffer stereo = AudioLoader::loadWAV("stereo_track.wav");
// Processes mono version internally
```

### Batch Analysis
```bash
#!/bin/bash
for track in beats/*.wav; do
    ref="reference.wav"
    out="${track%.wav}_mastering.json"
    ./build/mastered_cli "$ref" "$track" "$out"
done
```

### Custom Aggressiveness Levels
```cpp
// Conservative (0.5 = 50% of calculated correction)
config.aggressiveness = 0.5f;

// Balanced (0.8 = 80% of calculated correction)
config.aggressiveness = 0.8f;

// Aggressive (1.0 = 100% of calculated correction)
config.aggressiveness = 1.0f;
```

---

## Performance Tips

- Analyze mono tracks (faster)
- Use 4096 FFT for speed, 8192 for accuracy
- Longer audio = better averaging (smoother results)
- Average results from multiple references

---

## File Locations

```
.
├── build/
│   ├── libmastered_engine.a      (link this)
│   └── mastered_cli              (run this)
├── include/                       (include these headers)
├── test_audio/                    (sample WAV files)
├── result.json                    (example output)
└── CMakeLists.txt                 (build config)
```

---

## Support Files

- `README.md` - Full documentation
- `API_REFERENCE.md` - Complete API docs
- `generate_test_audio.py` - Create test files
- `build.sh` - Build helper script

---

## Next Steps

1. Build the project: `./build.sh`
2. Generate test audio: `python3 generate_test_audio.py`
3. Run analysis: `./build/mastered_cli test_audio/reference.wav test_audio/unmastered.wav`
4. View results: `cat result.json`
5. Integrate into your workflow
6. Build your frontend using the JSON output

---

**Ready to master!** 🎵
