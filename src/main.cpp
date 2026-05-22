#include <iostream>
#include <string>
#include <fstream>
#include "mastering_engine.h"

using namespace mastered;

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " <reference.wav> <unmastered.wav> [output.json]\n";
    std::cout << "Analyzes unmastered audio against reference track and generates EQ curve.\n";
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
        std::cout << "Mastered Engine - Professional Audio Mastering\n";
        std::cout << "==============================================\n\n";
        
        std::cout << "Loading reference track: " << referenceFile << "\n";
        std::cout << "Loading unmastered track: " << unmasteredFile << "\n\n";
        
        // Configure mastering engine
        MasteringConfig config;
        config.autoGain = true;
        config.perceptualWeighting = true;
        config.maxEQBands = 8;
        config.aggressiveness = 0.85f;
        config.smoothing = true;
        config.targetLoudnessLUFS = -14.f;
        
        MasteringEngine engine(config);
        
        std::cout << "Analyzing tracks...\n";
        auto result = engine.analyzeTracks(referenceFile, unmasteredFile);
        
        if (!result.success) {
            std::cerr << "Error: " << result.message << "\n";
            return 1;
        }
        
        std::cout << "Analysis complete!\n\n";
        std::cout << "Results:\n";
        std::cout << "--------\n";
        std::cout << "Spectral Correlation: " << result.matchingStats.correlation << "\n";
        std::cout << "Spectral Difference: " << result.matchingStats.spectralDifference << " dB\n";
        std::cout << "Confidence Score: " << result.matchingStats.confidenceScore << "\n";
        std::cout << "Estimated LUFS: " << result.estimatedLUFS << "\n";
        std::cout << "Makeup Gain: " << result.makeupGain << " dB\n";
        std::cout << "EQ Bands Generated: " << result.eqCurve.bands.size() << "\n\n";
        
        std::cout << "EQ Bands:\n";
        for (const auto& band : result.eqCurve.bands) {
            std::cout << "  " << band.frequency << " Hz: " << band.gain << " dB "
                      << "(Q=" << band.qFactor << ", Type=" << band.type << ")\n";
        }
        
        // Export results
        std::string jsonOutput = engine.exportEQasJSON(result);
        
        std::ofstream outFile(outputFile);
        if (outFile.is_open()) {
            outFile << jsonOutput;
            outFile.close();
            std::cout << "\nResults exported to: " << outputFile << "\n";
        } else {
            std::cerr << "Warning: Could not write to output file: " << outputFile << "\n";
        }
        
        return 0;
        
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
}
