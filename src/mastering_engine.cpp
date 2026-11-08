#include "mastering_engine.h"

#include "constants.h"
#include "dynamics.h"
#include "error.h"
#include "presets.h"
#include "wav_io.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace mastered {

namespace {

constexpr double kMinLevelHz = 63.0;
constexpr double kMaxLevelHz = 12500.0;
constexpr double kSideFloorDb = -45.0;
constexpr double kBassBoostFraction = 2.0 / 3.0;
constexpr double kBassBoostHz = 160.0;
constexpr double kAutoClipReductionDb = 4.0;
constexpr double kSaturationSlope = 0.2;
constexpr double kKneeToleranceLu = 0.25;
constexpr int kMaxKneeRefinements = 3;
constexpr double kGenreAggressivenessScale = 0.6;
constexpr double kGenreMaxBoostDb = 4.0;
constexpr double kGenreMaxCutDb = 5.0;
constexpr double kAutoSubsonicHz = 20.0;
constexpr double kDeEsserMinSpreadDb = 6.0;
constexpr double kAutoMonoBassHz = 120.0;
constexpr double kHeavyAverageReductionDb = 2.0;
constexpr double kHeavyMaxReductionDb = 8.0;
constexpr double kMaxWidthDb = 3.0;
constexpr double kMaxMakeupDb = 40.0;
constexpr double kPeakSafetyDb = 0.02;
constexpr double kPeakWindowDb = 0.08;
constexpr double kMaxCeilingRaiseDb = 0.6;
constexpr int kMaxPeakTrims = 5;
constexpr int kMaxCeilingRaises = 2;
constexpr double kEmptyBandDb = -50.0;
constexpr size_t kWaveformColumns = 800;
constexpr double kResonanceMinExcessDb = 5.0;
constexpr double kResonanceMinHz = 1500.0;
constexpr double kGlueThresholdBelowCeilingDb = 9.0;
constexpr double kCoarseMinSeconds = 100.0;
constexpr size_t kCoarseSegments = 8;
constexpr double kCoarseSegmentSeconds = 4.0;

struct FileScan {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitDepth = 0;
    bool isFloat = false;
    uint64_t frames = 0;
    AverageSpectrum spectrum;
    Measurements measurements;
    Waveform waveform;
    uint64_t nonFinite = 0;
};

void requireFinite(double value, const char* name) {
    if (!std::isfinite(value)) {
        throw MasteredError(std::string("Configuration value is not finite: ") + name);
    }
}

void requireRange(double value, double low, double high, const char* name) {
    requireFinite(value, name);
    if (value < low || value > high) {
        std::ostringstream message;
        message << "Configuration value out of range: " << name << " must be between " << low << " and " << high;
        throw MasteredError(message.str());
    }
}

void validateConfig(const MasteringConfig& c) {
    requireRange(c.targetLufs, -60.0, -1.0, "targetLufs");
    requireRange(c.aggressiveness, 0.0, 1.0, "aggressiveness");
    requireRange(c.ceilingDb, -12.0, 0.0, "ceilingDb");
    requireRange(static_cast<double>(c.maxEQBands), 0.0, 32.0, "maxEQBands");
    requireRange(static_cast<double>(c.maxSideBands), 0.0, 32.0, "maxSideBands");
    requireRange(c.dynamicsRatio, 1.0, 20.0, "dynamicsRatio");
    requireRange(c.dynamicsThresholdOffsetDb, -24.0, 24.0, "dynamicsThresholdOffsetDb");
    requireRange(c.softClipKneeDb, 0.1, 12.0, "softClipKneeDb");
    requireFinite(c.monoBassHz, "monoBassHz");
    if (c.monoBassHz != 0.0 && (c.monoBassHz < 20.0 || c.monoBassHz > 500.0)) {
        throw MasteredError("Configuration value out of range: monoBassHz must be 0 (off) or between 20 and 500");
    }
    requireRange(c.maxBoostDb, 0.0, 12.0, "maxBoostDb");
    requireRange(c.maxCutDb, 0.0, 18.0, "maxCutDb");
    requireFinite(c.punchGuardPlrDb, "punchGuardPlrDb");
    if (c.punchGuardPlrDb != 0.0 && (c.punchGuardPlrDb < 3.0 || c.punchGuardPlrDb > 16.0)) {
        throw MasteredError("Configuration value out of range: punchGuardPlrDb must be 0 (off) or between 3 and 16");
    }
    requireRange(c.limiterLookaheadMs, 0.5, 20.0, "limiterLookaheadMs");
    requireRange(c.limiterReleaseMs, 5.0, 1000.0, "limiterReleaseMs");
    requireRange(c.loudnessToleranceLu, 0.005, 2.0, "loudnessToleranceLu");
    requireRange(static_cast<double>(c.maxLoudnessIterations), 1.0, 30.0, "maxLoudnessIterations");
    if (c.bitDepth != 0 && c.bitDepth != 16 && c.bitDepth != 24 && c.bitDepth != 32) {
        throw MasteredError("Configuration value out of range: bitDepth must be 0, 16, 24 or 32");
    }
    if (c.dynamicsBands < 2 || c.dynamicsBands > kMaxDynamicsBands) {
        throw MasteredError("Configuration value out of range: dynamicsBands must be between 2 and 5");
    }
    if (!c.genre.empty() && findGenre(c.genre) == nullptr) {
        throw MasteredError("Configuration value out of range: genre is not a known target curve");
    }
    requireFinite(c.subsonicHz, "subsonicHz");
    if (c.subsonicHz != 0.0 && (c.subsonicHz < 10.0 || c.subsonicHz > 60.0)) {
        throw MasteredError("Configuration value out of range: subsonicHz must be 0 (off) or between 10 and 60");
    }
    if (c.fftSize < 1024 || c.fftSize > 65536 || (c.fftSize & (c.fftSize - 1)) != 0) {
        throw MasteredError("Configuration value out of range: fftSize must be a power of two between 1024 and 65536");
    }
}

FileScan scanFile(const std::string& path, size_t fftSize, bool dualMono) {
    WavReader reader(path);
    const uint64_t required = std::max<uint64_t>(reader.sampleRate(), fftSize);
    if (reader.frames() < required) {
        throw MasteredError(path + ": audio is too short, at least " + std::to_string(required) + " frames are required");
    }
    FileScan scan;
    scan.sampleRate = reader.sampleRate();
    scan.channels = reader.channels();
    scan.bitDepth = reader.bitDepth();
    scan.isFloat = reader.isFloat();
    scan.frames = reader.frames();
    SpectrumAccumulator spectrum(scan.sampleRate, scan.channels, fftSize);
    Meter meter(scan.sampleRate, scan.channels, PeakMode::Standard, dualMono);
    WaveformBuilder waveform(scan.frames, kWaveformColumns);
    AudioBlock block;
    size_t n;
    while ((n = reader.read(block, kBlockFrames)) > 0) {
        spectrum.push(block, n);
        meter.process(block, n);
        waveform.process(block, n);
    }
    scan.nonFinite = reader.nonFiniteSamples();
    scan.waveform = waveform.finish();
    scan.spectrum = spectrum.finish();
    scan.measurements = meter.finish();
    return scan;
}

std::vector<BandStatistics> measureBands(const std::string& path, const std::vector<double>& crossovers) {
    WavReader reader(path);
    BandLevelMeter meter(reader.sampleRate(), crossovers);
    AudioBlock block;
    size_t n;
    while ((n = reader.read(block, kBlockFrames)) > 0) {
        meter.process(block, n);
    }
    return meter.statistics();
}

BandStatistics measureSibilance(const std::string& path, double lowHz, double highHz) {
    WavReader reader(path);
    SibilanceMeter meter(reader.sampleRate(), lowHz, highHz);
    AudioBlock block;
    size_t n;
    while ((n = reader.read(block, kBlockFrames)) > 0) {
        meter.process(block, n);
    }
    return meter.statistics();
}

std::vector<double> chooseCrossovers(size_t bandCount, const AverageSpectrum& spectrum, double sampleRate, bool adaptive, const std::vector<double>& centres,
                                     double width) {
    std::vector<double> crossovers = defaultCrossovers(bandCount, sampleRate);
    if (!adaptive || centres.size() < 3) {
        return crossovers;
    }
    const std::vector<double> levels = bandLevelsDb(spectrum, false, centres, width);
    std::vector<double> chosen;
    for (double nominal : crossovers) {
        double best = nominal;
        double bestLevel = 1e9;
        for (size_t i = 1; i + 1 < centres.size(); ++i) {
            if (centres[i] < nominal / 1.26 || centres[i] > nominal * 1.26) {
                continue;
            }
            const double smooth = (levels[i - 1] + levels[i] + levels[i + 1]) / 3.0;
            if (smooth < bestLevel) {
                bestLevel = smooth;
                best = centres[i];
            }
        }
        if (!chosen.empty() && best < chosen.back() * 1.6) {
            best = nominal;
        }
        if (!chosen.empty() && best <= chosen.back()) {
            best = chosen.back() * 1.6;
        }
        if (best >= sampleRate * 0.45) {
            break;
        }
        chosen.push_back(best);
    }
    return chosen.empty() ? crossovers : chosen;
}

std::vector<BandStatistics> measureDynamics(const std::string& input, const DynamicsSettings& settings) {
    WavReader reader(input);
    BandLevelMeter meter(reader.sampleRate(), settings.crossovers());
    MultibandCompressor compressor(settings, reader.sampleRate(), reader.channels());
    AudioBlock block;
    size_t n;
    while ((n = reader.read(block, kBlockFrames)) > 0) {
        compressor.process(block, n);
        meter.process(block, n);
    }
    return meter.statistics();
}

double meanOverRange(const std::vector<double>& values, const std::vector<size_t>& range) {
    double sum = 0.0;
    for (size_t i : range) {
        sum += values[i];
    }
    return sum / static_cast<double>(range.size());
}

std::vector<double> shapeOf(const std::vector<double>& levels, const std::vector<size_t>& range) {
    const double mean = meanOverRange(levels, range);
    std::vector<double> shape(levels.size());
    for (size_t i = 0; i < levels.size(); ++i) {
        shape[i] = levels[i] - mean;
    }
    return shape;
}

CurveFit fitCurve(const std::vector<double>& centres, const std::vector<double>& target, size_t maxBands, double sampleRate, double maxHz, bool perceptual,
                  double noBoostAboveHz = 0.0) {
    FitOptions options;
    options.noBoostAboveHz = noBoostAboveHz;
    options.sampleRate = sampleRate;
    options.maxHz = maxHz;
    options.maxBands = maxBands;
    options.perceptualWeighting = perceptual;
    const FitResult fit = fitEQ(centres, target, options);
    CurveFit curve;
    curve.bands = fit.bands;
    curve.frequenciesHz = fit.gridHz;
    curve.targetDb = fit.targetDb;
    curve.responseDb = fit.responseDb;
    curve.residualRmsDb = fit.residualRmsDb;
    return curve;
}

std::vector<double> referenceShapeOf(const std::vector<const AverageSpectrum*>& references, bool side, const std::vector<double>& centres, double width,
                                     const std::vector<size_t>& range) {
    std::vector<double> referenceShape(centres.size(), 0.0);
    for (const AverageSpectrum* reference : references) {
        const std::vector<double> shape = shapeOf(bandLevelsDb(*reference, side, centres, width), range);
        for (size_t i = 0; i < shape.size(); ++i) {
            referenceShape[i] += shape[i] / static_cast<double>(references.size());
        }
    }
    return referenceShape;
}

std::vector<double> buildTarget(const AverageSpectrum& input, const std::vector<double>& referenceShape, bool side, const std::vector<double>& centres,
                                double width, const std::vector<size_t>& range, double aggressiveness, double maxCutDb, double maxBoostDb) {
    const std::vector<double> inputShape = shapeOf(bandLevelsDb(input, side, centres, width), range);
    std::vector<double> difference(centres.size());
    for (size_t i = 0; i < difference.size(); ++i) {
        difference[i] = referenceShape[i] - inputShape[i];
    }
    std::vector<double> inRange;
    inRange.reserve(range.size());
    for (size_t i : range) {
        inRange.push_back(difference[i]);
    }
    std::sort(inRange.begin(), inRange.end());
    const size_t middle = inRange.size() / 2;
    const double offset = inRange.size() % 2 == 1 ? inRange[middle] : 0.5 * (inRange[middle - 1] + inRange[middle]);
    std::vector<double> target(centres.size());
    for (size_t i = 0; i < target.size(); ++i) {
        double value = (difference[i] - offset) * aggressiveness;
        if (inputShape[i] < kEmptyBandDb) {
            value = std::min(value, 0.0);
        }
        const double maxBoost = centres[i] < kBassBoostHz ? maxBoostDb * kBassBoostFraction : maxBoostDb;
        target[i] = std::min(std::max(value, -maxCutDb), maxBoost);
    }
    return target;
}

class TempFile {
public:
    explicit TempFile(std::string path) : path_(std::move(path)) {}
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    ~TempFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

class CacheWriter {
public:
    CacheWriter(const std::string& path, size_t channels) : channels_(channels), file_(path, std::ios::binary | std::ios::trunc) {
        if (!file_) {
            throw MasteredError("Cannot create the temporary working file next to the output: " + path);
        }
    }

