#include <iostream>
#include <string>
#include <fstream>
#include <filesystem>
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
    
    // Validate file existence
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
        std::cout << "║  Mastered Engine - Professional Audio Mastering              ║\n";
        std::cout << "║  Full Processing Pipeline: Analyze → EQ → Apply → Output    ║\n";
        std::cout << "╚══════════════════════════════════════════════════════════════╝\n\n";
        
        std::cout << "→ Loading reference track: " << referenceFile << "\n";
        std::cout << "→ Loading unmastered track: " << unmasteredFile << "\n\n";
        
        MasteringEngine engine(createDefaultConfig());
        
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
        std::string baseName = std::filesystem::path(unmasteredFile).stem().string();
        std::string masteredFile = "mastered_" + baseName + ".wav";
        std::cout << "\n→ Saving mastered audio: " << masteredFile << "\n";
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
