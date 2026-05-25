# Mathematics Behind Mastered Engine

> **Core formulas and algorithms** for audio spectral analysis and mastering

---

## SECTION 1: SPECTRAL ANALYSIS

### Hann Window
**Purpose:** Reduce spectral leakage in FFT analysis

<div style="text-align: center; font-size: 1.3em; margin: 20px 0;">

$$w[n] = 0.5 \left(1 - \cos\left(\frac{2\pi n}{N-1}\right)\right)$$

</div>

Smoothly tapers frame edges to eliminate discontinuities.

---

### Cooley-Tukey FFT
**Purpose:** Fast frequency analysis

<div style="text-align: center; font-size: 1.3em; margin: 20px 0;">

$$O(N \log N) \text{ time complexity}$$

</div>

Bit-reversal permutation **FIRST** (critical!)  
Butterfly operations follow  
Twiddle factor multiplication

---

### Magnitude to dB
**Purpose:** Convert linear to logarithmic scale (human perception)

<div style="text-align: center; font-size: 1.3em; margin: 20px 0;">

$$M_{\text{dB}} = 20 \log_{10}(\max(|X|, 10^{-10}))$$

</div>

**Range:** 0 dB (full scale) to −∞ (silence)

---

## SECTION 2: SPECTRAL COMPARISON

### Frequency Correction
**Purpose:** Calculate per-frequency EQ adjustment needed

<div style="text-align: center; font-size: 1.3em; margin: 20px 0;">

$$\text{Correction}[i] = \text{Ref}_{\text{dB}}[i] - \text{Target}_{\text{dB}}[i]$$

</div>

- **Positive** = boost at this frequency  
- **Negative** = cut at this frequency

---

### A-Weighting (Perceptual)
**Purpose:** Weight corrections by human hearing sensitivity

<div style="text-align: center; font-size: 1.3em; margin: 20px 0;">

$$A(f) = 20 \log_{10}\left(\frac{12200^2 f^4}{(f^2 + 20.6^2)(f^2 + 107.7^2)(f^2 + 737.9^2)(f^2 + 12200^2)}\right) + 2 \text{ dB}$$

</div>

**Peak sensitivity:** 1–4 kHz (speech range)  
**Low sensitivity:** < 100 Hz & > 10 kHz (inaudible)

---

### RMS Spectral Difference
**Purpose:** Measure overall spectral mismatch

<div style="text-align: center; font-size: 1.3em; margin: 20px 0;">

$$\Delta_{\text{RMS}} = \sqrt{\frac{1}{N}\sum_{i=0}^{N-1} \text{Correction}[i]^2}$$

</div>

**Interpretation:**
- 0 dB = perfect match
- 3–6 dB = noticeable difference
- > 6 dB = significant mismatch

---

### Spectral Correlation
**Purpose:** Statistical similarity between spectra

<div style="text-align: center; font-size: 1.3em; margin: 20px 0;">

$$r = \frac{\sum (R_i - \bar{R})(T_i - \bar{T})}{\sqrt{\sum (R_i - \bar{R})^2 \sum (T_i - \bar{T})^2}}$$

</div>

**Scale:** $r \in [-1, 1]$
- **r = 1.0** → identical
- **r = 0.0** → random
- **r = -1.0** → inverted

---

## SECTION 3: EQUALIZATION & FILTERING

### Second-Order Peaking EQ (Bristow-Johnson)
**Purpose:** Parametric filter at specific frequency & bandwidth

<div style="text-align: center; font-size: 1.3em; margin: 20px 0;">

$$A = 10^{g/40}, \quad \omega_0 = \frac{2\pi f}{f_s}, \quad \alpha = \frac{\sin(\omega_0)}{2Q}$$

</div>

**Filter Coefficients:**

| Type | Value |
|------|-------|
| $b_0$ | $1 + \alpha A$ |
| $b_1$ | $-2\cos(\omega_0)$ |
| $b_2$ | $1 - \alpha A$ |
| $a_0$ | $1 + \alpha/A$ |
| $a_1$ | $-2\cos(\omega_0)$ |
| $a_2$ | $1 - \alpha/A$ |

**Direct Form II (Implementation):**

<div style="text-align: center; font-size: 1.2em; margin: 15px 0;">

$$w_n = x_n - a_1' w_{n-1} - a_2' w_{n-2}$$

$$y_n = b_0' w_n + b_1' w_{n-1} + b_2' w_{n-2}$$

</div>

---

### Magnitude Response
**Purpose:** Calculate filter response across frequencies

<div style="text-align: center; font-size: 1.2em; margin: 20px 0;">