    void write(const AudioBlock& block, size_t frames) {
        buffer_.resize(frames * channels_);
        for (size_t i = 0; i < frames; ++i) {
            for (size_t ch = 0; ch < channels_; ++ch) {
                buffer_[i * channels_ + ch] = static_cast<float>(block[ch][i]);
            }
        }
        file_.write(reinterpret_cast<const char*>(buffer_.data()), static_cast<std::streamsize>(buffer_.size() * sizeof(float)));
        if (!file_) {
            throw MasteredError("Failed to write the temporary working file (is the disk full?)");
        }
    }

    void close() {
        file_.flush();
        if (!file_) {
            throw MasteredError("Failed to write the temporary working file (is the disk full?)");
        }
        file_.close();
    }

private:
    size_t channels_;
    std::ofstream file_;
    std::vector<float> buffer_;
};

class CacheReader {
public:
    CacheReader(const std::string& path, size_t channels, uint64_t frames)
        : channels_(channels), total_(frames), remaining_(frames), file_(path, std::ios::binary) {
        if (!file_) {
            throw MasteredError("Cannot read the temporary working file: " + path);
        }
    }

    void seek(uint64_t frame) {
        const uint64_t position = std::min(frame, total_);
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(position * channels_ * sizeof(float)));
        remaining_ = total_ - position;
    }

    size_t read(AudioBlock& block, size_t maxFrames) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining_, maxFrames));
        if (want == 0) {
            return 0;
        }
        buffer_.resize(want * channels_);
        file_.read(reinterpret_cast<char*>(buffer_.data()), static_cast<std::streamsize>(buffer_.size() * sizeof(float)));
        if (static_cast<size_t>(file_.gcount()) != buffer_.size() * sizeof(float)) {
            throw MasteredError("The temporary working file ended early");
        }
        block.resize(channels_);
        for (size_t ch = 0; ch < channels_; ++ch) {
            block[ch].resize(want);
        }
        for (size_t i = 0; i < want; ++i) {
            for (size_t ch = 0; ch < channels_; ++ch) {
                block[ch][i] = static_cast<double>(buffer_[i * channels_ + ch]);
            }
        }
        remaining_ -= want;
        return want;
    }

private:
    size_t channels_;
    uint64_t total_;
    uint64_t remaining_;
    std::ifstream file_;
    std::vector<float> buffer_;
};

Measurements runPre(const std::string& input, const ChainPlan& plan, const std::string& cachePath, bool dualMono) {
    WavReader reader(input);
    const size_t channels = reader.channels();
    ProcessingChain chain(plan, reader.sampleRate(), channels, ChainStage::Pre);
    Meter meter(reader.sampleRate(), channels, PeakMode::Standard, dualMono);
    CacheWriter cache(cachePath, channels);
    AudioBlock block;
    size_t skip = chain.preLatency();

    auto emitBlock = [&](AudioBlock& data, size_t frames) {
        size_t offset = std::min(skip, frames);
        skip -= offset;
        if (offset >= frames) {
            return;
        }
        if (offset > 0) {
            for (auto& channel : data) {
                channel.erase(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(offset));
            }
        }
        const size_t count = frames - offset;
        meter.process(data, count);
        cache.write(data, count);
    };

    size_t n;
    while ((n = reader.read(block, kBlockFrames)) > 0) {
        chain.processPre(block, n);
        emitBlock(block, n);
    }
    size_t pending = chain.preLatency();
    while (pending > 0) {
        const size_t count = std::min(pending, kBlockFrames);
        AudioBlock tail = makeBlock(channels, count);
        chain.processPre(tail, count);
        emitBlock(tail, count);
        pending -= count;
    }
    cache.close();
    return meter.finish();
}

Measurements runPost(const std::string& cachePath, const Analysis& analysis, const ChainPlan& plan, WavWriter* writer, PeakMode peakMode,
                     bool dualMono, WaveformBuilder* waveform = nullptr, LimiterStats* limiter = nullptr) {
    const size_t channels = analysis.channels;
    CacheReader reader(cachePath, channels, analysis.frames);
    ProcessingChain chain(plan, static_cast<double>(analysis.sampleRate), channels, ChainStage::Post);
    Meter meter(static_cast<double>(analysis.sampleRate), channels, peakMode, dualMono);
    chain.collectLimiterStatistics(limiter != nullptr);
    size_t skip = chain.latency();
    AudioBlock block;

    auto emitBlock = [&](AudioBlock& data, size_t frames) {
        size_t offset = 0;
        if (skip > 0) {
            offset = std::min(skip, frames);
            skip -= offset;
        }
        if (offset >= frames) {
            return;
        }
        if (offset > 0) {
            for (auto& channel : data) {
                channel.erase(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(offset));
            }
        }
        const size_t count = frames - offset;
        meter.process(data, count);
        if (waveform) {
            waveform->process(data, count);
        }
        if (writer) {
            writer->write(data, count);
        }
    };

    size_t n;
    while ((n = reader.read(block, kBlockFrames)) > 0) {
        chain.processPost(block, n);
        emitBlock(block, n);
    }
    size_t pending = chain.latency();
    while (pending > 0) {
        const size_t count = std::min(pending, kBlockFrames);
        AudioBlock tail = makeBlock(channels, count);
        chain.processPost(tail, count);
        emitBlock(tail, count);
        pending -= count;
    }
    if (limiter) {
        *limiter = chain.limiterStatistics();
    }
    return meter.finish();
}

struct Segment {
    uint64_t start;
    uint64_t length;
};

