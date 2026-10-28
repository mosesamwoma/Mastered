#include "constants.h"
#include "error.h"
#include "mastering_engine.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

using mastered::MasteredError;

void printUsage() {
    std::cout <<
        "Mastered " << mastered::kVersion << " - reference-based audio mastering\n\n"
        "Usage:\n"
        "  mastered_cli <reference.wav> <input.wav> [options]\n\n"
        "Options:\n"
        "  --reference PATH      Additional reference track (repeatable)\n"
        "  --output PATH         Mastered WAV path (default: <input>_mastered.wav)\n"
        "  --json PATH           Analysis report path (default: output with .json)\n"
        "  --presets             Also write ReaEQ and Equalizer APO band lists\n"
        "  --target LUFS         Loudness target, -60 to -1 (default -14)\n"
        "  --aggressive N        EQ strength, 0 to 1 (default 0.85)\n"
        "  --ceiling DB          True-peak ceiling, -12 to 0 (default -1)\n"
        "  --bands N             Maximum mid EQ bands, 0 to 32 (default 8)\n"
        "  --side-bands N        Maximum side EQ bands, 0 to 32 (default 4)\n"
        "  --bitdepth N          16, 24 or 32 (32 writes float); default keeps source\n"
        "  --no-dither           Disable TPDF dither for 16 and 24 bit output\n"
        "  --noise-shaping       Enable first-order noise shaping\n"
        "  --no-mid-side         Disable mid/side EQ\n"
        "  --no-width            Disable stereo width matching\n"
        "  --no-auto-gain        Skip the loudness loop\n"
        "  --no-smoothing        Use 1/6 octave analysis instead of 1/3 octave\n"
        "  --no-perceptual       Disable perceptual error weighting in the EQ fit\n"
        "  --dynamics            Enable multiband compression matched to the reference\n"
        "  --dynamics-ratio N    Fixed compression ratio, 1 to 20 (implies --dynamics)\n"
        "  --soft-clip           Enable soft clipping before the limiter\n"
        "  --lookahead MS        Limiter look-ahead, 0.5 to 20 (default 3)\n"
        "  --release MS          Limiter release, 5 to 1000 (default 80)\n"
        "  --fft N               FFT size, power of two 1024 to 65536 (default 8192)\n"
        "  --verbose             Print EQ bands and measurements\n"
        "  --help                Show this help\n";
}

double parseNumber(const std::string& option, const std::string& text) {
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(value)) {
        throw MasteredError("Invalid value for " + option + ": " + text);
    }
    return value;
}

size_t parseCount(const std::string& option, const std::string& text, double low, double high) {
    const double value = parseNumber(option, text);
    if (value != std::floor(value) || value < low || value > high) {
        throw MasteredError("Invalid value for " + option + ": " + text);
    }
    return static_cast<size_t>(value);
}

void writeTextFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        throw MasteredError("Cannot write file: " + path.string());
    }
    file << content;
    file.flush();
    if (!file) {
        throw MasteredError("Failed to write file: " + path.string());
    }
}

void printMeasurements(const char* label, const mastered::Measurements& m) {
    std::cout << "  " << std::left << std::setw(10) << label << std::right << std::fixed << std::setprecision(1)
              << std::setw(7) << m.integratedLufs << " LUFS  " << std::setw(6) << m.truePeakDb << " dBTP  LRA "
              << std::setw(4) << m.loudnessRangeLu << " LU";
    if (m.hasStereo) {
        std::cout << "  corr " << std::setprecision(2) << m.correlation;
    }
    std::cout << "\n";
}

void printBands(const char* title, const std::vector<mastered::EQBand>& bands) {
    if (bands.empty()) {
        std::cout << "  " << title << ": no correction needed\n";
        return;
    }
    std::cout << "  " << title << ":\n";
    for (size_t i = 0; i < bands.size(); ++i) {
        std::cout << "    " << (i + 1) << ". " << std::left << std::setw(10) << mastered::bandTypeName(bands[i].type) << std::right
                  << std::fixed << std::setprecision(1) << std::setw(8) << bands[i].frequency << " Hz  " << std::showpos
                  << std::setprecision(2) << std::setw(6) << bands[i].gainDb << std::noshowpos << " dB  Q " << std::setprecision(2)
                  << bands[i].q << "\n";
    }
}

