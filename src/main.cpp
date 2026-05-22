#include <iostream>
#include <string>
#include <fstream>
#include "mastering_engine.h"

using namespace mastered;

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " <reference.wav> <unmastered.wav> [output.json]\n";
    std::cout << "Analyzes unmastered audio, applies mastering EQ, and outputs:\n";
    std::cout << "  - mastered_[input].wav - Mastered audio file\n";
    std::cout << "  - [output].json - Analysis and EQ settings (optional)\n";
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }
    
    std::string referenceFile = argv[1];
    std::string unmasteredFile = argv[2];
    std::string outputFile = (argc > 3) ? argv[3] : "mastering_result.json";
    
    try {
        std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
        std::cout << "║  Mastered Engine - Professional Audio Mastering              ║\n";
        std::cout << "║  Full Processing Pipeline: Analyze → EQ → Apply → Output    ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";
        
        std::cout << "→ Loading reference track: " << referenceFile << "\n";
        std::cout << "→ Loading unmastered track: " << unmasteredFile << "\n\n";
        
        // Configure mastering engine
        MasteringConfig config;
        config.autoGain = true;
        config.perceptualWeighting = true;
        config.maxEQBands = 8;
        config.aggressiveness = 0.85f;
        config.smoothing = true;
        config.targetLoudnessLUFS = -14.f;
        
        MasteringEngine engine(config);
        
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
        std::cout << "  Spectral Correlation:  " << result.matchingStats.correlation << "\n";
        std::cout << "  Spectral Difference:   " << result.matchingStats.spectralDifference << " dB\n";
        std::cout << "  Confidence Score:      " << result.matchingStats.confidenceScore << "\n";
        std::cout << "  Estimated LUFS:        " << result.estimatedLUFS << "\n";
        std::cout << "  Makeup Gain:           " << result.makeupGain << " dB\n";
        std::cout << "  EQ Bands Generated:    " << result.eqCurve.bands.size() << "\n";
        std::cout << "────────────────────────────────────────────\n\n";
        
        std::cout << "EQ Bands:\n";
        for (const auto& band : result.eqCurve.bands) {
            std::cout << "  " << band.frequency << " Hz: " << band.gain << " dB "
                      << "(Q=" << band.qFactor << ", Type=" << band.type << ")\n";
        }
        
        // Apply mastering
        std::cout << "\n→ Applying mastering EQ to audio...\n";
        AudioBuffer unmasteredBuffer = AudioLoader::loadWAV(unmasteredFile);
        AudioBuffer masteredBuffer = engine.applyMastering(unmasteredBuffer, result.eqCurve, result.makeupGain);
        std::cout << "✓ EQ applied successfully!\n";
        
        // Save mastered audio
        size_t lastSlash = unmasteredFile.find_last_of("/\\");
        std::string baseName = (lastSlash != std::string::npos) ? unmasteredFile.substr(lastSlash + 1) : unmasteredFile;
        size_t dotPos = baseName.find_last_of(".");
        std::string nameWithoutExt = (dotPos != std::string::npos) ? baseName.substr(0, dotPos) : baseName;
        std::string masteredFile = "mastered_" + nameWithoutExt + ".wav";
        std::cout << "\n→ Saving mastered audio: " << masteredFile << "\n";
        if (AudioLoader::saveWAV(masteredFile, masteredBuffer)) {
            std::cout << "✓ Mastered audio saved!\n";
        } else {
            std::cerr << "✗ Warning: Could not save mastered audio\n";
        }
        
        // Export results as JSON
        std::cout << "→ Exporting analysis results: " << outputFile << "\n";
        std::string jsonOutput = engine.exportEQasJSON(result);
        
        std::ofstream outFile(outputFile);
        if (outFile.is_open()) {
            outFile << jsonOutput;
            outFile.close();
            std::cout << "✓ Analysis exported!\n";
        } else {
            std::cerr << "✗ Warning: Could not write to output file: " << outputFile << "\n";
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