Measurements runPostExcerpts(const std::string& cachePath, const Analysis& analysis, const ChainPlan& plan, const std::vector<Segment>& segments,
                             bool dualMono) {
    const size_t channels = analysis.channels;
    const double sampleRate = static_cast<double>(analysis.sampleRate);
    Meter meter(sampleRate, channels, PeakMode::Off, dualMono);
    for (const Segment& segment : segments) {
        CacheReader reader(cachePath, channels, analysis.frames);
        ProcessingChain chain(plan, sampleRate, channels, ChainStage::Post);
        const uint64_t preroll = std::min<uint64_t>(segment.start, analysis.sampleRate);
        reader.seek(segment.start - preroll);
        uint64_t discard = preroll + chain.latency();
        uint64_t remainingToMeter = segment.length;
        const uint64_t toFeed = preroll + segment.length + chain.latency();
        uint64_t fed = 0;
        AudioBlock block;
        while (fed < toFeed && remainingToMeter > 0) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(toFeed - fed, kBlockFrames));
            size_t n = reader.read(block, want);
            if (n == 0) {
                block = makeBlock(channels, want);
                n = want;
            }
            chain.processPost(block, n);
            fed += n;
            size_t offset = 0;
            if (discard > 0) {
                offset = static_cast<size_t>(std::min<uint64_t>(discard, n));
                discard -= offset;
            }
            if (offset >= n) {
                continue;
            }
            if (offset > 0) {
                for (auto& channel : block) {
                    channel.erase(channel.begin(), channel.begin() + static_cast<std::ptrdiff_t>(offset));
                }
            }
            const size_t count = static_cast<size_t>(std::min<uint64_t>(n - offset, remainingToMeter));
            meter.process(block, count);
            remainingToMeter -= count;
        }
    }
    return meter.finish();
}

AverageSpectrum scanSpectrum(const std::string& path, size_t fftSize) {
    WavReader reader(path);
    SpectrumAccumulator spectrum(reader.sampleRate(), reader.channels(), fftSize);
    AudioBlock block;
    size_t n;
    while ((n = reader.read(block, kBlockFrames)) > 0) {
        spectrum.push(block, n);
    }
    return spectrum.finish();
}

double rmsDifference(const std::vector<double>& a, const std::vector<double>& b, const std::vector<size_t>& range) {
    double sum = 0.0;
    for (size_t i : range) {
        const double d = a[i] - b[i];
        sum += d * d;
    }
    return std::sqrt(sum / static_cast<double>(std::max<size_t>(range.size(), 1)));
}

std::string escapeJson(const std::string& text) {
    std::ostringstream out;
    for (unsigned char c : text) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c) << std::dec << std::setfill(' ');
                } else {
                    out << c;
                }
        }
    }
    return out.str();
}

std::string num(double value, int precision) {
    if (!std::isfinite(value)) {
        return "null";
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}

std::string quote(const std::string& text) {
    return "\"" + escapeJson(text) + "\"";
}

std::string measurementsJson(const Measurements& m) {
    std::ostringstream out;
    out << "{\"integratedLufs\": " << num(m.integratedLufs, 3)
        << ", \"loudnessRangeLu\": " << num(m.loudnessRangeLu, 2)
        << ", \"momentaryMaxLufs\": " << num(m.momentaryMaxLufs, 2)
        << ", \"shortTermMaxLufs\": " << num(m.shortTermMaxLufs, 2)
        << ", \"truePeakDb\": " << num(m.truePeakDb, 3)
        << ", \"samplePeakDb\": " << num(m.samplePeakDb, 2)
        << ", \"plrDb\": " << num(m.plrDb(), 2)
        << ", \"dcOffset\": " << num(m.dcOffset, 5)
        << ", \"clippedSamples\": " << m.clippedSamples
        << ", \"stereoCorrelation\": " << (m.hasStereo ? num(m.correlation, 3) : "null")
        << ", \"lowCorrelation\": " << (m.hasStereo ? num(m.lowCorrelation, 3) : "null")
        << ", \"monoLossDb\": " << (m.hasStereo ? num(m.monoLossDb, 2) : "null")
        << ", \"sideToMidDb\": " << (m.hasStereo ? num(m.sideToMidDb, 2) : "null") << "}";
    return out.str();
}

std::string numberArray(const std::vector<double>& values, int precision) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        out << (i > 0 ? ", " : "") << num(values[i], precision);
    }
    out << "]";
    return out.str();
}

std::string floatArray(const std::vector<float>& values) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        out << (i > 0 ? "," : "") << num(static_cast<double>(values[i]), 4);
    }
    out << "]";
    return out.str();
}

std::string waveformJson(const Waveform& w) {
    return "{\"peak\": " + floatArray(w.peak) + ", \"rms\": " + floatArray(w.rms) + "}";
}

std::string curveJson(const CurveFit& curve) {
    std::ostringstream out;
    out << "{\"bands\": [";
    for (size_t i = 0; i < curve.bands.size(); ++i) {
        const EQBand& b = curve.bands[i];
        if (i > 0) {
            out << ", ";
        }
        out << "{\"id\": " << (i + 1) << ", \"frequency\": " << num(b.frequency, 1) << ", \"gain\": " << num(b.gainDb, 2)
            << ", \"q\": " << num(b.q, 3) << ", \"type\": " << quote(bandTypeName(b.type)) << "}";
    }
    out << "], \"residualRmsDb\": " << num(curve.residualRmsDb, 3) << ", \"curve\": {\"frequencies\": [";
    for (size_t i = 0; i < curve.frequenciesHz.size(); ++i) {
        out << (i > 0 ? ", " : "") << num(curve.frequenciesHz[i], 1);
    }
    out << "], \"targetDb\": [";
    for (size_t i = 0; i < curve.targetDb.size(); ++i) {
        out << (i > 0 ? ", " : "") << num(curve.targetDb[i], 2);
    }
    out << "], \"responseDb\": [";
    for (size_t i = 0; i < curve.responseDb.size(); ++i) {
        out << (i > 0 ? ", " : "") << num(curve.responseDb[i], 2);
    }
    out << "]}}";
    return out.str();
}

std::string apoLine(size_t index, const EQBand& band) {
    const char* type = band.type == BandType::LowShelf ? "LSC" : (band.type == BandType::HighShelf ? "HSC" : "PK");
    std::ostringstream out;
    out << std::fixed << "Filter " << index << ": ON " << type << " Fc " << std::setprecision(1) << band.frequency << " Hz Gain "
        << std::setprecision(2) << band.gainDb << " dB Q " << std::setprecision(3) << band.q;
    return out.str();
}

std::string reaLine(size_t index, const EQBand& band) {
    std::ostringstream out;
    out << std::fixed << "Band " << index << ": ";
    if (band.type == BandType::LowShelf) {
        out << "Low shelf";
    } else if (band.type == BandType::HighShelf) {
        out << "High shelf";
    } else {
        out << "Band";
    }
    out << ", " << std::setprecision(1) << band.frequency << " Hz, " << std::showpos << std::setprecision(2) << band.gainDb
        << std::noshowpos << " dB";
    if (band.type == BandType::Peak) {
        const double octaves = 2.0 / std::log(2.0) * std::asinh(1.0 / (2.0 * band.q));
        out << ", bandwidth " << std::setprecision(2) << octaves << " oct";
    }
    out << " (Q " << std::setprecision(3) << band.q << ")";
    return out.str();
}

struct QualityGrade {
    char grade = 'A';
    int score = 100;
    std::vector<std::string> reasons;
};

QualityGrade gradeResult(const MasteringResult& result, const MasteringConfig& config) {
    QualityGrade quality;
    auto penalise = [&](int points, const std::string& reason) {
        quality.score -= points;
        quality.reasons.push_back(reason);
    };
    if (config.autoGain && !result.targetReached) {
        penalise(25, "The loudness target was not reached");
    }
    const double average = result.limiter.averageReductionDb;
    const double maximum = result.limiter.maxReductionDb;
    if (average > 3.0) {
        penalise(25, "The limiter is working very hard (" + num(average, 1) + " dB on average)");
    } else if (average > 2.0) {
        penalise(15, "The limiter is working hard (" + num(average, 1) + " dB on average)");
    } else if (average > 1.0) {
        penalise(5, "The limiter is working moderately (" + num(average, 1) + " dB on average)");
    }
    if (maximum > 8.0) {
        penalise(10, "The limiter reaches " + num(maximum, 1) + " dB on the loudest hits");
    } else if (maximum > 5.0) {
        penalise(5, "The limiter reaches " + num(maximum, 1) + " dB on the loudest hits");
    }
    if (result.output.truePeakDb > config.ceilingDb + 0.05) {
        penalise(15, "The true peak is above the ceiling");
    }
    if (result.output.clippedSamples > 0) {
        penalise(10, "The output contains samples at full scale");
    }
    if (result.matchMeasured && result.matchAfterDb > result.matchBeforeDb + 0.05) {
        penalise(15, "The tone moved away from the target");
    } else if (result.matchMeasured && result.matchAfterDb > 2.0) {
        penalise(5, "The tone is still some way from the target");
    }
    if (result.output.hasStereo && result.output.monoLossDb < -4.0) {
        penalise(10, "The mix loses level when folded to mono");
    }
    if (result.output.hasStereo && result.output.correlation < 0.0) {
        penalise(10, "The stereo correlation is negative");
    }
    quality.score = std::max(quality.score, 0);
    quality.grade = quality.score >= 90 ? 'A' : (quality.score >= 75 ? 'B' : (quality.score >= 60 ? 'C' : 'D'));
    return quality;
}

}

MasteringEngine::MasteringEngine(const MasteringConfig& config) : config_(config) {
    validateConfig(config_);
}