int run(int argc, char** argv) {
    mastered::MasteringConfig config;
    std::vector<std::string> positional;
    std::vector<std::string> extraReferences;
    std::string outputPath;
    std::string jsonPath;
    bool presets = false;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                throw MasteredError("Missing value for " + arg);
            }
            return argv[++i];
        };
        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--reference") {
            extraReferences.push_back(value());
        } else if (arg == "--output") {
            outputPath = value();
        } else if (arg == "--json") {
            jsonPath = value();
        } else if (arg == "--presets") {
            presets = true;
        } else if (arg == "--target") {
            config.targetLufs = parseNumber(arg, value());
        } else if (arg == "--aggressive") {
            config.aggressiveness = parseNumber(arg, value());
        } else if (arg == "--ceiling") {
            config.ceilingDb = parseNumber(arg, value());
        } else if (arg == "--bands") {
            config.maxEQBands = parseCount(arg, value(), 0, 32);
        } else if (arg == "--side-bands") {
            config.maxSideBands = parseCount(arg, value(), 0, 32);
        } else if (arg == "--bitdepth") {
            config.bitDepth = static_cast<uint16_t>(parseCount(arg, value(), 16, 32));
        } else if (arg == "--no-dither") {
            config.dither = false;
        } else if (arg == "--noise-shaping") {
            config.noiseShaping = true;
        } else if (arg == "--no-mid-side") {
            config.midSide = false;
        } else if (arg == "--no-width") {
            config.matchWidth = false;
        } else if (arg == "--no-auto-gain") {
            config.autoGain = false;
        } else if (arg == "--no-smoothing") {
            config.smoothing = false;
        } else if (arg == "--no-perceptual") {
            config.perceptualWeighting = false;
        } else if (arg == "--dynamics") {
            config.dynamics = true;
        } else if (arg == "--dynamics-ratio") {
            config.dynamics = true;
            config.dynamicsMatch = false;
            config.dynamicsRatio = parseNumber(arg, value());
        } else if (arg == "--soft-clip") {
            config.softClip = true;
        } else if (arg == "--lookahead") {
            config.limiterLookaheadMs = parseNumber(arg, value());
        } else if (arg == "--release") {
            config.limiterReleaseMs = parseNumber(arg, value());
        } else if (arg == "--fft") {
            config.fftSize = parseCount(arg, value(), 1024, 65536);
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg.size() > 1 && arg[0] == '-' && arg != "-") {
            throw MasteredError("Unknown option: " + arg);
        } else {
            positional.push_back(arg);
        }
    }
    if (positional.size() != 2) {
        printUsage();
        return 2;
    }

    const std::string referencePath = positional[0];
    const std::string inputPath = positional[1];
    std::vector<std::string> references{referencePath};
    references.insert(references.end(), extraReferences.begin(), extraReferences.end());

    std::filesystem::path output;
    if (outputPath.empty()) {
        const std::filesystem::path input(inputPath);
        output = input.parent_path() / (input.stem().string() + "_mastered.wav");
    } else {
        output = outputPath;
    }
    std::error_code ec;
    if (std::filesystem::exists(output, ec) && std::filesystem::exists(inputPath, ec) &&
        std::filesystem::equivalent(output, inputPath, ec)) {
        throw MasteredError("Output path must differ from the input path");
    }
    std::filesystem::path json = jsonPath.empty() ? std::filesystem::path(output).replace_extension(".json") : std::filesystem::path(jsonPath);

    mastered::MasteringEngine engine(config);
    engine.setProgressCallback([](const std::string& message, double percent) {
        std::cout << "[" << std::setw(5) << std::fixed << std::setprecision(1) << percent << "%] " << message << std::endl;
    });
    const mastered::MasteringResult result = engine.run(references, inputPath, output.string());

    std::cout << "\nResults\n";
    printMeasurements("Input", result.analysis.input);
    printMeasurements("Reference", result.analysis.reference);
    printMeasurements("Output", result.output);
    std::cout << "  Makeup gain " << std::fixed << std::setprecision(2) << result.makeupGainDb << " dB after " << result.iterations
              << " loudness pass" << (result.iterations == 1 ? "" : "es") << "\n";
    if (verbose) {
        printBands("Mid EQ", result.analysis.mid.bands);
        if (result.analysis.plan.midSide) {
            printBands("Side EQ", result.analysis.side.bands);
            std::cout << "  Side gain " << std::showpos << result.analysis.sideGainDb << std::noshowpos << " dB\n";
        }
        if (result.analysis.plan.dynamics.enabled) {
            std::cout << "  Dynamics ratio " << result.analysis.plan.dynamics.bands[0].ratio << ":1 (3 bands)\n";
        }
    }
    for (const std::string& warning : result.warnings) {
        std::cout << "  Warning: " << warning << "\n";
    }

    writeTextFile(json, mastered::MasteringEngine::exportJSON(result));
    std::cout << "\nMastered audio: " << output.string() << "\nReport:         " << json.string() << "\n";
    if (presets) {
        const std::filesystem::path reaeq = std::filesystem::path(output).replace_extension(".reaeq.txt");
        const std::filesystem::path apo = std::filesystem::path(output).replace_extension(".apo.txt");
        writeTextFile(reaeq, mastered::MasteringEngine::exportReaEQ(result.analysis));
        writeTextFile(apo, mastered::MasteringEngine::exportEqualizerAPO(result.analysis));
        std::cout << "ReaEQ bands:    " << reaeq.string() << "\nEqualizer APO:  " << apo.string() << "\n";
    }
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const MasteredError& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << "\n";
        return 1;
    }
}
