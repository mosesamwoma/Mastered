#include <iostream>
#include <string>
#include <fstream>
#include <filesystem>
#include <cstring>
#include <iomanip>
#include "mastering_engine.h"

using namespace mastered;

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " <reference.wav> <unmastered.wav> [output.json] [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  --verbose          Print detailed analysis info\n";
    std::cout << "  --aggressive N     Set aggressiveness (0-1, default 0.85)\n";
    std::cout << "  --headroom N       Set clipping headroom (0-1, default 0.99)\n";
    std::cout << "  --target N         Set target loudness in LUFS (default -14)\n";
    std::cout << "  --bitdepth N       Set output bit depth (16, 24, or 32, default 16)\n";
    std::cout << "  --help             Show this help message\n";
    std::cout << "\nExample:\n";
    std::cout << "  " << programName << " reference.wav beat.wav result.json --verbose --aggressive 0.8 --bitdepth 24\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }
    
    std::string referenceFile = argv[1];
    std::string unmasteredFile = argv[2];
    std::string outputFile = (argc > 3 && argv[3][0] != '-') ? argv[3] : "mastering_result.json";
    
    MasteringConfig config = createDefaultConfig();
    uint16_t outputBitDepth = 0;   // 0 = preserve source bit depth
    
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        
        if (arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        else if (arg == "--verbose") {
            config.verbose = true;
        }
        else if (arg == "--aggressive" && i + 1 < argc) {
            config.aggressiveness = std::stof(argv[++i]);
            if (config.aggressiveness < 0.f || config.aggressiveness > 1.f) {
                std::cerr << "✗ Error: aggressiveness must be between 0 and 1\n";
                return 1;
            }
        }
        else if (arg == "--headroom" && i + 1 < argc) {
            config.clippingHeadroom = std::stof(argv[++i]);
            if (config.clippingHeadroom <= 0.f || config.clippingHeadroom > 1.f) {
                std::cerr << "✗ Error: headroom must be between 0 and 1\n";
                return 1;
            }
        }
        else if (arg == "--target" && i + 1 < argc) {
            config.targetLoudnessLUFS = std::stof(argv[++i]);
            if (config.targetLoudnessLUFS > 0.f) {
                std::cerr << "✗ Error: target LUFS must be negative\n";
                return 1;
            }
        }
        else if (arg == "--bitdepth" && i + 1 < argc) {
            outputBitDepth = std::stoi(argv[++i]);
            if (outputBitDepth != 16 && outputBitDepth != 24 && outputBitDepth != 32) {
                std::cerr << "✗ Error: bitdepth must be 16, 24, or 32\n";
                return 1;
            }
        }
    }
    
    if (!std::filesystem::exists(referenceFile)) {
        std::cerr << "✗ Error: Reference file not found: " << referenceFile << "\n";
        return 1;
    }
    if (!std::filesystem::exists(unmasteredFile)) {
        std::cerr << "✗ Error: Unmastered file not found: " << unmasteredFile << "\n";
        return 1;
    }
    
    try {
        std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
        std::cout << "║  Mastered Engine v1.1 - Professional Audio Mastering        ║\n";
        std::cout << "║  Full Processing Pipeline: Analyze → EQ → Apply → Output    ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";
        
        if (config.verbose) {
            std::cout << "Configuration:\n";
            std::cout << "  Aggressiveness: " << config.aggressiveness << "\n";
            std::cout << "  Clipping Headroom: " << config.clippingHeadroom << "\n";
            std::cout << "  Target Loudness: " << config.targetLoudnessLUFS << " LUFS\n";
            std::cout << "  Perceptual Weighting: " << (config.perceptualWeighting ? "Yes" : "No") << "\n\n";
        }
        
        std::cout << "→ Loading reference track: " << referenceFile << "\n";
        std::cout << "→ Loading unmastered track: " << unmasteredFile << "\n\n";
        
        MasteringEngine engine(config);
        
        // Set up progress callback to show live feedback
        engine.setProgressCallback([](const std::string& message, float percentage) {
            if (percentage >= 0.f) {
                std::cout << "  [" << std::fixed << std::setw(5) << std::setprecision(1) 
                         << percentage << "%] " << message << "\n";
            }
        });
        
        // Load audio
        std::cout << "→ Analyzing tracks...\n";
        auto result = engine.analyzeTracks(referenceFile, unmasteredFile);
        
        if (!result.success) {
            std::cerr << "✗ Error: " << result.message << "\n";
            return 1;
        }
        
        std::cout << "✓ Analysis complete!\n\n";
        std::cout << "Results:\n";
        std::cout << "────────────────────────────────────────────\n";
        std::cout << "  Spectral Correlation:  " << std::fixed << std::setprecision(4) 
                  << result.matchingStats.correlation << "\n";
        std::cout << "  Spectral Difference:   " << std::setprecision(2) 
                  << result.matchingStats.spectralDifference << " dB\n";
        std::cout << "  Confidence Score:      " << std::setprecision(4) 
                  << result.matchingStats.confidenceScore << "\n";
        std::cout << "  Estimated LUFS:        " << std::setprecision(2) 
                  << result.estimatedLUFS << "\n";
        std::cout << "  Makeup Gain:           " << result.makeupGain << " dB\n";
        std::cout << "  EQ Bands Generated:    " << result.eqCurve.bands.size() << "\n";
        std::cout << "────────────────────────────────────────────\n\n";
        
        if (!result.eqCurve.bands.empty()) {
            std::cout << "EQ Bands:\n";
            for (size_t i = 0; i < result.eqCurve.bands.size(); ++i) {
                const auto& band = result.eqCurve.bands[i];
                std::cout << "  [" << (i + 1) << "] " << std::fixed << std::setprecision(1) 
                          << band.frequency << " Hz: " << std::setprecision(2) 
                          << band.gain << " dB (Q=" << band.qFactor << ", Type=" << band.type << ")\n";
            }
        } else {
            std::cout << "⚠ No EQ bands needed - reference and input are well-matched\n";
        }
        
        // Apply mastering
        std::cout << "\n→ Applying mastering EQ to audio...\n";
        AudioBuffer& unmasteredBuffer = result.inputBuffer;  // Reuse retained buffer
        if (outputBitDepth != 0)
            unmasteredBuffer.bitDepth = outputBitDepth;  // Override bit depth if specified
        AudioBuffer masteredBuffer = engine.applyMastering(unmasteredBuffer, result.eqCurve, result.makeupGain);
        std::cout << "✓ EQ applied successfully!\n";
        
        // Save mastered audio
        std::string baseName = std::filesystem::path(unmasteredFile).stem().string();
        std::string masteredFile = "mastered_" + baseName + ".wav";
        std::cout << "\n→ Saving mastered audio: " << masteredFile;
        if (outputBitDepth != 0) std::cout << " (" << masteredBuffer.bitDepth << "-bit)";
        std::cout << "\n";
        if (!AudioLoader::saveWAV(masteredFile, masteredBuffer)) {
            std::cerr << "✗ Error: Could not save mastered audio to: " << masteredFile << "\n";
            std::cerr << "  Check: disk space, file permissions, output directory exists\n";
            return 1;
        }
        std::cout << "✓ Mastered audio saved!\n";
        
        // Export results as JSON
        std::cout << "→ Exporting analysis results: " << outputFile << "\n";
        std::string jsonOutput = engine.exportEQasJSON(result);
        
        std::ofstream outFile(outputFile);
        if (!outFile.is_open()) {
            std::cerr << "✗ Error: Could not write to output file: " << outputFile << "\n";
            std::cerr << "  Check: file permissions, output directory exists\n";
            return 1;
        }
        outFile << jsonOutput;
        outFile.close();
        std::cout << "✓ Analysis exported!\n";
        
        if (config.verbose) {
            std::string reaEQOutput = engine.exportEQasReaEQ(result);
            std::string reaEQFile = baseName + "_reaEQ.txt";
            std::ofstream reaFile(reaEQFile);
            reaFile << reaEQOutput;
            reaFile.close();
            std::cout << "✓ ReaEQ export: " << reaEQFile << "\n";
            
            std::string apoOutput = engine.exportEQasEqualizerAPO(result);
            std::string apoFile = baseName + "_EqualizerAPO.txt";
            std::ofstream apoFil(apoFile);
            apoFil << apoOutput;
            apoFil.close();
            std::cout << "✓ EqualizerAPO export: " << apoFile << "\n";
        }
        
        std::cout << "\n╔══════════════════════════════════════════════════════════════╗\n";
        std::cout << "║  ✓ MASTERING COMPLETE                                         ║\n";
        std::cout << "║                                                               ║\n";
        std::cout << "║  Output Files:                                                ║\n";
        std::cout << "║  • " << masteredFile << " (Mastered Audio)\n";
        std::cout << "║  • " << outputFile << " (Analysis & EQ Settings)\n";
        std::cout << "║                                                               ║\n";
        std::cout << "║  Your beat is now ready for streaming! 🎵                    ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "✗ Fatal error: " << e.what() << "\n";
        return 1;
    }
}