void MasteringEngine::setConfig(const MasteringConfig& config) {
    validateConfig(config);
    config_ = config;
}

void MasteringEngine::emit(const std::string& message, double percent) const {
    if (progress_) {
        progress_(message, percent);
    }
}

Analysis MasteringEngine::analyze(const std::vector<std::string>& references, const std::string& input) {
    validateConfig(config_);
    const bool genreMode = references.empty();
    if (genreMode && config_.genre.empty()) {
        throw MasteredError("Provide at least one reference track or choose a genre target curve");
    }
    emit("Analysing input track", 0.0);
    const FileScan in = scanFile(input, config_.fftSize, config_.dualMono);
    std::vector<FileScan> refs;
    refs.reserve(references.size());
    for (size_t i = 0; i < references.size(); ++i) {
        emit("Analysing reference " + std::to_string(i + 1) + " of " + std::to_string(references.size()),
             30.0 * static_cast<double>(i + 1) / static_cast<double>(references.size() + 1));
        refs.push_back(scanFile(references[i], config_.fftSize, config_.dualMono));
    }

    Analysis analysis;
    analysis.sampleRate = in.sampleRate;
    analysis.channels = in.channels;
    analysis.frames = in.frames;
    analysis.sourceBitDepth = in.bitDepth;
    analysis.sourceFloat = in.isFloat;
    analysis.referenceCount = refs.size();
    analysis.input = in.measurements;

    const double count = static_cast<double>(refs.size());
    double minRate = in.sampleRate;
    Measurements averaged;
    averaged.integratedLufs = 0.0;
    averaged.truePeakDb = 0.0;
    averaged.correlation = 0.0;
    averaged.sideToMidDb = 0.0;
    averaged.lowCorrelation = 0.0;
    averaged.monoLossDb = 0.0;
    size_t stereoRefs = 0;
    for (const FileScan& ref : refs) {
        minRate = std::min(minRate, static_cast<double>(ref.sampleRate));
        averaged.integratedLufs += ref.measurements.integratedLufs / count;
        averaged.loudnessRangeLu += ref.measurements.loudnessRangeLu / count;
        averaged.truePeakDb += ref.measurements.truePeakDb / count;
        if (ref.measurements.hasStereo) {
            ++stereoRefs;
            averaged.correlation += ref.measurements.correlation;
            averaged.sideToMidDb += ref.measurements.sideToMidDb;
            averaged.monoLossDb += ref.measurements.monoLossDb;
            averaged.lowCorrelation += ref.measurements.lowCorrelation;
        }
    }
    if (stereoRefs > 0) {
        averaged.hasStereo = true;
        averaged.correlation /= static_cast<double>(stereoRefs);
        averaged.sideToMidDb /= static_cast<double>(stereoRefs);
        averaged.monoLossDb /= static_cast<double>(stereoRefs);
        averaged.lowCorrelation /= static_cast<double>(stereoRefs);
    } else {
        averaged.correlation = 1.0;
        averaged.lowCorrelation = 1.0;
        averaged.sideToMidDb = -120.0;
    }
    if (!genreMode) {
        analysis.reference = averaged;
    }

    const double maxHz = std::min(18000.0, 0.45 * minRate);
    const std::vector<double> centres = makeBandCentres(30.0, maxHz);
    const double width = config_.smoothing ? 1.0 / 3.0 : 1.0 / 6.0;
    std::vector<size_t> range;
    for (size_t i = 0; i < centres.size(); ++i) {
        if (centres[i] >= kMinLevelHz && centres[i] <= kMaxLevelHz) {
            range.push_back(i);
        }
    }
    if (range.empty()) {
        for (size_t i = 0; i < centres.size(); ++i) {
            range.push_back(i);
        }
    }

    analysis.shapeCentresHz = centres;
    analysis.shapeRange = range;
    analysis.shapeWidthOctaves = width;
    analysis.inputWaveform = in.waveform;
    analysis.inputShapeDb = shapeOf(bandLevelsDb(in.spectrum, false, centres, width), range);
    std::vector<const AverageSpectrum*> referenceSpectra;
    for (const FileScan& ref : refs) {
        referenceSpectra.push_back(&ref.spectrum);
    }
    if (genreMode) {
        analysis.referenceShapeDb = shapeOf(genreLevelsDb(config_.genre, centres), range);
        analysis.reference = Measurements();
    } else {
        analysis.referenceShapeDb = referenceShapeOf(referenceSpectra, false, centres, width, range);
    }
    const double fitAggressiveness = genreMode ? config_.aggressiveness * kGenreAggressivenessScale : config_.aggressiveness;
    const double fitMaxBoost = genreMode ? std::min(config_.maxBoostDb, kGenreMaxBoostDb) : config_.maxBoostDb;
    const double fitMaxCut = genreMode ? std::min(config_.maxCutDb, kGenreMaxCutDb) : config_.maxCutDb;
    if (genreMode) {
        analysis.notes.push_back(std::string("No reference track was used: the tone was matched to the built-in ") + findGenre(config_.genre)->name +
                                 " target curve, which is a general guide, so the correction is gentler than with a real reference");
    }

    double emptyAboveHz = 0.0;
    for (size_t i = centres.size(); i > 0; --i) {
        if (analysis.inputShapeDb[i - 1] >= kEmptyBandDb) {
            break;
        }
        emptyAboveHz = centres[i - 1];
    }

    emit("Fitting mid EQ", 32.0);
    const std::vector<double> midTarget = buildTarget(in.spectrum, analysis.referenceShapeDb, false, centres, width, range, fitAggressiveness, fitMaxCut, fitMaxBoost);
    analysis.mid = fitCurve(centres, midTarget, config_.maxEQBands, in.sampleRate, maxHz, config_.perceptualWeighting, emptyAboveHz);
    analysis.plan.midBands = analysis.mid.bands;

    const bool inputStereo = in.channels == 2 && in.measurements.sideToMidDb > kSideFloorDb;
    bool referencesStereo = !genreMode;
    for (const FileScan& ref : refs) {
        if (!ref.spectrum.hasSide || !ref.measurements.hasStereo || ref.measurements.sideToMidDb <= kSideFloorDb) {
            referencesStereo = false;
        }
    }
    if (inputStereo && referencesStereo) {
        if (config_.midSide && config_.maxSideBands > 0) {
            emit("Fitting side EQ", 34.0);
            const std::vector<double> sideTarget = buildTarget(in.spectrum, referenceShapeOf(referenceSpectra, true, centres, width, range), true, centres, width, range,
                                                               config_.aggressiveness, config_.maxCutDb, config_.maxBoostDb);
            analysis.side = fitCurve(centres, sideTarget, config_.maxSideBands, in.sampleRate, maxHz, config_.perceptualWeighting);
            analysis.plan.sideBands = analysis.side.bands;
        }
        if (config_.matchWidth) {
            const double difference = (averaged.sideToMidDb - in.measurements.sideToMidDb) * config_.aggressiveness;
            analysis.sideGainDb = std::min(std::max(difference, -kMaxWidthDb), kMaxWidthDb);
            analysis.plan.sideGain = std::pow(10.0, analysis.sideGainDb / 20.0);
        }
    }
    analysis.plan.midSide = in.channels == 2 && (!analysis.plan.sideBands.empty() || std::fabs(analysis.sideGainDb) >= 0.05);
    if (config_.monoBassHz > 0.0) {
        if (in.channels == 2) {
            analysis.plan.monoBassHz = config_.monoBassHz;
        } else {
            analysis.warnings.push_back("Mono bass only applies to stereo files and was skipped");
        }
    } else if (config_.autoMonoBass && in.channels == 2 && in.measurements.hasStereo && in.measurements.lowCorrelation < 0.3) {
        analysis.plan.monoBassHz = kAutoMonoBassHz;
        std::ostringstream note;
        note << "Mono bass was applied below " << static_cast<int>(kAutoMonoBassHz)
             << " Hz because the low end was partly out of phase between left and right";
        analysis.notes.push_back(note.str());
    }

    if (config_.dynamics && genreMode) {
        analysis.warnings.push_back("Dynamics matching needs a reference track and was skipped");
    } else if (config_.dynamics) {
        DynamicsSettings dynamics;
        dynamics.enabled = true;
        const std::vector<double> crossovers = chooseCrossovers(config_.dynamicsBands, in.spectrum, in.sampleRate, config_.adaptiveCrossovers, centres, width);
        dynamics.bandCount = crossovers.size() + 1;
        for (size_t k = 0; k < crossovers.size(); ++k) {
            dynamics.crossoversHz[k] = crossovers[k];
        }
        analysis.dynamicsCrossoversHz = crossovers;
        const size_t bandCount = dynamics.bandCount;
        emit("Measuring band levels", 31.0);
        const std::vector<BandStatistics> inputBands = measureBands(input, crossovers);
        std::vector<std::vector<BandStatistics>> referenceBands;
        for (const std::string& path : references) {
            referenceBands.push_back(measureBands(path, crossovers));
        }
        static const std::array<std::array<double, 5>, 4> kAttack = {{{12.0, 3.0, 0.0, 0.0, 0.0},
                                                                      {15.0, 8.0, 3.0, 0.0, 0.0},
                                                                      {15.0, 10.0, 5.0, 3.0, 0.0},
                                                                      {15.0, 10.0, 8.0, 4.0, 2.5}}};
        static const std::array<std::array<double, 5>, 4> kRelease = {{{110.0, 60.0, 0.0, 0.0, 0.0},
                                                                       {120.0, 90.0, 60.0, 0.0, 0.0},
                                                                       {120.0, 100.0, 80.0, 60.0, 0.0},
                                                                       {120.0, 100.0, 90.0, 70.0, 50.0}}};
        const std::array<double, 5>& attack = kAttack[bandCount - 2];
        const std::array<double, 5>& release = kRelease[bandCount - 2];
        analysis.inputSpreadDb.assign(bandCount, 0.0);
        analysis.referenceSpreadDb.assign(bandCount, 0.0);
        bool active = false;
        for (size_t b = 0; b < bandCount; ++b) {
            const double inputSpread = inputBands[b].spreadDb();
            double referenceSpread = 0.0;
            for (const std::vector<BandStatistics>& ref : referenceBands) {
                referenceSpread += ref[b].spreadDb() / count;
            }
            analysis.inputSpreadDb[b] = inputSpread;
            analysis.referenceSpreadDb[b] = referenceSpread;
            double ratio = config_.dynamicsRatio;
            if (config_.dynamicsMatch) {
                const double reduction = std::max(0.0, inputSpread - referenceSpread) * config_.aggressiveness;
                ratio = 1.0;
                if (reduction >= 0.5 && inputSpread > 0.5) {
                    const double remaining = std::max(inputSpread - reduction, 0.25 * inputSpread);
                    ratio = std::min(inputSpread / remaining, 6.0);
                }
            }
            dynamics.bands[b].thresholdDb = inputBands[b].p50Db + config_.dynamicsThresholdOffsetDb;
            dynamics.bands[b].ratio = ratio;
            dynamics.bands[b].kneeDb = 6.0;
            dynamics.bands[b].attackMs = attack[b];
            dynamics.bands[b].releaseMs = release[b];
            if (ratio >= 1.05) {
                active = true;
            }
        }
        if (active && config_.dynamicsMatch) {
            for (int round = 0; round < 3; ++round) {
                emit("Refining dynamics", 33.0 + static_cast<double>(round));
                const std::vector<BandStatistics> achieved = measureDynamics(input, dynamics);
                bool changed = false;
                for (size_t b = 0; b < bandCount; ++b) {
                    const double inputSpread = analysis.inputSpreadDb[b];
                    const double ratio = dynamics.bands[b].ratio;
                    if (ratio < 1.05 || inputSpread <= 0.5) {
                        continue;
                    }
                    const double reduction = std::max(0.0, inputSpread - analysis.referenceSpreadDb[b]) * config_.aggressiveness;
                    const double desired = std::max(inputSpread - reduction, 0.25 * inputSpread);
                    const double actual = achieved[b].spreadDb();
                    if (std::fabs(actual - desired) < 0.3) {
                        continue;
                    }
                    const double portion = (inputSpread - actual) / (inputSpread * (1.0 - 1.0 / ratio));
                    const double effectiveness = std::min(std::max(portion, 0.05), 2.0);
                    const double required = ((inputSpread - desired) / inputSpread) / effectiveness;
                    const double next = required >= 0.85 ? 6.0 : std::min(std::max(1.0 / (1.0 - required), 1.0), 6.0);
                    if (std::fabs(next - ratio) > 0.02) {
                        dynamics.bands[b].ratio = next;
                        changed = true;
                    }
                }
                if (!changed) {
                    break;
                }
            }
            active = false;
            for (size_t b = 0; b < bandCount; ++b) {
                if (dynamics.bands[b].ratio >= 1.05) {
                    active = true;
                }
            }
        }
        if (!active) {
            dynamics.enabled = false;
            analysis.warnings.push_back("Dynamics already match the reference; compression skipped");
        }
        analysis.plan.dynamics = dynamics;
    }

    if (config_.deess) {
        double lowHz = 0.0;
        double highHz = 0.0;
        if (!deEsserBand(in.sampleRate, lowHz, highHz)) {
            analysis.warnings.push_back("The sample rate is too low for the de-esser, so it was skipped");
        } else {
            emit("Measuring sibilance", 36.0);
            const BandStatistics sibilance = measureSibilance(input, lowHz, highHz);
            const double spread = sibilance.p95Db - sibilance.p50Db;
            if (spread >= kDeEsserMinSpreadDb) {
                DeEsserSettings deEsser;
                deEsser.enabled = true;
                deEsser.lowHz = lowHz;
                deEsser.highHz = highHz;
                deEsser.thresholdDb = sibilance.p50Db + 0.5 * spread;
                analysis.plan.deEsser = deEsser;
                analysis.notes.push_back("The de-esser is on: the " + num(lowHz, 0) + " to " + num(highHz, 0) + " Hz band is reduced when it jumps well above its usual level");
            } else {
                analysis.notes.push_back("No harsh sibilance was found, so the de-esser was left off");
            }
        }
    }

    if (config_.resonance) {
        const double limitHz = std::min(12000.0, static_cast<double>(in.sampleRate) * 0.3);
        const std::vector<double> centres = makeBandCentres(kResonanceMinHz, limitHz);
        if (centres.size() >= 9) {
            const std::vector<double> levels = bandLevelsDb(in.spectrum, false, centres, 1.0 / 6.0);
            struct Candidate {
                size_t index;
                double excess;
            };
            std::vector<Candidate> found;
            for (size_t i = 3; i + 3 < centres.size(); ++i) {
                double neighbours = 0.0;
                size_t count = 0;
                for (int d = -3; d <= 3; ++d) {
                    if (d >= -1 && d <= 1) {
                        continue;
                    }
                    neighbours += levels[static_cast<size_t>(static_cast<int>(i) + d)];
                    ++count;
                }
                const double excess = levels[i] - neighbours / static_cast<double>(count);
                if (excess >= kResonanceMinExcessDb && levels[i] >= levels[i - 1] && levels[i] >= levels[i + 1]) {
                    found.push_back({i, excess});
                }
            }
            std::sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) { return a.excess > b.excess; });
            ResonanceSettings resonance;
            std::vector<size_t> chosen;
            for (const Candidate& candidate : found) {
                if (resonance.count >= kMaxResonanceBands) {
                    break;
                }
                bool close = false;
                for (size_t other : chosen) {
                    const size_t gap = other > candidate.index ? other - candidate.index : candidate.index - other;
                    close = close || gap < 3;
                }
                if (close) {
                    continue;
                }
                chosen.push_back(candidate.index);
                ResonanceBand band;
                band.frequency = centres[candidate.index];
                band.q = 6.0;
                band.maxCutDb = std::min(std::max(0.6 * candidate.excess, 1.5), 4.0);
                band.thresholdDb = 2.0;
                resonance.bands[resonance.count++] = band;
            }
            if (resonance.count > 0) {
                resonance.enabled = true;
                analysis.plan.resonance = resonance;
                std::string list;
                for (size_t b = 0; b < resonance.count; ++b) {
                    list += (b > 0 ? ", " : "") + num(resonance.bands[b].frequency, 0) + " Hz";
                }
                analysis.notes.push_back("The resonance tamer is on at " + list + ": each band is reduced only while it sticks out above its neighbours");
            } else {
                analysis.notes.push_back("No lasting resonances were found, so the resonance tamer was left off");
            }
        }
    }

    analysis.plan.softClip = config_.softClip;
    analysis.plan.softClipKneeDb = config_.softClipKneeDb;
    analysis.plan.limiterEnabled = true;
    analysis.plan.ceilingDb = config_.ceilingDb;
    analysis.plan.lookaheadMs = config_.limiterLookaheadMs;
    analysis.plan.releaseMs = config_.limiterReleaseMs;
    analysis.plan.bassLimiter = config_.bassLimiter;
    analysis.plan.strictPeak = config_.strictPeak;
    analysis.plan.linearPhase = config_.linearPhase;
    if (config_.subsonicHz > 0.0) {
        analysis.plan.subsonicHz = config_.subsonicHz;
    } else if (config_.autoSubsonic && in.measurements.dcOffset > 0.005) {
        analysis.plan.subsonicHz = kAutoSubsonicHz;
        analysis.notes.push_back("A 20 Hz high-pass filter was applied because the input has a DC offset");
    }

    if (in.nonFinite > 0) {
        analysis.warnings.push_back("The input contains " + std::to_string(in.nonFinite) + " invalid samples (not-a-number or infinite values); they were replaced with silence");
    }
    if (in.measurements.truePeakDb > 0.0) {
        analysis.warnings.push_back("Input contains true peaks above 0 dBTP");
    }
    if (in.measurements.clippedSamples > 0) {
        analysis.warnings.push_back("Input has " + std::to_string(in.measurements.clippedSamples) + " samples at full scale; it may already be clipped");
    }
    if (in.measurements.dcOffset > 0.005 && analysis.plan.subsonicHz <= 0.0) {
        analysis.warnings.push_back("Input has a DC offset; it wastes headroom and can be removed with a high-pass filter");
    }
    if (in.measurements.hasStereo && in.measurements.lowCorrelation < 0.3 && analysis.plan.monoBassHz <= 0.0) {
        analysis.warnings.push_back("Low frequencies are partly out of phase between left and right; mono bass is recommended");
    }
    if (in.measurements.integratedLufs <= -100.0) {
        analysis.warnings.push_back("Input is too quiet to measure loudness");
    }
    if (in.measurements.integratedLufs > -100.0 && in.measurements.integratedLufs > -10.0 && in.measurements.plrDb() < 7.0) {
        analysis.warnings.push_back("The input is already loud and heavily limited; further limiting will cost punch, so a lower loudness target is advisable");
    }
    for (size_t i = 0; i < refs.size(); ++i) {
        const FileScan& ref = refs[i];
        const std::string label = refs.size() > 1 ? "Reference " + std::to_string(i + 1) : "The reference";
        if (ref.measurements.integratedLufs <= -100.0) {
            analysis.warnings.push_back(label + " is too quiet to measure; its tone is a poor guide");
        } else if (ref.measurements.integratedLufs < -30.0) {
            analysis.warnings.push_back(label + " is very quiet (" + num(ref.measurements.integratedLufs, 1) + " LUFS); the tonal match may be unreliable");
        }
        if (ref.measurements.integratedLufs > -100.0 && ref.measurements.plrDb() < 6.0) {
            analysis.warnings.push_back(label + " is heavily compressed (peak to loudness ratio " + num(ref.measurements.plrDb(), 1) + " dB); matching it with dynamics on can flatten the track");
        }
        if (ref.sampleRate != in.sampleRate) {
            analysis.notes.push_back(label + " has a different sample rate (" + std::to_string(ref.sampleRate) + " Hz against " + std::to_string(in.sampleRate) +
                                     " Hz); tone is compared only up to the lower of the two rates");
        }
        if (ref.channels != in.channels && (ref.channels == 1 || in.channels == 1)) {
            analysis.notes.push_back(label + (ref.channels == 1 ? " is mono" : " is stereo") + " while the input is " + (in.channels == 1 ? "mono" : "stereo") +
                                     "; stereo width matching was skipped");
        }
    }
    if (in.channels > 2) {
        analysis.notes.push_back("The input has " + std::to_string(in.channels) + " channels; they are processed together and no stereo width matching is done");
    }
    emit("Analysis complete", 35.0);
    return analysis;
}

