# Mathematics Behind Mastered Engine

Core mathematical formulas and algorithms.

## 1. Hann Window
$$w[n] = 0.5 \left(1 - \cos\left(\frac{2\pi n}{N-1}\right)\right)$$
Reduces spectral leakage in FFT analysis.

## 2. Cooley-Tukey FFT
$O(N \log N)$ fast Fourier transform with bit-reversal permutation applied FIRST.

## 3. Magnitude to dB
$$M_{\text{dB}} = 20 \log_{10}(\max(|X|, 10^{-10}))$$

## 4. Frequency Correction
$$\text{Correction}[i] = \text{Ref}_{\text{dB}}[i] - \text{Target}_{\text{dB}}[i]$$

## 5. A-Weighting (Perceptual)
$$A(f) = 20 \log_{10}\left(\frac{12200^2 f^4}{(f^2 + 20.6^2)(f^2 + 107.7^2)(f^2 + 737.9^2)(f^2 + 12200^2)}\right) + 2$$
Weights corrections by human hearing sensitivity (peak at 1-4 kHz).

## 6. RMS Spectral Difference
$$\Delta_{\text{RMS}} = \sqrt{\frac{1}{N}\sum_{i=0}^{N-1} \text{Correction}[i]^2}$$

## 7. Spectral Correlation
$$r = \frac{\sum (R_i - \bar{R})(T_i - \bar{T})}{\sqrt{\sum (R_i - \bar{R})^2 \sum (T_i - \bar{T})^2}}$$
Measures statistical similarity: 1.0 = identical, 0 = uncorrelated.

## 8. Second-Order Peaking EQ (Bristow-Johnson)
$$A = 10^{g/40}, \quad \omega_0 = \frac{2\pi f}{f_s}, \quad \alpha = \frac{\sin(\omega_0)}{2Q}$$

**Numerator:** $b_0 = 1 + \alpha A$, $b_1 = -2\cos(\omega_0)$, $b_2 = 1 - \alpha A$

**Denominator:** $a_0 = 1 + \alpha/A$, $a_1 = -2\cos(\omega_0)$, $a_2 = 1 - \alpha/A$

**Direct Form II:**
$$w_n = x_n - a_1' w_{n-1} - a_2' w_{n-2}$$
$$y_n = b_0' w_n + b_1' w_{n-1} + b_2' w_{n-2}$$

## 9. Magnitude Response
$$|H(\omega)| = \frac{\sqrt{(b_0 + b_1\cos\omega + b_2\cos 2\omega)^2 + (b_1\sin\omega + b_2\sin 2\omega)^2}}{\sqrt{(1 + a_1\cos\omega + a_2\cos 2\omega)^2 + (a_1\sin\omega + a_2\sin 2\omega)^2}}$$

## 10. Confidence Score
$$\text{Confidence} = \text{clamp}\left(r \cdot \left(1 - \frac{\Delta_{\text{RMS}}}{20}\right), 0, 1\right)$$
Combines correlation and difference for reliability rating.

## 11. LUFS Loudness (ITU-R BS.1770-4)
$$\text{LUFS} = -0.691 + 10 \log_{10}\left(\frac{1}{N} \sum_{i=1}^{N} P_i^2\right)$$
Perceptual loudness measurement. Target: -14 LUFS (streaming), -23 LUFS (broadcast).

## 12. Makeup Gain
$$\text{MakeupGain} = \text{TargetLUFS} - \text{EstimatedLUFS}$$
$$\text{GainLinear} = 10^{\text{MakeupGain}/20}$$

## 13. Stereo to Mono
$$\text{Mono}[n] = \frac{1}{C} \sum_{c=0}^{C-1} \text{Stereo}[n, c]$$

## 14. Peak Normalization
$$\text{if } \max(|x|) > 1.0 \text{ then } \text{scale} = 0.9 / \max(|x|)$$
Soft-limit to prevent clipping while preserving loudness.

## 15. Frequency Resolution
$$f_{\text{resolution}} = \frac{f_s}{N}$$
Example: 44.1 kHz / 8192 = **5.38 Hz** per bin

## 16. Linear to dB
$$\text{dB}_{\text{amplitude}} = 20 \log_{10}(\text{ratio})$$
$$\text{dB}_{\text{power}} = 10 \log_{10}(\text{ratio})$$

| dB | Linear | Effect |
|----|--------|--------|
| +6 | 2.0× | Doubling |
| +3 | 1.41× | ~10% louder |
| 0 | 1.0× | Reference |
| -3 | 0.71× | ~10% quieter |
| -6 | 0.5× | Halving |

## Configuration Constants
```cpp
namespace constants {
    constexpr float DEFAULT_Q_FACTOR = 2.0f;
    constexpr float PEAK_DETECTION_THRESHOLD = 0.5f;
    constexpr float MIN_GAIN_THRESHOLD = 0.3f;
    constexpr float CLIPPING_HEADROOM = 0.99f;
    constexpr uint32_t DEFAULT_FFT_SIZE = 8192;
    constexpr uint32_t DEFAULT_SAMPLE_RATE = 44100;
}
```

## Pipeline Overview
```
Input → Load & Validate → FFT Analysis → Magnitude to dB
→ Averaging → Spectral Comparison → EQ Generation
→ Apply Bands → LUFS Calc → Makeup Gain → Clipping Prevention
→ Save WAV → Export JSON
```

## Key Fixes Applied
- ✅ Bit-reversal: MOVED TO BEFORE FFT (was after, causing wrong frequencies)
- ✅ Variance: Fixed var2 division (was dividing by spec1.size, should be spec2.size())
- ✅ Normalization: Changed to soft-limit (was aggressively scaling all audio)
- ✅ Last Frame: Now zero-padded and included (was skipped)
- ✅ Bit-Depth: Preserved in save/load (was always 16-bit)

## References
- **Cooley-Tukey FFT:** Cooley & Tukey (1965)
- **Bristow-Johnson Filters:** Robert Bristow-Johnson EQ Cookbook
- **A-Weighting:** IEC 61672-1:2013
- **LUFS:** ITU-R BS.1770-4:2015
- **Hann Window:** Harris (1978)

*Updated: May 25, 2026 | Engine: v1.5 (Critical bugs fixed)*