$$|H(\omega)| = \frac{\sqrt{(\text{num}_{\text{real}})^2 + (\text{num}_{\text{imag}})^2}}{\sqrt{(\text{denom}_{\text{real}})^2 + (\text{denom}_{\text{imag}})^2}}$$

$$|H(\omega)|_{\text{dB}} = 20 \log_{10}(|H(\omega)|)$$

</div>

---

## SECTION 4: QUALITY & LOUDNESS

### Confidence Score
**Purpose:** Rate match reliability (0–1)

<div style="text-align: center; font-size: 1.3em; margin: 20px 0;">

$$\text{Confidence} = \text{clamp}\left(r \cdot \left(1 - \frac{\Delta_{\text{RMS}}}{20}\right), 0, 1\right)$$

</div>

| Score | Rating | Action |
|-------|--------|--------|
| 0.9–1.0 | Very Confident | Apply |
| 0.7–0.9 | Confident | Apply |
| 0.5–0.7 | Moderate | Review |
| < 0.5 | Low | Manual check |

---

### LUFS Loudness (ITU-R BS.1770-4)
**Purpose:** Perceptual loudness measurement

<div style="text-align: center; font-size: 1.3em; margin: 20px 0;">

$$\text{LUFS} = -0.691 + 10 \log_{10}\left(\frac{1}{N} \sum_{i=1}^{N} P_i^2\right)$$

</div>

**Standard Targets:**
- Spotify/YouTube: **−14 LUFS**
- Broadcasting: **−23 LUFS**
- Mastered music: **−8 to −12 LUFS**

---

### Makeup Gain
**Purpose:** Adjust amplitude to reach target loudness

<div style="text-align: center; font-size: 1.2em; margin: 20px 0;">

$$\text{MakeupGain} = \text{TargetLUFS} - \text{EstimatedLUFS}$$

$$\text{GainLinear} = 10^{\text{MakeupGain}/20}$$

</div>

- Positive gain = amplification
- Negative gain = attenuation

---

## SECTION 5: AUDIO CONVERSION & NORMALIZATION

### Stereo to Mono
**Purpose:** Convert multi-channel to mono

<div style="text-align: center; font-size: 1.3em; margin: 20px 0;">

$$\text{Mono}[n] = \frac{1}{C} \sum_{c=0}^{C-1} \text{Stereo}[n, c]$$

</div>

Simple average of all channels.

---

### Peak Normalization
**Purpose:** Prevent clipping while preserving loudness

<div style="text-align: center; font-size: 1.3em; margin: 20px 0;">

$$\text{if } \max(|x|) > 1.0 \text{ then } \text{scale} = 0.9 / \max(|x|)$$

</div>

**Soft-limit approach** (not aggressive scaling)  
**Quiet audio preserved**  
**Only scales when needed**

---

### Frequency Resolution
**Purpose:** Spectral precision of FFT analysis

<div style="text-align: center; font-size: 1.3em; margin: 20px 0;">

$$f_{\text{resolution}} = \frac{f_s}{N}$$

</div>

**Example:** 44.1 kHz ÷ 8192 = **5.38 Hz per bin**

Higher resolution = more computation/latency

---

### Linear to dB Conversion
**Purpose:** Amplitude and power ratio scaling

<div style="text-align: center; font-size: 1.2em; margin: 15px 0;">

$$\text{dB}_{\text{amp}} = 20 \log_{10}(\text{ratio})$$

$$\text{dB}_{\text{power}} = 10 \log_{10}(\text{ratio})$$

</div>

**Quick Reference:**

| dB | Linear | Perception |
|:---:|:---:|:---:|
| +6 dB | 2.0× | Doubling |
| +3 dB | 1.41× | ~10% louder |
| 0 dB | 1.0× | Reference |
| −3 dB | 0.71× | ~10% quieter |
| −6 dB | 0.5× | Halving |

---

## SECTION 6: CONFIGURATION

### Configuration Constants
**Default engine parameters** (namespace constants)

```cpp
constexpr float DEFAULT_Q_FACTOR = 2.0f;
constexpr float PEAK_DETECTION_THRESHOLD = 0.5f;
constexpr float MIN_GAIN_THRESHOLD = 0.3f;
constexpr float CLIPPING_HEADROOM = 0.99f;
constexpr uint32_t DEFAULT_FFT_SIZE = 8192;
constexpr uint32_t DEFAULT_SAMPLE_RATE = 44100;
```

---

## References

- **Cooley-Tukey FFT:** Cooley & Tukey (1965)
- **Bristow-Johnson Filters:** Robert Bristow-Johnson EQ Cookbook
- **A-Weighting:** IEC 61672-1:2013
- **LUFS Standard:** ITU-R BS.1770-4:2015
- **Hann Window:** Harris (1978)

---