MasteringResult MasteringEngine::render(const Analysis& analysis, const std::string& input, const std::string& output) {
    validateConfig(config_);
    {
        WavReader probe(input);
        if (probe.sampleRate() != analysis.sampleRate || probe.channels() != analysis.channels || probe.frames() != analysis.frames) {
            throw MasteredError("Input file does not match the analysis it is being rendered with");
        }
    }
    MasteringResult result;
    result.analysis = analysis;
    result.inputPath = input;
    result.outputPath = output;
    result.warnings = analysis.warnings;
    result.notes = analysis.notes;

    ChainPlan base = analysis.plan;
    double targetLufs = config_.targetLufs;
    if (config_.autoGain && config_.punchGuardPlrDb > 0.0) {
        const double guarded = config_.ceilingDb - config_.punchGuardPlrDb;
        if (targetLufs > guarded) {
            targetLufs = guarded;
            std::ostringstream note;
            note << std::fixed << std::setprecision(1) << "The punch guard lowered the loudness target from " << config_.targetLufs << " to " << guarded
                 << " LUFS so the track keeps at least " << config_.punchGuardPlrDb << " dB between its peaks and its average level";
            result.notes.push_back(note.str());
        }
    }
    result.effectiveTargetLufs = targetLufs;
    {
        const double inputPlr = analysis.input.integratedLufs > -100.0 ? analysis.input.plrDb() : 11.0;
        const double preserved = std::min(std::max(0.75 * inputPlr, 7.0), 12.0);
        result.safeMaxLufs = config_.ceilingDb - preserved;
    }
    if (config_.glue) {
        base.glue.enabled = true;
        base.glue.thresholdDb = config_.ceilingDb - kGlueThresholdBelowCeilingDb;
        result.notes.push_back("A gentle glue compressor runs before the limiter for extra cohesion and density");
    }
    const double peakTarget = config_.ceilingDb - kPeakSafetyDb;
    double trimDb = 0.0;
    double gain = 0.0;
    double startGain = 0.0;
    int iterations = 0;
    double percent = 36.0;

    auto advance = [&](const std::string& message, double value) {
        percent = std::max(percent, std::min(value, 99.0));
        emit(message, percent);
    };

    advance("Preparing the tone and dynamics", 37.0);
    const TempFile cache(output + ".stage.tmp");
    const Measurements preMeasure = runPre(input, base, cache.path(), config_.dualMono);

    if (config_.autoGain) {
        advance("Measuring loudness", 40.0);
        if (preMeasure.integratedLufs <= -100.0) {
            throw MasteredError("Audio is too quiet to measure (below -70 LUFS), so it cannot be normalised to a loudness target; raise its level first");
        }
        startGain = std::min(std::max(targetLufs - preMeasure.integratedLufs, -kMaxMakeupDb), kMaxMakeupDb);
        gain = startGain;
    }

    const double neededReductionDb = preMeasure.truePeakDb + gain - config_.ceilingDb;
    if (config_.autoGain && config_.autoClip && !base.softClip && neededReductionDb > kAutoClipReductionDb) {
        base.softClip = true;
        std::ostringstream note;
        note << std::fixed << std::setprecision(1) << "The soft clipper was switched on because the peaks are about " << neededReductionDb
             << " dB above the ceiling at this loudness; it takes the tallest peaks so the limiter works less";
        result.notes.push_back(note.str());
    }
    result.analysis.plan.softClip = base.softClip;

    auto planFor = [&](double gainDb) {
        ChainPlan plan = base;
        plan.gainDb = gainDb;
        plan.ceilingDb = base.ceilingDb - trimDb;
        return plan;
    };
    std::vector<Segment> segments;
    if (config_.autoGain && static_cast<double>(analysis.frames) >= kCoarseMinSeconds * static_cast<double>(analysis.sampleRate)) {
        const uint64_t length = static_cast<uint64_t>(kCoarseSegmentSeconds * static_cast<double>(analysis.sampleRate));
        for (size_t k = 0; k < kCoarseSegments; ++k) {
            const uint64_t centre = static_cast<uint64_t>((static_cast<double>(k) + 0.5) * static_cast<double>(analysis.frames) / static_cast<double>(kCoarseSegments));
            segments.push_back({centre > length / 2 ? centre - length / 2 : 0, length});
        }
    }
    double slopeEstimate = 1.0;
    bool saturated = false;
    double saturationBestLufs = 0.0;
    auto converge = [&](double initialGain) {
        struct Probe {
            double gain;
            double lufs;
        };
        std::vector<Probe> probes;
        auto probe = [&](double gainDb) {
            advance("Matching loudness (pass " + std::to_string(iterations + 1) + ")", percent + 1.5);
            const Measurements m = runPost(cache.path(), analysis, planFor(gainDb), nullptr, PeakMode::Off, config_.dualMono);
            ++iterations;
            probes.push_back({gainDb, m.integratedLufs});
            return m.integratedLufs;
        };
        double current = initialGain;
        int stalls = 0;
        saturated = false;
        bool havePending = false;
        double pendingGain = 0.0;
        double pendingLufs = 0.0;
        if (!segments.empty()) {
            const double fullStart = probe(initialGain);
            auto excerptLufs = [&](double gainDb) {
                advance("Estimating loudness", percent + 0.5);
                return runPostExcerpts(cache.path(), analysis, planFor(gainDb), segments, config_.dualMono).integratedLufs;
            };
            const double excerptStart = excerptLufs(initialGain);
            if (fullStart > -100.0 && excerptStart > -100.0) {
                havePending = true;
                pendingGain = initialGain;
                pendingLufs = fullStart;
                const double offset = fullStart - excerptStart;
                double gainDb = initialGain;
                double level = excerptStart;
                double coarseSlope = slopeEstimate;
                for (int k = 0; k < 5; ++k) {
                    const double error = targetLufs - (level + offset);
                    if (std::fabs(error) <= config_.loudnessToleranceLu * 0.5) {
                        break;
                    }
                    const double next = std::min(std::max(gainDb + error / coarseSlope, -kMaxMakeupDb), kMaxMakeupDb);
                    if (std::fabs(next - gainDb) < 1e-4) {
                        break;
                    }
                    const double nextLevel = excerptLufs(next);
                    if (nextLevel <= -100.0) {
                        break;
                    }
                    const double measured = (nextLevel - level) / (next - gainDb);
                    if (measured < 0.35) {
                        break;
                    }
                    coarseSlope = std::min(std::max(measured, 0.25), 1.0);
                    gainDb = next;
                    level = nextLevel;
                }
                current = gainDb;
            }
        }
        for (int i = 1; i <= config_.maxLoudnessIterations; ++i) {
            double lufs;
            if (havePending && std::fabs(current - pendingGain) < 1e-9) {
                lufs = pendingLufs;
            } else {
                lufs = probe(current);
            }
            const double error = targetLufs - lufs;
            if (std::fabs(error) <= config_.loudnessToleranceLu) {
                return current;
            }
            double slope = slopeEstimate;
            if (probes.size() > 1) {
                const Probe& a = probes[probes.size() - 2];
                const Probe& b = probes.back();
                if (std::fabs(b.gain - a.gain) > 1e-3) {
                    const double measured = (b.lufs - a.lufs) / (b.gain - a.gain);
                    stalls = measured < kSaturationSlope ? stalls + 1 : 0;
                    slope = std::min(std::max(measured, 0.25), 1.0);
                    slopeEstimate = slope;
                }
            }
            if (stalls >= 2) {
                saturated = true;
                break;
            }
            const double next = std::min(std::max(current + error / slope, -kMaxMakeupDb), kMaxMakeupDb);
            if (std::fabs(next - current) < 1e-4) {
                break;
            }
            current = next;
        }
        size_t best = 0;
        double highest = probes[0].lufs;
        for (size_t k = 1; k < probes.size(); ++k) {
            if (std::fabs(targetLufs - probes[k].lufs) < std::fabs(targetLufs - probes[best].lufs)) {
                best = k;
            }
            highest = std::max(highest, probes[k].lufs);
        }
        if (!saturated) {
            return probes[best].gain;
        }
        saturationBestLufs = highest;
        const double threshold = highest - kKneeToleranceLu;
        double high = probes[best].gain;
        for (const Probe& p : probes) {
            if (p.lufs >= threshold && p.gain < high) {
                high = p.gain;
            }
        }
        double low = high - 6.0;
        for (const Probe& p : probes) {
            if (p.lufs < threshold && p.gain < high && p.gain > low) {
                low = p.gain;
            }
        }
        for (int k = 0; k < kMaxKneeRefinements; ++k) {
            const double middle = 0.5 * (low + high);
            if (probe(middle) >= threshold) {
                high = middle;
            } else {
                low = middle;
            }
        }
        return high;
    };

    uint16_t depth = config_.bitDepth;
    if (depth == 0) {
        depth = analysis.sourceFloat ? 32 : analysis.sourceBitDepth;
    }
    if (depth > 32) {
        depth = 32;
    }

    Measurements delivered;
    int raises = 0;
    for (int attempt = 0;; ++attempt) {
        if (config_.autoGain) {
            gain = converge(attempt == 0 ? startGain : gain);
        }
        advance(attempt == 0 ? "Writing mastered audio" : "Adjusting the peak ceiling", percent + 2.0);
        LimiterStats limiterStats;
        WaveformBuilder waveform(analysis.frames, kWaveformColumns);
        {
            WavWriter writer(output, analysis.sampleRate, analysis.channels, depth, config_.dither, config_.noiseShaping);
            runPost(cache.path(), analysis, planFor(gain), &writer, PeakMode::Off, config_.dualMono, &waveform, &limiterStats);
            if (writer.framesWritten() != analysis.frames) {
                throw MasteredError("The mastered audio length does not match the input length");
            }
            result.framesWritten = writer.framesWritten();
            writer.finalize();
        }
        delivered = MasteringEngine::measureFile(output, config_.dualMono, config_.strictPeak);
        const double over = delivered.truePeakDb - peakTarget;
        bool again = false;
        if (config_.accuratePeak && attempt < kMaxPeakTrims) {
            if (over > 0.005) {
                trimDb += over + 0.01;
                again = true;
            } else if (over < -kPeakWindowDb && limiterStats.maxReductionDb > 0.05 && raises < kMaxCeilingRaises &&
                       trimDb + over >= -kMaxCeilingRaiseDb) {
                trimDb += over + 0.01;
                ++raises;
                again = true;
            }
        }
        if (!again) {
            result.output = delivered;
            result.outputWaveform = waveform.finish();
            result.limiter = limiterStats;
            break;
        }
    }
    result.ceilingTrimDb = trimDb;

    ChainPlan finalPlan = planFor(gain);
    result.analysis.plan = finalPlan;
    result.outputBitDepth = depth;
    result.makeupGainDb = gain;
    result.iterations = iterations;
    if (config_.autoGain) {
        const double finalError = targetLufs - result.output.integratedLufs;
        result.targetReached = std::fabs(finalError) <= std::max(config_.loudnessToleranceLu, 0.05) * 1.5;
        if (!result.targetReached) {
            std::ostringstream message;
            if (saturated) {
                message << "Loudness target not reached: this track cannot get much louder than " << std::fixed << std::setprecision(2) << saturationBestLufs
                        << " LUFS at a ceiling of " << config_.ceilingDb << " dBTP without crushing its dynamics, so the engine stopped at " << result.output.integratedLufs
                        << " LUFS instead of " << targetLufs << " LUFS; choose a lower target or a higher ceiling";
            } else {
                message << "Loudness target not reached: measured " << std::fixed << std::setprecision(2) << result.output.integratedLufs
                        << " LUFS against a target of " << targetLufs << " LUFS; the true-peak ceiling limits the achievable loudness";
            }
            result.warnings.push_back(message.str());
        }
    }
    if (result.output.truePeakDb > config_.ceilingDb + 0.02) {
        std::ostringstream message;
        message << "True peak of " << std::fixed << std::setprecision(2) << result.output.truePeakDb
                << " dBTP is above the ceiling of " << config_.ceilingDb << " dBTP";
        result.warnings.push_back(message.str());
    }
    if (config_.autoGain && (result.limiter.averageReductionDb > kHeavyAverageReductionDb || result.limiter.maxReductionDb > kHeavyMaxReductionDb)) {
        std::ostringstream message;
        message << "Heavy limiting: " << std::fixed << std::setprecision(1) << result.limiter.averageReductionDb << " dB on average and up to "
                << result.limiter.maxReductionDb << " dB. At this loudness drums can lose punch; a lower loudness target or a higher ceiling keeps the beat more open";
        result.warnings.push_back(message.str());
    }
    if (!analysis.shapeCentresHz.empty()) {
        try {
            advance("Checking the match to the reference", 99.0);
            const AverageSpectrum spectrum = scanSpectrum(output, config_.fftSize);
            result.outputShapeDb = shapeOf(bandLevelsDb(spectrum, false, analysis.shapeCentresHz, analysis.shapeWidthOctaves), analysis.shapeRange);
            result.matchBeforeDb = rmsDifference(analysis.referenceShapeDb, analysis.inputShapeDb, analysis.shapeRange);
            result.matchAfterDb = rmsDifference(analysis.referenceShapeDb, result.outputShapeDb, analysis.shapeRange);
            result.matchMeasured = true;
        } catch (const MasteredError&) {
            result.outputShapeDb.clear();
        }
    }
    if (result.output.hasStereo && result.output.monoLossDb < -4.0) {
        std::ostringstream message;
        message << "Folding to mono lowers the level by " << std::fixed << std::setprecision(1) << -result.output.monoLossDb << " dB";
        result.warnings.push_back(message.str());
    }
    if (result.output.hasStereo && result.output.correlation < 0.0) {
        result.warnings.push_back("Output stereo correlation is negative; check mono compatibility");
    }
    emit("Done", 100.0);
    return result;
}

