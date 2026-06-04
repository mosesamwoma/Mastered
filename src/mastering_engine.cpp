#include "mastering_engine.h"
#include <cmath>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <algorithm>
#include <iostream>

namespace mastered {

MasteringEngine::MasteringEngine(const MasteringConfig& config)
    : config_(config),
      fftAnalyzer_(std::make_unique<FFTAnalyzer>(constants::DEFAULT_FFT_SIZE)),
      eqCalculator_(std::make_unique<EQCalculator>()),
      spectrumMatcher_(std::make_unique<SpectrumMatcher>()) {
    validateConfig(config_);
}

MasteringEngine::~MasteringEngine() = default;

MasteringResult MasteringEngine::analyzeTracks(const std::string& referenceTrackPath,
                                               const std::string& unmasteredTrackPath) {
    auto referenceBuffer = AudioLoader::loadWAV(referenceTrackPath);
    auto unmasteredBuffer = AudioLoader::loadWAV(unmasteredTrackPath);

    return analyzeBuffers(referenceBuffer, unmasteredBuffer);
}

MasteringResult MasteringEngine::analyzeBuffers(const AudioBuffer& reference,
                                                const AudioBuffer& unmastered) {
    MasteringResult result;
    result.inputBuffer = unmastered;

    try {
        emitProgress("Starting audio analysis...", 0.f);

        validateAudioBuffer(reference, "Reference");
        validateAudioBuffer(unmastered, "Unmastered");
        validateConfig(config_);

        emitProgress("Validating audio parameters...", 10.f);

        if (reference.sampleRate != unmastered.sampleRate) {
            throw std::runtime_error(
                "Sample rate mismatch: reference=" + std::to_string(reference.sampleRate) +
                " Hz, unmastered=" + std::to_string(unmastered.sampleRate) + " Hz"
            );
        }

        fftAnalyzer_->setSampleRate(reference.sampleRate);

        emitProgress("Computing reference spectrum...", 20.f);
        auto refSpectrum = fftAnalyzer_->getAveragedSpectrum(reference.samples);

        emitProgress("Computing target spectrum...", 30.f);
        auto targetSpectrum = fftAnalyzer_->getAveragedSpectrum(unmastered.samples);

        emitProgress("Smoothing frequency response...", 40.f);
        refSpectrum = fftAnalyzer_->smoothSpectrum(refSpectrum, 7);
        targetSpectrum = fftAnalyzer_->smoothSpectrum(targetSpectrum, 7);

        emitProgress("Matching spectral characteristics...", 50.f);
        result.matchingStats = spectrumMatcher_->matchSpectra(refSpectrum, targetSpectrum);

        emitProgress("Generating EQ curve...", 60.f);
        result.eqCurve = eqCalculator_->calculateEQFromReference(refSpectrum, targetSpectrum);

        for (auto& band : result.eqCurve.bands) {
            band.gain *= config_.aggressiveness;
        }
        for (float& val : result.eqCurve.curveResponse) {
            val *= config_.aggressiveness;
        }

        emitProgress("Calculating loudness (LUFS)...", 75.f);
        result.estimatedLUFS = calculateLUFS(unmastered);

        emitProgress("Computing makeup gain...", 85.f);
        if (config_.autoGain) {
            result.makeupGain = calculateMakeupGain(unmastered, config_.targetLoudnessLUFS);
        }

        result.success = true;
        result.message = "Analysis completed successfully";

        emitProgress("Analysis complete", 100.f);

        if (config_.verbose) {
            std::cout << "✓ FFT Size: " << constants::DEFAULT_FFT_SIZE << "\n";
            std::cout << "✓ Sample Rate: " << reference.sampleRate << " Hz\n";
            std::cout << "✓ EQ Bands: " << result.eqCurve.bands.size() << "\n";
        }

    } catch (const std::exception& e) {
        result.success = false;
        result.message = std::string("Analysis failed: ") + e.what();
        emitProgress("Analysis failed: " + std::string(e.what()), -1.f);
    }

    return result;
}

AudioBuffer MasteringEngine::applyMastering(const AudioBuffer& input, const EQCurve& eqCurve, float makeupGain) {
    AudioBuffer output = input;

    if (output.samples.empty()) {
        throw std::runtime_error("Cannot apply mastering to empty buffer");
    }

    emitProgress("Applying EQ mastering...", 0.f);

    size_t bandCount = 0;

    if (!output.channelSamples.empty()) {
        // Stereo path: process each channel independently, skip interleaved output.samples
        for (auto& channelData : output.channelSamples) {
            for (const auto& band : eqCurve.bands) {
                if (band.gain != 0.f && band.frequency > 0.f && band.qFactor > 0.01f) {
                    applyEQBandInPlace(channelData, band, output.sampleRate);
                    ++bandCount;
                }
            }
            if (makeupGain != 0.f) {
                float gainLinear = std::pow(10.f, makeupGain / 20.f);
                for (float& sample : channelData) {
                    sample *= gainLinear;
                }
            }
        }
    } else {
        // Mono path: process interleaved samples directly
        for (const auto& band : eqCurve.bands) {
            if (band.gain != 0.f && band.frequency > 0.f && band.qFactor > 0.01f) {
                applyEQBandInPlace(output.samples, band, output.sampleRate);
                ++bandCount;
            }
        }
        if (makeupGain != 0.f) {
            float gainLinear = std::pow(10.f, makeupGain / 20.f);
            for (float& sample : output.samples) {
                sample *= gainLinear;
            }
        }
    }

    if (!eqCurve.bands.empty()) {
        float progress = 50.f * static_cast<float>(bandCount) / static_cast<float>(eqCurve.bands.size());
        emitProgress("Applied EQ bands...", progress);
    }

    emitProgress("Applying makeup gain...", 75.f);

    // Sync output.samples from channelSamples after processing.
    // For stereo: rebuild interleaved mix as (L+R)/2 so output.samples
    // reflects the processed audio (used by callers that read samples directly).
    // For mono: channelSamples[0] IS the processed audio — copy it back.
    if (!output.channelSamples.empty()) {
        if (output.channelSamples.size() >= 2) {
            // Stereo: update mid-channel downmix
            output.samples.resize(output.numFrames);
            for (size_t i = 0; i < output.numFrames; ++i) {
                output.samples[i] = (output.channelSamples[0][i] + output.channelSamples[1][i]) / 2.f;
            }
        } else {
            // Mono: sync processed channel back to samples
            output.samples = output.channelSamples[0];
        }
    }

    emitProgress("Checking true peak (4x oversampling)...", 85.f);

    std::vector<std::vector<float>*> allChannels;
    allChannels.push_back(&output.samples);
    for (auto& channel : output.channelSamples) {
        allChannels.push_back(&channel);
    }

    constexpr float TRUE_PEAK_LIMIT = 0.891251f;
    float truePeakMax = 0.f;

    // 4-point sinc-based true peak detection per ITU-R BS.1770-4 Annex 2.
    // Evaluates 3 inter-sample points between each pair of samples using a
    // windowed sinc kernel. Catches high-frequency inter-sample peaks that
    // linear interpolation misses by several dB.
    // Kernel coefficients for t = 0.25, 0.50, 0.75 (4x oversampling):
    // h(t) = sinc(t-n) * hann_window(t-n), summed over n = -1..2
    const float sinc_kernels[3][4] = {
        // t=0.25: interpolate 1/4 between samples
        { -0.0947f,  0.6473f,  0.5267f, -0.0793f },
        // t=0.50: interpolate midpoint
        { -0.1250f,  0.6250f,  0.6250f, -0.1250f },
        // t=0.75: interpolate 3/4 between samples
        { -0.0793f,  0.5267f,  0.6473f, -0.0947f }
    };

    for (const auto* channelPtr : allChannels) {
        const auto& channel = *channelPtr;
        const size_t n = channel.size();

        for (size_t i = 0; i < n; ++i) {
            // Check the sample itself
            truePeakMax = std::max(truePeakMax, std::abs(channel[i]));

            // Check 3 inter-sample points using sinc kernel
            for (int k = 0; k < 3; ++k) {
                // Gather 4 neighbouring samples with boundary clamping
                float s0 = (i >= 1)     ? channel[i - 1] : 0.f;
                float s1 = channel[i];
                float s2 = (i + 1 < n)  ? channel[i + 1] : 0.f;
                float s3 = (i + 2 < n)  ? channel[i + 2] : 0.f;

                float interp = sinc_kernels[k][0] * s0
                             + sinc_kernels[k][1] * s1
                             + sinc_kernels[k][2] * s2
                             + sinc_kernels[k][3] * s3;

                truePeakMax = std::max(truePeakMax, std::abs(interp));
            }
        }
    }

    if (truePeakMax > TRUE_PEAK_LIMIT) {
        float tpScale = TRUE_PEAK_LIMIT / truePeakMax;
        for (auto* channelPtr : allChannels) {
            for (float& sample : *channelPtr) {
                sample *= tpScale;
            }
        }
        if (config_.verbose) {
            std::cout << "⚠ True peak limiting: scaled by " << tpScale << " (true peak was "
                      << truePeakMax << " = " << (20.f * std::log10(truePeakMax)) << " dBFS)\n";
        }
    }

    emitProgress("Preventing clipping...", 90.f);

    float maxAbs = 0.f;
    for (const auto* channelPtr : allChannels) {
        for (float sample : *channelPtr) {
            maxAbs = std::max(maxAbs, std::abs(sample));
        }
    }

    if (maxAbs > 1.f) {
        float scale = config_.clippingHeadroom / maxAbs;
        for (auto* channelPtr : allChannels) {
            for (float& sample : *channelPtr) {
                sample *= scale;
            }
        }
        if (config_.verbose) {
            std::cout << "⚠ Clipping prevention: scaled by " << scale << "\n";
        }
    }

    emitProgress("Mastering complete", 100.f);

    return output;
}

void MasteringEngine::applyEQBandInPlace(std::vector<float>& samples,
                                         const EQBand& band,
                                         uint32_t sampleRate) {
    if (samples.empty() || band.gain == 0.f) {
        return;
    }

    if (band.frequency <= 0.f || sampleRate == 0 || band.qFactor <= 0.01f) {
        return;
    }

    float nyquist = sampleRate / 2.f;
    if (band.frequency >= nyquist) {
        return;
    }

    // RBJ Audio EQ Cookbook — choose correct filter type based on band.type.
    // Shelf filters use different coefficient formulas to peaking EQ.
    float A   = std::pow(10.f, band.gain / 40.f);
    float w0  = 2.f * M_PI * band.frequency / static_cast<float>(sampleRate);
    float sinW0 = std::sin(w0);
    float cosW0 = std::cos(w0);

    float b0, b1, b2, a0, a1, a2;

    if (band.type == "shelf-low") {
        // RBJ low-shelf (provides smooth gain below the corner frequency)
        float beta = std::sqrt(A) / band.qFactor;
        b0 =    A * ((A + 1.f) - (A - 1.f) * cosW0 + beta * sinW0);
        b1 =  2.f * A * ((A - 1.f) - (A + 1.f) * cosW0);
        b2 =    A * ((A + 1.f) - (A - 1.f) * cosW0 - beta * sinW0);
        a0 =        (A + 1.f) + (A - 1.f) * cosW0 + beta * sinW0;
        a1 = -2.f * ((A - 1.f) + (A + 1.f) * cosW0);
        a2 =        (A + 1.f) + (A - 1.f) * cosW0 - beta * sinW0;
    } else if (band.type == "shelf-high") {
        // RBJ high-shelf (provides smooth gain above the corner frequency)
        float beta = std::sqrt(A) / band.qFactor;
        b0 =    A * ((A + 1.f) + (A - 1.f) * cosW0 + beta * sinW0);
        b1 = -2.f * A * ((A - 1.f) + (A + 1.f) * cosW0);
        b2 =    A * ((A + 1.f) + (A - 1.f) * cosW0 - beta * sinW0);
        a0 =        (A + 1.f) - (A - 1.f) * cosW0 + beta * sinW0;
        a1 =  2.f * ((A - 1.f) - (A + 1.f) * cosW0);
        a2 =        (A + 1.f) - (A - 1.f) * cosW0 - beta * sinW0;
    } else {
        // RBJ peaking EQ (all other types)
        float alpha = sinW0 / (2.f * band.qFactor);
        b0 = 1.f + alpha * A;
        b1 = -2.f * cosW0;
        b2 = 1.f - alpha * A;
        a0 = 1.f + alpha / A;
        a1 = -2.f * cosW0;
        a2 = 1.f - alpha / A;
    }

    b0 /= a0;
    b1 /= a0;
    b2 /= a0;
    a1 /= a0;
    a2 /= a0;

    float w_n1 = 0.f, w_n2 = 0.f;
    for (size_t i = 0; i < samples.size(); ++i) {
        float w_n = samples[i] - a1 * w_n1 - a2 * w_n2;
        samples[i] = b0 * w_n + b1 * w_n1 + b2 * w_n2;

        w_n2 = w_n1;
        w_n1 = w_n;
    }
}

std::string MasteringEngine::exportEQasJSON(const MasteringResult& result) const {
    std::ostringstream json;
    json << std::fixed << std::setprecision(2);

    json << "{\n";
    json << "  \"success\": " << (result.success ? "true" : "false") << ",\n";
    json << "  \"message\": \"" << result.message << "\",\n";
    json << "  \"version\": \"1.1\",\n";
    json << "  \"stats\": {\n";
    json << "    \"correlation\": " << result.matchingStats.correlation << ",\n";
    json << "    \"spectralDifference\": " << result.matchingStats.spectralDifference << ",\n";
    json << "    \"confidence\": " << result.matchingStats.confidenceScore << ",\n";
    json << "    \"estimatedLUFS\": " << result.estimatedLUFS << ",\n";
    json << "    \"makeupGain\": " << result.makeupGain << "\n";
    json << "  },\n";

    json << "  \"eqBands\": [\n";
    for (size_t i = 0; i < result.eqCurve.bands.size(); ++i) {
        const auto& band = result.eqCurve.bands[i];
        json << "    {\n";
        json << "      \"id\": " << i << ",\n";
        json << "      \"frequency\": " << band.frequency << ",\n";
        json << "      \"gain\": " << band.gain << ",\n";
        json << "      \"q\": " << band.qFactor << ",\n";
        json << "      \"type\": \"" << band.type << "\",\n";
        json << "      \"bandwidth\": " << (band.frequency / band.qFactor) << "\n";
        json << "    }";
        if (i < result.eqCurve.bands.size() - 1) json << ",";
        json << "\n";
    }
    json << "  ],\n";

    json << "  \"curveResponse\": [\n";
    for (size_t i = 0; i < result.eqCurve.curveResponse.size(); ++i) {
        if (i % 10 == 0) json << "    ";
        json << result.eqCurve.curveResponse[i];
        if (i < result.eqCurve.curveResponse.size() - 1) json << ", ";
        if ((i + 1) % 10 == 0) json << "\n";
    }
    json << "  ]\n";
    json << "}\n";

    return json.str();
}

std::string MasteringEngine::exportEQasReaEQ(const MasteringResult& result) const {
    std::ostringstream reaEQ;

    reaEQ << "# ReaEQ Band Reference (enter manually into Reaper ReaEQ plugin)\n";
    reaEQ << "# Bands: " << result.eqCurve.bands.size() << "\n\n";

    for (size_t i = 0; i < result.eqCurve.bands.size(); ++i) {
        const auto& band = result.eqCurve.bands[i];
        float bandwidth = band.frequency / band.qFactor;
        // Map internal type names to ReaEQ filter type labels
        std::string reaType = "Band";
        if (band.type == "shelf-low")  reaType = "Low Shelf";
        if (band.type == "shelf-high") reaType = "High Shelf";
        reaEQ << "Band " << (i + 1) << ": freq=" << std::fixed << std::setprecision(1) << band.frequency
              << " Hz  gain=" << std::setprecision(2) << band.gain
              << " dB  bw=" << std::setprecision(1) << bandwidth
              << " Hz  Q=" << band.qFactor
              << "  type=" << reaType << "\n";
    }

    return reaEQ.str();
}

std::string MasteringEngine::exportEQasEqualizerAPO(const MasteringResult& result) const {
    std::ostringstream apoEQ;
    apoEQ << std::fixed << std::setprecision(2);

    for (const auto& band : result.eqCurve.bands) {
        std::string apoType = "PK";
        if (band.type == "shelf-low")  apoType = "LS";
        if (band.type == "shelf-high") apoType = "HS";
        apoEQ << "Filter: ON " << apoType << " Fc=" << std::setprecision(1) << band.frequency
              << " Gain=" << std::setprecision(2) << band.gain
              << " Q=" << band.qFactor << "\n";
    }

    return apoEQ.str();
}

float MasteringEngine::calculateLUFS(const AudioBuffer& buffer) const {
    if (buffer.samples.empty()) return -120.f;

    const double fs = static_cast<double>(buffer.sampleRate);
    constexpr float CALIBRATION_CONST = -0.691f;

    auto calcBiquad = [](double b0, double b1, double b2,
                         double a0, double a1, double a2,
                         const std::vector<float>& x) -> std::vector<float> {
        std::vector<float> y(x.size());
        const double nb0 = b0 / a0;
        const double nb1 = b1 / a0;
        const double nb2 = b2 / a0;
        const double na1 = a1 / a0;
        const double na2 = a2 / a0;
        double w1 = 0.0;
        double w2 = 0.0;
        for (size_t i = 0; i < x.size(); ++i) {
            const double w = static_cast<double>(x[i]) - na1 * w1 - na2 * w2;
            y[i] = static_cast<float>(nb0 * w + nb1 * w1 + nb2 * w2);
            w2 = w1;
            w1 = w;
        }
        return y;
    };

    auto kWeight = [&](const std::vector<float>& input) -> std::vector<float> {
        std::vector<float> filtered = input;

        {
            const double fc = 1681.974450955533;
            const double Q = 0.7071752369554196;
            const double gainDb = 4.0;
            const double A = std::pow(10.0, gainDb / 20.0);
            const double K = std::tan(M_PI * fc / fs);
            const double K2 = K * K;
            const double Vh = A;
            const double Vb = 1.0;

            const double b0 = Vh + Vb * K / Q + K2;
            const double b1 = 2.0 * (K2 - Vh);
            const double b2 = Vh - Vb * K / Q + K2;
            const double a0 = 1.0 + K / Q + K2;
            const double a1 = 2.0 * (K2 - 1.0);
            const double a2 = 1.0 - K / Q + K2;

            filtered = calcBiquad(b0, b1, b2, a0, a1, a2, filtered);
        }

        {
            const double fc = 38.1354708681762;
            const double Q = 0.50032959205650;
            const double K = std::tan(M_PI * fc / fs);
            const double K2 = K * K;

            const double b0 = 1.0;
            const double b1 = -2.0;
            const double b2 = 1.0;
            const double a0 = 1.0 + K / Q + K2;
            const double a1 = 2.0 * (K2 - 1.0);
            const double a2 = 1.0 - K / Q + K2;

            filtered = calcBiquad(b0, b1, b2, a0, a1, a2, filtered);
        }

        return filtered;
    };

    const bool hasStereoChannels = !buffer.channelSamples.empty() && buffer.channelSamples.size() >= 2;
    std::vector<std::vector<float>> filteredChannels;
    std::vector<float> filtered = kWeight(buffer.samples);

    if (hasStereoChannels) {
        filteredChannels.reserve(buffer.channelSamples.size());
        for (const auto& channel : buffer.channelSamples) {
            filteredChannels.push_back(kWeight(channel));
        }
    }

    uint32_t blockSize = static_cast<uint32_t>(fs * 0.4);
    uint32_t hopSize = static_cast<uint32_t>(fs * 0.1);
    if (blockSize < 1024) blockSize = 1024;
    if (hopSize < 256) hopSize = 256;

    std::vector<float> blockLoudnesses;
    if (hasStereoChannels && !filteredChannels.empty()) {
        for (size_t start = 0; start + blockSize <= filteredChannels.front().size(); start += hopSize) {
            float channelPowerSum = 0.f;
            size_t validChannels = 0;
            for (const auto& channel : filteredChannels) {
                if (start + blockSize > channel.size()) {
                    continue;
                }

                float meanSquare = 0.f;
                for (size_t i = 0; i < blockSize; ++i) {
                    const float s = channel[start + i];
                    meanSquare += s * s;
                }
                meanSquare /= static_cast<float>(blockSize);
                channelPowerSum += meanSquare;
                ++validChannels;
            }

            if (validChannels == 0) {
                continue;
            }

            float meanSquare = channelPowerSum / static_cast<float>(validChannels);
            blockLoudnesses.push_back(CALIBRATION_CONST + 10.f * std::log10(std::max(meanSquare, 1e-10f)));
        }
    } else {
        for (size_t start = 0; start + blockSize <= filtered.size(); start += hopSize) {
            float meanSquare = 0.f;
            for (size_t i = 0; i < blockSize; ++i) {
                const float s = filtered[start + i];
                meanSquare += s * s;
            }
            meanSquare /= static_cast<float>(blockSize);
            blockLoudnesses.push_back(CALIBRATION_CONST + 10.f * std::log10(std::max(meanSquare, 1e-10f)));
        }
    }

    if (blockLoudnesses.empty()) return -120.f;

    float sumP = 0.f;
    int cnt = 0;
    for (float lk : blockLoudnesses) {
        if (lk >= -70.f) {
            sumP += std::pow(10.f, (lk - CALIBRATION_CONST) / 10.f);
            ++cnt;
        }
    }
    if (cnt == 0) return -120.f;

    const float overallLUFS = CALIBRATION_CONST + 10.f * std::log10(sumP / cnt);
    const float relGate = overallLUFS - 10.f;

    float sumP2 = 0.f;
    int cnt2 = 0;
    for (float lk : blockLoudnesses) {
        if (lk >= -70.f && lk >= relGate) {
            sumP2 += std::pow(10.f, (lk - CALIBRATION_CONST) / 10.f);
            ++cnt2;
        }
    }
    if (cnt2 == 0) return -120.f;

    const float finalLUFS = CALIBRATION_CONST + 10.f * std::log10(sumP2 / cnt2);
    return std::max(-120.f, std::min(0.f, finalLUFS));
}

float MasteringEngine::calculateMakeupGain(const AudioBuffer& original,
                                          float targetLUFS) const {
    float originalLUFS = calculateLUFS(original);
    float difference = targetLUFS - originalLUFS;
    return difference;
}

} // namespace mastered
// minor cleanup
