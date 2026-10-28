#include "mastering_engine.h"

#include "constants.h"
#include "dynamics.h"
#include "error.h"
#include "wav_io.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace mastered {

namespace {

constexpr double kMinLevelHz = 63.0;
constexpr double kMaxLevelHz = 12500.0;
constexpr double kSideFloorDb = -45.0;
constexpr double kMaxTargetDb = 12.0;
constexpr double kMaxWidthDb = 3.0;
constexpr double kMaxMakeupDb = 40.0;

struct FileScan {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitDepth = 0;
    bool isFloat = false;
    uint64_t frames = 0;
    AverageSpectrum spectrum;
    Measurements measurements;
    std::array<double, 3> bandRmsDb{{-90.0, -90.0, -90.0}};
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
    requireRange(c.limiterLookaheadMs, 0.5, 20.0, "limiterLookaheadMs");
    requireRange(c.limiterReleaseMs, 5.0, 1000.0, "limiterReleaseMs");
    requireRange(c.loudnessToleranceLu, 0.01, 2.0, "loudnessToleranceLu");
    requireRange(static_cast<double>(c.maxLoudnessIterations), 1.0, 30.0, "maxLoudnessIterations");
    if (c.bitDepth != 0 && c.bitDepth != 16 && c.bitDepth != 24 && c.bitDepth != 32) {
        throw MasteredError("Configuration value out of range: bitDepth must be 0, 16, 24 or 32");
    }
    if (c.fftSize < 1024 || c.fftSize > 65536 || (c.fftSize & (c.fftSize - 1)) != 0) {
        throw MasteredError("Configuration value out of range: fftSize must be a power of two between 1024 and 65536");
    }
}

FileScan scanFile(const std::string& path, size_t fftSize) {
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
    Meter meter(scan.sampleRate, scan.channels);
    BandLevelMeter bands(scan.sampleRate, DynamicsSettings().lowCrossoverHz, DynamicsSettings().highCrossoverHz);
    AudioBlock block;
    size_t n;
    while ((n = reader.read(block, kBlockFrames)) > 0) {
        spectrum.push(block, n);
        meter.process(block, n);
        bands.process(block, n);
    }
    scan.spectrum = spectrum.finish();
    scan.measurements = meter.finish();
    scan.bandRmsDb = bands.rmsDb();
    return scan;
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

CurveFit fitCurve(const std::vector<double>& centres, const std::vector<double>& target, size_t maxBands, double sampleRate, double maxHz, bool perceptual) {
    FitOptions options;
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

std::vector<double> buildTarget(const AverageSpectrum& input, const std::vector<const AverageSpectrum*>& references, bool side,
                                const std::vector<double>& centres, double width, const std::vector<size_t>& range, double aggressiveness) {
    const std::vector<double> inputShape = shapeOf(bandLevelsDb(input, side, centres, width), range);
    std::vector<double> referenceShape(centres.size(), 0.0);
    for (const AverageSpectrum* reference : references) {
        const std::vector<double> shape = shapeOf(bandLevelsDb(*reference, side, centres, width), range);
        for (size_t i = 0; i < shape.size(); ++i) {
            referenceShape[i] += shape[i] / static_cast<double>(references.size());
        }
    }
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
        const double value = (difference[i] - offset) * aggressiveness;
        target[i] = std::min(std::max(value, -kMaxTargetDb), kMaxTargetDb);
    }
    return target;
}

Measurements runPass(const std::string& input, const ChainPlan& plan, WavWriter* writer, bool measureTruePeak) {
    WavReader reader(input);
    const size_t channels = reader.channels();
    ProcessingChain chain(plan, reader.sampleRate(), channels);
    Meter meter(reader.sampleRate(), channels, measureTruePeak);
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
        if (writer) {
            writer->write(data, count);
        }
    };

    size_t n;
    while ((n = reader.read(block, kBlockFrames)) > 0) {
        chain.process(block, n);
        emitBlock(block, n);
    }
    size_t pending = chain.latency();
    while (pending > 0) {
        const size_t count = std::min(pending, kBlockFrames);
        AudioBlock tail = makeBlock(channels, count);
        chain.process(tail, count);
        emitBlock(tail, count);
        pending -= count;
    }
    return meter.finish();
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
    out << "{\"integratedLufs\": " << num(m.integratedLufs, 2)
        << ", \"loudnessRangeLu\": " << num(m.loudnessRangeLu, 2)
        << ", \"truePeakDb\": " << num(m.truePeakDb, 2)
        << ", \"stereoCorrelation\": " << (m.hasStereo ? num(m.correlation, 3) : "null")
        << ", \"sideToMidDb\": " << (m.hasStereo ? num(m.sideToMidDb, 2) : "null") << "}";
    return out.str();
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

std::string bandLine(size_t index, const EQBand& band, const char* style) {
    std::ostringstream out;
    out << std::fixed;
    if (std::string(style) == "apo") {
        const char* type = band.type == BandType::LowShelf ? "LSC" : (band.type == BandType::HighShelf ? "HSC" : "PK");
        out << "Filter " << index << ": ON " << type << " Fc " << std::setprecision(1) << band.frequency << " Hz Gain "
            << std::setprecision(2) << band.gainDb << " dB Q " << std::setprecision(3) << band.q;
    } else {
        const char* type = band.type == BandType::LowShelf ? "Low Shelf" : (band.type == BandType::HighShelf ? "High Shelf" : "Band");
        out << "Band " << index << ": " << type << " Freq " << std::setprecision(1) << band.frequency << " Hz Gain "
            << std::setprecision(2) << band.gainDb << " dB Q " << std::setprecision(3) << band.q;
    }
    return out.str();
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
    if (references.empty()) {
        throw MasteredError("At least one reference track is required");
    }
    emit("Analysing input track", 0.0);
    const FileScan in = scanFile(input, config_.fftSize);
    std::vector<FileScan> refs;
    refs.reserve(references.size());
    for (size_t i = 0; i < references.size(); ++i) {
        emit("Analysing reference " + std::to_string(i + 1) + " of " + std::to_string(references.size()),
             30.0 * static_cast<double>(i + 1) / static_cast<double>(references.size() + 1));
        refs.push_back(scanFile(references[i], config_.fftSize));
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
        }
    }
    if (stereoRefs > 0) {
        averaged.hasStereo = true;
        averaged.correlation /= static_cast<double>(stereoRefs);
        averaged.sideToMidDb /= static_cast<double>(stereoRefs);
    } else {
        averaged.correlation = 1.0;
        averaged.sideToMidDb = -120.0;
    }
    analysis.reference = averaged;

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

    emit("Fitting mid EQ", 32.0);
    std::vector<const AverageSpectrum*> referenceSpectra;
    for (const FileScan& ref : refs) {
        referenceSpectra.push_back(&ref.spectrum);
    }
    const std::vector<double> midTarget = buildTarget(in.spectrum, referenceSpectra, false, centres, width, range, config_.aggressiveness);
    analysis.mid = fitCurve(centres, midTarget, config_.maxEQBands, in.sampleRate, maxHz, config_.perceptualWeighting);
    analysis.plan.midBands = analysis.mid.bands;

    const bool inputStereo = in.channels == 2 && in.measurements.sideToMidDb > kSideFloorDb;
    bool referencesStereo = true;
    for (const FileScan& ref : refs) {
        if (!ref.spectrum.hasSide || !ref.measurements.hasStereo || ref.measurements.sideToMidDb <= kSideFloorDb) {
            referencesStereo = false;
        }
    }
    if (inputStereo && referencesStereo) {
        if (config_.midSide && config_.maxSideBands > 0) {
            emit("Fitting side EQ", 34.0);
            const std::vector<double> sideTarget = buildTarget(in.spectrum, referenceSpectra, true, centres, width, range, config_.aggressiveness);
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

    if (config_.dynamics) {
        DynamicsSettings dynamics;
        dynamics.enabled = true;
        double ratio = config_.dynamicsRatio;
        if (config_.dynamicsMatch) {
            const bool measurable = in.measurements.integratedLufs > -100.0 && averaged.integratedLufs > -100.0;
            const double inputPlr = in.measurements.truePeakDb - in.measurements.integratedLufs;
            const double referencePlr = averaged.truePeakDb - averaged.integratedLufs;
            double delta = 0.0;
            if (measurable) {
                delta = std::max(0.0, inputPlr - referencePlr) +
                        0.5 * std::max(0.0, in.measurements.loudnessRangeLu - averaged.loudnessRangeLu);
            }
            ratio = std::min(std::max(1.0 + delta / 4.0, 1.0), 4.0);
        }
        if (ratio < 1.05) {
            dynamics.enabled = false;
            analysis.warnings.push_back("Dynamics already match the reference; compression skipped");
        } else {
            const std::array<double, 3> attack = {30.0, 15.0, 5.0};
            const std::array<double, 3> release = {250.0, 150.0, 100.0};
            for (size_t b = 0; b < 3; ++b) {
                dynamics.bands[b].thresholdDb = in.bandRmsDb[b] + config_.dynamicsThresholdOffsetDb;
                dynamics.bands[b].ratio = ratio;
                dynamics.bands[b].kneeDb = 6.0;
                dynamics.bands[b].attackMs = attack[b];
                dynamics.bands[b].releaseMs = release[b];
            }
        }
        analysis.plan.dynamics = dynamics;
    }

    analysis.plan.softClip = config_.softClip;
    analysis.plan.softClipKneeDb = config_.softClipKneeDb;
    analysis.plan.limiterEnabled = true;
    analysis.plan.ceilingDb = config_.ceilingDb;
    analysis.plan.lookaheadMs = config_.limiterLookaheadMs;
    analysis.plan.releaseMs = config_.limiterReleaseMs;

    if (in.measurements.truePeakDb > 0.0) {
        analysis.warnings.push_back("Input contains true peaks above 0 dBTP");
    }
    if (in.measurements.integratedLufs <= -100.0) {
        analysis.warnings.push_back("Input is too quiet to measure loudness");
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

    const ChainPlan base = analysis.plan;
    double gain = 0.0;
    int iterations = 0;
    double bestError = 0.0;

    if (config_.autoGain) {
        emit("Measuring loudness", 38.0);
        ChainPlan pre = base;
        pre.limiterEnabled = false;
        pre.gainDb = 0.0;
        const Measurements preMeasure = runPass(input, pre, nullptr, false);
        if (preMeasure.integratedLufs <= -100.0) {
            throw MasteredError("Audio is too quiet to normalise to a loudness target");
        }
        gain = std::min(std::max(config_.targetLufs - preMeasure.integratedLufs, -kMaxMakeupDb), kMaxMakeupDb);
        double bestGain = gain;
        bestError = 1e9;
        double previousGain = 0.0;
        double previousLufs = 0.0;
        for (int i = 1; i <= config_.maxLoudnessIterations; ++i) {
            emit("Matching loudness (pass " + std::to_string(i) + ")", 38.0 + 52.0 * static_cast<double>(i) / static_cast<double>(config_.maxLoudnessIterations));
            ChainPlan trial = base;
            trial.gainDb = gain;
            const Measurements m = runPass(input, trial, nullptr, false);
            iterations = i;
            const double error = config_.targetLufs - m.integratedLufs;
            if (std::fabs(error) < std::fabs(bestError)) {
                bestError = error;
                bestGain = gain;
            }
            if (std::fabs(error) <= config_.loudnessToleranceLu) {
                break;
            }
            double slope = 1.0;
            if (i > 1 && std::fabs(gain - previousGain) > 1e-6) {
                slope = std::min(std::max((m.integratedLufs - previousLufs) / (gain - previousGain), 0.15), 1.0);
            }
            previousGain = gain;
            previousLufs = m.integratedLufs;
            const double next = std::min(std::max(gain + error / slope, -kMaxMakeupDb), kMaxMakeupDb);
            if (std::fabs(next - gain) < 1e-4) {
                break;
            }
            gain = next;
        }
        gain = bestGain;
    }

    uint16_t depth = config_.bitDepth;
    if (depth == 0) {
        depth = analysis.sourceFloat ? 32 : analysis.sourceBitDepth;
    }
    if (depth > 32) {
        depth = 32;
    }
    emit("Writing mastered audio", 92.0);
    ChainPlan finalPlan = base;
    finalPlan.gainDb = gain;
    WavWriter writer(output, analysis.sampleRate, analysis.channels, depth, config_.dither, config_.noiseShaping);
    result.output = runPass(input, finalPlan, &writer, true);
    writer.finalize();

    result.analysis.plan = finalPlan;
    result.outputBitDepth = depth;
    result.makeupGainDb = gain;
    result.iterations = iterations;
    if (config_.autoGain) {
        const double finalError = config_.targetLufs - result.output.integratedLufs;
        result.targetReached = std::fabs(finalError) <= std::max(config_.loudnessToleranceLu, 0.05) * 1.5;
        if (!result.targetReached) {
            std::ostringstream message;
            message << "Loudness target not reached: measured " << std::fixed << std::setprecision(1) << result.output.integratedLufs
                    << " LUFS against a target of " << config_.targetLufs << " LUFS; the true-peak ceiling limits the achievable loudness";
            result.warnings.push_back(message.str());
        }
    }
    if (result.output.hasStereo && result.output.correlation < 0.0) {
        result.warnings.push_back("Output stereo correlation is negative; check mono compatibility");
    }
    emit("Done", 100.0);
    return result;
}

MasteringResult MasteringEngine::run(const std::vector<std::string>& references, const std::string& input, const std::string& output) {
    const Analysis analysis = analyze(references, input);
    return render(analysis, input, output);
}

std::string MasteringEngine::exportJSON(const MasteringResult& result) {
    const Analysis& a = result.analysis;
    std::ostringstream out;
    out << "{\n";
    out << "  \"version\": " << quote(kVersion) << ",\n";
    out << "  \"input\": {\"path\": " << quote(result.inputPath) << ", \"sampleRate\": " << a.sampleRate << ", \"channels\": " << a.channels
        << ", \"frames\": " << a.frames << ", \"measurements\": " << measurementsJson(a.input) << "},\n";
    out << "  \"references\": {\"count\": " << a.referenceCount << ", \"measurements\": " << measurementsJson(a.reference) << "},\n";
    out << "  \"output\": {\"path\": " << quote(result.outputPath) << ", \"bitDepth\": " << result.outputBitDepth
        << ", \"measurements\": " << measurementsJson(result.output) << "},\n";
    out << "  \"gain\": {\"makeupDb\": " << num(result.makeupGainDb, 2) << ", \"iterations\": " << result.iterations
        << ", \"targetReached\": " << (result.targetReached ? "true" : "false") << "},\n";
    out << "  \"mid\": " << curveJson(a.mid) << ",\n";
    out << "  \"side\": " << curveJson(a.side) << ",\n";
    out << "  \"width\": {\"sideGainDb\": " << num(a.sideGainDb, 2) << ", \"midSide\": " << (a.plan.midSide ? "true" : "false") << "},\n";
    out << "  \"dynamics\": {\"enabled\": " << (a.plan.dynamics.enabled ? "true" : "false") << ", \"bands\": [";
    for (size_t i = 0; i < a.plan.dynamics.bands.size(); ++i) {
        const CompressorBand& b = a.plan.dynamics.bands[i];
        out << (i > 0 ? ", " : "") << "{\"thresholdDb\": " << num(b.thresholdDb, 2) << ", \"ratio\": " << num(b.ratio, 2)
            << ", \"kneeDb\": " << num(b.kneeDb, 1) << ", \"attackMs\": " << num(b.attackMs, 1) << ", \"releaseMs\": " << num(b.releaseMs, 1) << "}";
    }
    out << "]},\n";
    out << "  \"warnings\": [";
    for (size_t i = 0; i < result.warnings.size(); ++i) {
        out << (i > 0 ? ", " : "") << quote(result.warnings[i]);
    }
    out << "]\n}\n";
    return out.str();
}

std::string MasteringEngine::exportReaEQ(const Analysis& analysis) {
    std::ostringstream out;
    out << "Mid EQ\n";
    for (size_t i = 0; i < analysis.mid.bands.size(); ++i) {
        out << bandLine(i + 1, analysis.mid.bands[i], "rea") << "\n";
    }
    if (!analysis.side.bands.empty()) {
        out << "Side EQ\n";
        for (size_t i = 0; i < analysis.side.bands.size(); ++i) {
            out << bandLine(i + 1, analysis.side.bands[i], "rea") << "\n";
        }
    }
    return out.str();
}

std::string MasteringEngine::exportEqualizerAPO(const Analysis& analysis) {
    std::ostringstream out;
    for (size_t i = 0; i < analysis.mid.bands.size(); ++i) {
        out << bandLine(i + 1, analysis.mid.bands[i], "apo") << "\n";
    }
    return out.str();
}

}