Measurements MasteringEngine::measureFile(const std::string& path, bool dualMono, bool strictPeak) {
    WavReader reader(path);
    Meter meter(reader.sampleRate(), reader.channels(), strictPeak ? PeakMode::Accurate : PeakMode::Standard, dualMono);
    AudioBlock block;
    size_t n;
    while ((n = reader.read(block, kBlockFrames)) > 0) {
        meter.process(block, n);
    }
    return meter.finish();
}

std::string MasteringEngine::measurementsJsonText(const Measurements& measurements) {
    return measurementsJson(measurements);
}

MasteringResult MasteringEngine::run(const std::vector<std::string>& references, const std::string& input, const std::string& output) {
    const Analysis analysis = analyze(references, input);
    return render(analysis, input, output);
}

std::string MasteringEngine::exportJSON(const MasteringResult& result, const MasteringConfig& config) {
    const Analysis& a = result.analysis;
    std::ostringstream out;
    out << "{\n";
    out << "  \"version\": " << quote(kVersion) << ",\n";
    out << "  \"settings\": {\"targetLufs\": " << num(config.targetLufs, 2) << ", \"ceilingDb\": " << num(config.ceilingDb, 2)
        << ", \"toleranceLu\": " << num(config.loudnessToleranceLu, 3) << ", \"dualMono\": " << (config.dualMono ? "true" : "false")
        << ", \"lookaheadMs\": " << num(config.limiterLookaheadMs, 1) << ", \"releaseMs\": " << num(config.limiterReleaseMs, 0)
        << ", \"strictPeak\": " << (config.strictPeak ? "true" : "false") << ", \"linearPhase\": " << (config.linearPhase ? "true" : "false") << ", \"deess\": " << (config.deess ? "true" : "false") << ", \"genre\": " << quote(config.genre)
        << ", \"effectiveTargetLufs\": " << num(result.effectiveTargetLufs != 0.0 ? result.effectiveTargetLufs : config.targetLufs, 2)
        << ", \"punchGuardPlrDb\": " << num(config.punchGuardPlrDb, 1) << ", \"glue\": " << (config.glue ? "true" : "false")
        << ", \"resonance\": " << (config.resonance ? "true" : "false") << "},\n";
    out << "  \"input\": {\"path\": " << quote(result.inputPath) << ", \"sampleRate\": " << a.sampleRate << ", \"channels\": " << a.channels
        << ", \"frames\": " << a.frames << ", \"measurements\": " << measurementsJson(a.input) << "},\n";
    out << "  \"references\": {\"count\": " << a.referenceCount << ", \"measurements\": " << measurementsJson(a.reference) << "},\n";
    out << "  \"output\": {\"path\": " << quote(result.outputPath) << ", \"bitDepth\": " << result.outputBitDepth
        << ", \"measurements\": " << measurementsJson(result.output) << "},\n";
    out << "  \"gain\": {\"makeupDb\": " << num(result.makeupGainDb, 2) << ", \"iterations\": " << result.iterations
        << ", \"targetReached\": " << (result.targetReached ? "true" : "false") << ", \"ceilingTrimDb\": " << num(result.ceilingTrimDb, 3)
        << ", \"safeMaxLufs\": " << num(result.safeMaxLufs, 1) << "},\n";
    {
        const QualityGrade quality = gradeResult(result, config);
        out << "  \"quality\": {\"grade\": " << quote(std::string(1, quality.grade)) << ", \"score\": " << quality.score << ", \"reasons\": [";
        for (size_t i = 0; i < quality.reasons.size(); ++i) {
            out << (i > 0 ? ", " : "") << quote(quality.reasons[i]);
        }
        out << "]},\n";
    }
    auto extreme = [](const std::vector<EQBand>& bands, bool boost) {
        double value = 0.0;
        for (const EQBand& band : bands) {
            value = boost ? std::max(value, band.gainDb) : std::min(value, band.gainDb);
        }
        return value;
    };
    out << "  \"changes\": {\"loudnessChangeLu\": " << num(result.output.integratedLufs - a.input.integratedLufs, 2)
        << ", \"truePeakChangeDb\": " << num(result.output.truePeakDb - a.input.truePeakDb, 2)
        << ", \"makeupGainDb\": " << num(result.makeupGainDb, 2)
        << ", \"midBoostDb\": " << num(extreme(a.mid.bands, true), 2) << ", \"midCutDb\": " << num(extreme(a.mid.bands, false), 2)
        << ", \"sideBoostDb\": " << num(extreme(a.side.bands, true), 2) << ", \"sideCutDb\": " << num(extreme(a.side.bands, false), 2)
        << ", \"widthChangeDb\": " << num(a.sideGainDb, 2)
        << ", \"limiterMaxReductionDb\": " << num(result.limiter.maxReductionDb, 2)
        << ", \"limiterAverageReductionDb\": " << num(result.limiter.averageReductionDb, 2)
        << ", \"dynamicsApplied\": " << (a.plan.dynamics.enabled ? "true" : "false")
        << ", \"softClipApplied\": " << (a.plan.softClip ? "true" : "false")
        << ", \"monoBassHz\": " << num(a.plan.monoBassHz, 0)
        << ", \"bands\": " << (a.mid.bands.size() + a.side.bands.size()) << "},\n";
    out << "  \"mid\": " << curveJson(a.mid) << ",\n";
    out << "  \"side\": " << curveJson(a.side) << ",\n";
    out << "  \"deEsser\": {\"enabled\": " << (a.plan.deEsser.enabled ? "true" : "false") << ", \"thresholdDb\": " << num(a.plan.deEsser.thresholdDb, 2)
        << ", \"ratio\": " << num(a.plan.deEsser.ratio, 1) << ", \"lowHz\": " << num(a.plan.deEsser.lowHz, 0) << ", \"highHz\": " << num(a.plan.deEsser.highHz, 0) << "},\n";
    out << "  \"width\": {\"sideGainDb\": " << num(a.sideGainDb, 2) << ", \"midSide\": " << (a.plan.midSide ? "true" : "false") << "},\n";
    out << "  \"dynamics\": {\"enabled\": " << (a.plan.dynamics.enabled ? "true" : "false") << ", \"bands\": [";
    for (size_t i = 0; i < a.plan.dynamics.bandCount; ++i) {
        const CompressorBand& b = a.plan.dynamics.bands[i];
        out << (i > 0 ? ", " : "") << "{\"thresholdDb\": " << num(b.thresholdDb, 2) << ", \"ratio\": " << num(b.ratio, 2)
            << ", \"kneeDb\": " << num(b.kneeDb, 1) << ", \"attackMs\": " << num(b.attackMs, 1) << ", \"releaseMs\": " << num(b.releaseMs, 1) << "}";
    }
    out << "], \"spreadDb\": {\"input\": [";
    for (size_t i = 0; i < a.inputSpreadDb.size(); ++i) {
        out << (i > 0 ? ", " : "") << num(a.inputSpreadDb[i], 2);
    }
    out << "], \"reference\": [";
    for (size_t i = 0; i < a.referenceSpreadDb.size(); ++i) {
        out << (i > 0 ? ", " : "") << num(a.referenceSpreadDb[i], 2);
    }
    out << "]}},\n";
    out << "  \"limiter\": {\"maxReductionDb\": " << num(result.limiter.maxReductionDb, 2)
        << ", \"averageReductionDb\": " << num(result.limiter.averageReductionDb, 3)
        << ", \"reducedFraction\": " << num(result.limiter.reducedFraction, 4)
        << ", \"softClip\": " << (a.plan.softClip ? "true" : "false") << ", \"bassLimiter\": " << (a.plan.bassLimiter ? "true" : "false") << "},\n";
    out << "  \"monoBassHz\": " << num(a.plan.monoBassHz, 1) << ",\n";
    out << "  \"spectrum\": {\"frequencies\": " << numberArray(a.shapeCentresHz, 1) << ", \"input\": " << numberArray(a.inputShapeDb, 2)
        << ", \"reference\": " << numberArray(a.referenceShapeDb, 2) << ", \"output\": " << numberArray(result.outputShapeDb, 2)
        << ", \"match\": {\"measured\": " << (result.matchMeasured ? "true" : "false") << ", \"beforeDb\": " << num(result.matchBeforeDb, 2)
        << ", \"afterDb\": " << num(result.matchAfterDb, 2) << "}},\n";
    out << "  \"waveform\": {\"columns\": " << a.inputWaveform.peak.size() << ", \"input\": " << waveformJson(a.inputWaveform)
        << ", \"output\": " << waveformJson(result.outputWaveform) << "},\n";
    {
        auto toDoubles = [](const std::vector<float>& values) {
            return std::vector<double>(values.begin(), values.end());
        };
        out << "  \"timeline\": {\"stepSec\": " << num(a.input.timelineStepSec, 3) << ", \"input\": " << numberArray(toDoubles(a.input.timeline), 1)
            << ", \"output\": " << numberArray(toDoubles(result.output.timeline), 1) << "},\n";
        out << "  \"resonance\": {\"enabled\": " << (a.plan.resonance.enabled ? "true" : "false") << ", \"bands\": [";
        for (size_t i = 0; i < a.plan.resonance.count; ++i) {
            const ResonanceBand& b = a.plan.resonance.bands[i];
            out << (i > 0 ? ", " : "") << "{\"frequency\": " << num(b.frequency, 0) << ", \"maxCutDb\": " << num(b.maxCutDb, 1) << "}";
        }
        out << "]},\n";
        out << "  \"glue\": {\"enabled\": " << (a.plan.glue.enabled ? "true" : "false") << ", \"thresholdDb\": " << num(a.plan.glue.thresholdDb, 1)
            << ", \"ratio\": " << num(a.plan.glue.ratio, 2) << "},\n";
    }
    out << "  \"notes\": [";
    for (size_t i = 0; i < result.notes.size(); ++i) {
        out << (i > 0 ? ", " : "") << quote(result.notes[i]);
    }
    out << "],\n";
    out << "  \"warnings\": [";
    for (size_t i = 0; i < result.warnings.size(); ++i) {
        out << (i > 0 ? ", " : "") << quote(result.warnings[i]);
    }
    out << "]\n}\n";
    return out.str();
}

