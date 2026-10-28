# Mathematics

## Loudness (ITU-R BS.1770-4)

Each channel passes through two cascaded biquads.

Shelf stage with `K = tan(pi f0 / fs)`, `f0 = 1681.974450955533`, `Q = 0.7071752369554196`, `Vh = 10^(3.999843853973347 / 20)`, `Vb = Vh^0.4996667741545416`, `a0 = 1 + K/Q + K^2`:

```
b0 = (Vh + Vb K/Q + K^2) / a0
b1 = 2 (K^2 - Vh) / a0
b2 = (Vh - Vb K/Q + K^2) / a0
a1 = 2 (K^2 - 1) / a0
a2 = (1 - K/Q + K^2) / a0
```

High-pass stage with `f0 = 38.13547087602444`, `Q = 0.5003270373238773`: numerator `1, -2, 1`, denominator `a1 = 2 (K^2 - 1) / a0`, `a2 = (1 - K/Q + K^2) / a0`.

Mean square per 100 ms hop is summed over channels with weights `G` (1.0 for front channels, 1.41254 for surround, 0 for LFE). A 400 ms block is the mean of four hops, giving a 75 % overlap. Block loudness:

```
l = -0.691 + 10 log10(z)
```

Gating: blocks below -70 LUFS are discarded, then blocks below the mean of the remainder minus 10 LU. Integrated loudness is computed from the surviving blocks in the power domain.

Loudness range uses 3 s windows on the same hop. After the -70 LUFS gate and a relative gate 20 LU below the mean, LRA is the 95th minus the 10th percentile of the window loudness values.

## True peak

A 96-tap Kaiser-windowed sinc (beta 6) is split into four phases of 24 taps. Phase 0 is an exact delay of the input, phases 1 to 3 are normalised to unit DC gain. The peak is the maximum absolute output over all phases and channels, evaluated for every input sample.

## Spectrum

Mid `M = (L + R) / 2` and side `S = (L - R) / 2` are packed into one complex FFT `z = M + iS` with a periodic Hann window and 50 % overlap:

```
X_M[k] = (Z[k] + conj(Z[N-k])) / 2
X_S[k] = (Z[k] - conj(Z[N-k])) / (2i)
```

Power `|X|^2` is averaged over frames in the power domain and normalised by the window energy. Frames whose mean square is below `1e-7` and incomplete trailing frames are skipped.

Band level at centre `fc = 30 * 2^(i/6)` is `10 log10` of the mean power of the bins inside `fc * 2^(+/- w/2)` with `w = 1/3` octave (`1/6` without smoothing). Bands narrower than one bin use linear interpolation between neighbouring bins.

## Target curve

For input band levels `x` and reference band levels `r` (each reference first reduced by its own mean over 63 Hz to 12.5 kHz and then averaged):

```
d_i = (r_i - mean(r)) - (x_i - mean(x))
t_i = clamp(a (d_i - median(d)), -12, 12)
```

with `a` the aggressiveness. The median over the level range removes the overall level difference without being pulled by a local hole or peak.

## EQ fit

Biquads use the RBJ cookbook forms. With `A = 10^(g/40)`, `w0 = 2 pi f / fs`, `alpha = sin(w0) / (2Q)`:

```
peak:       b = [1 + alpha A, -2 cos w0, 1 - alpha A]
            a = [1 + alpha / A, -2 cos w0, 1 - alpha / A]
low shelf:  b0 = A ((A+1) - (A-1) cos w0 + 2 sqrt(A) alpha)
            b1 = 2A ((A-1) - (A+1) cos w0)
            b2 = A ((A+1) - (A-1) cos w0 - 2 sqrt(A) alpha)
            a0 = (A+1) + (A-1) cos w0 + 2 sqrt(A) alpha
            a1 = -2 ((A-1) + (A+1) cos w0)
            a2 = (A+1) + (A-1) cos w0 - 2 sqrt(A) alpha
```

The high shelf is the mirror image. The fit grid is 1/12 octave from 30 Hz to the lower of 18 kHz and 0.45 fs, with the target interpolated linearly in log frequency. The cost is

```
J = sum(w_i (t_i - R_i)^2) / sum(w_i) + sum_b (0.004 g_b^2 + 0.03 ln(Q_b)^2)
```

where `R` is the summed dB response of all bands and `w` is a perceptual weight `clamp(A(f) / A(1 kHz), 0.35, 1)` built from the A-weighting curve (or 1 when disabled). Each greedy step picks the frequency with the largest weighted smoothed residual, tries all three band types, refines the new band by pattern search over frequency, gain and Q, keeps the best, then refines all bands jointly. The search stops when no band lowers the cost by 0.001, the residual falls below 0.4 dB, or the band limit is reached. Bands under 0.3 dB are removed.

Limits: gain +/-12 dB, peak Q 0.4 to 6, shelf Q 0.4 to 1, low shelf corner up to 500 Hz, high shelf corner from 1500 Hz.

## Stereo width

```
g_side_dB = clamp(a (sideToMid_ref - sideToMid_in), -3, +3)
```

where `sideToMid = 10 log10(sum S^2 / sum M^2)`. The gain multiplies the side signal after the side EQ.

## Dynamics

Crossovers are Linkwitz-Riley (two cascaded 2nd-order Butterworth) at 150 Hz and 3 kHz. The lowest band is passed through the 3 kHz low-pass plus high-pass pair so the three bands sum to an all-pass response. Levels are detected linked across channels with a one-pole attack/release envelope. With threshold `T`, ratio `R` and knee `W`, the gain reduction in dB for input level `L` and `o = L - T` is

```
0                                   for 2o < -W
(1/R - 1)(o + W/2)^2 / (2W)         for 2|o| <= W
o/R - o                             for 2o > W
```

In match mode the ratio is

```
R = clamp(1 + (max(0, PLR_in - PLR_ref) + 0.5 max(0, LRA_in - LRA_ref)) / 4, 1, 4)
```

with `PLR = truePeak - integratedLoudness`, and thresholds are set to the measured band RMS levels of the input. Compression is skipped when `R < 1.05`.

## Soft clipper

With ceiling `c` and threshold `t = c 10^(-knee/20)`:

```
y = x                                        |x| <= t
y = sign(x) (t + (c - t) tanh((|x| - t) / (c - t)))   |x| > t
```

## Limiter

The detector computes the oversampled peak `p[n]` over all channels. The required gain for sample `j` is

```
r[j] = min(1, c / max(p[j-1], p[j]))
```

with `c` the ceiling minus the safety margin. The minimum of `r` over the next `L` samples is taken with a monotonic queue, passed through an instant-attack, exponential-release envelope, and smoothed with a moving average of `L + 1` samples. Because every averaged value is at most `r[j]`, the applied gain never exceeds the required gain. Audio is delayed by `L + 12` samples so the gain is applied in advance of the peak.

## Loudness loop

1. Measure `L0` with EQ and dynamics only, set `g = target - L0`.
2. Run the full chain at gain `g`, measure `L`, error `e = target - L`.
3. Update `g <- g + e / s` with `s` the measured slope `dL/dg` clamped to 0.15 to 1.
4. Stop when `|e|` is within tolerance or the pass limit is reached, then write with the best gain found.

## Dither

TPDF dither is the sum of two uniform variables minus one, in LSB units. With noise shaping the previous quantisation error `e` is fed back:

```
v = x scale - e[n-1]
q = floor(v + d + 0.5)
e[n] = q - v
```

giving a noise transfer function of `1 - z^-1`.