std::string MasteringEngine::exportReaEQ(const Analysis& analysis) {
    std::ostringstream out;
    if (analysis.plan.monoBassHz > 0.0) {
        out << "Note: mono bass below " << std::fixed << std::setprecision(0) << analysis.plan.monoBassHz
            << " Hz is applied by the engine and is not part of these bands\n";
    }
    out << (analysis.plan.midSide ? "Mid EQ\n" : "EQ\n");
    if (analysis.mid.bands.empty()) {
        out << "No bands\n";
    }
    for (size_t i = 0; i < analysis.mid.bands.size(); ++i) {
        out << reaLine(i + 1, analysis.mid.bands[i]) << "\n";
    }
    if (analysis.plan.midSide) {
        out << "Side EQ\n";
        if (analysis.side.bands.empty()) {
            out << "No bands\n";
        }
        for (size_t i = 0; i < analysis.side.bands.size(); ++i) {
            out << reaLine(i + 1, analysis.side.bands[i]) << "\n";
        }
        if (std::fabs(analysis.sideGainDb) >= 0.05) {
            out << "Side level: " << std::showpos << std::fixed << std::setprecision(2) << analysis.sideGainDb << std::noshowpos << " dB\n";
        }
    }
    return out.str();
}

std::string MasteringEngine::exportEqualizerAPO(const Analysis& analysis) {
    std::ostringstream out;
    if (analysis.plan.monoBassHz > 0.0) {
        out << "# Mono bass below " << std::fixed << std::setprecision(0) << analysis.plan.monoBassHz
            << " Hz is applied by the engine and is not part of this file\n";
    }
    double peakGain = 0.0;
    for (double value : analysis.mid.responseDb) {
        peakGain = std::max(peakGain, value);
    }
    if (peakGain > 0.05) {
        out << "Preamp: " << std::fixed << std::setprecision(1) << -peakGain << " dB\n";
    }
    if (analysis.plan.midSide) {
        out << "Copy: R=L+-1.0*R\n";
        out << "Copy: L=L+-0.5*R\n";
        out << "Channel: L\n";
    }
    for (size_t i = 0; i < analysis.mid.bands.size(); ++i) {
        out << apoLine(i + 1, analysis.mid.bands[i]) << "\n";
    }
    if (analysis.plan.midSide) {
        out << "Channel: R\n";
        if (std::fabs(analysis.sideGainDb) >= 0.05) {
            out << "Preamp: " << std::fixed << std::setprecision(2) << analysis.sideGainDb << " dB\n";
        }
        for (size_t i = 0; i < analysis.side.bands.size(); ++i) {
            out << apoLine(i + 1, analysis.side.bands[i]) << "\n";
        }
        out << "Channel: all\n";
        out << "Copy: L=L+0.5*R\n";
        out << "Copy: R=L+-1.0*R\n";
    }
    return out.str();
}

}
