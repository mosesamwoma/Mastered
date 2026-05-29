#include "audio_loader.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cmath>

namespace mastered {

// Helper: Read little-endian value with error checking
template<typename T>
bool readLE(std::ifstream& file, T& value) {
    file.read(reinterpret_cast<char*>(&value), sizeof(T));
    return file.gcount() == sizeof(T);
}

// Helper: Get file size
uint32_t getFileSize(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file) return 0;
    return static_cast<uint32_t>(file.tellg());
}

AudioBuffer AudioLoader::loadWAV(const std::string& filepath) {
    AudioBuffer result;
    
    // ============ PRE-VALIDATION ============
    uint32_t fileSize = getFileSize(filepath);
    if (fileSize == 0) {
        throw std::runtime_error("File not found or empty: " + filepath);
    }
    
    // Maximum file size: 5 minutes at 192kHz, 32-bit stereo ≈ 460 MB (limit to 500 MB for safety)
    constexpr uint32_t MAX_FILE_SIZE = 500 * 1024 * 1024;
    if (fileSize > MAX_FILE_SIZE) {
        throw std::runtime_error("File too large (max 500MB for safety): " + filepath);
    }
    
    // ============ OPEN FILE ============
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filepath);
    }
    
    // ============ VALIDATE RIFF HEADER ============
    char riffID[4];
    if (!readLE(file, riffID) || std::string(riffID, 4) != "RIFF") {
        throw std::runtime_error("Invalid WAV file: missing RIFF header");
    }
    
    uint32_t riffSize = 0;
    if (!readLE(file, riffSize)) {
        throw std::runtime_error("File truncated: cannot read RIFF size");
    }
    
    // Validate RIFF size does not exceed actual file size (allow trailing chunks/padding)
    if (riffSize + 8 > fileSize) {
        throw std::runtime_error(
            "Corrupt WAV: RIFF header claims " + std::to_string(riffSize + 8) +
            " bytes but file is only " + std::to_string(fileSize) + " bytes"
        );
    }
    
    char waveID[4];
    if (!readLE(file, waveID) || std::string(waveID, 4) != "WAVE") {
        throw std::runtime_error("Invalid WAV file: missing WAVE header");
    }
    
    // ============ PARSE CHUNKS ============
    WAVHeader wavHeader = {};
    uint32_t dataSize = 0;
    uint32_t dataOffset = 0;
    bool foundFmt = false, foundData = false;
    
    while (file.tellg() < static_cast<int32_t>(fileSize)) {
        char chunkID[4];
        if (!readLE(file, chunkID)) {
            throw std::runtime_error("File truncated: cannot read chunk ID");
        }
        
        uint32_t chunkSize = 0;
        if (!readLE(file, chunkSize)) {
            throw std::runtime_error("File truncated: cannot read chunk size");
        }
        
        // Prevent chunk size from causing overflow
        if (chunkSize > MAX_FILE_SIZE) {
            throw std::runtime_error(
                "Invalid chunk size " + std::to_string(chunkSize) + 
                " (exceeds maximum sensible value)"
            );
        }
        
        uint32_t chunkStart = file.tellg();
        
        if (std::string(chunkID, 4) == "fmt ") {
            // ============ PARSE FORMAT CHUNK ============
            if (chunkSize < 16) {
                throw std::runtime_error("Format chunk too small: " + std::to_string(chunkSize));
            }
            
            if (!readLE(file, wavHeader.audioFormat)) throw std::runtime_error("File truncated in fmt chunk (audioFormat)");
            if (!readLE(file, wavHeader.numChannels)) throw std::runtime_error("File truncated in fmt chunk (numChannels)");
            if (!readLE(file, wavHeader.sampleRate)) throw std::runtime_error("File truncated in fmt chunk (sampleRate)");
            if (!readLE(file, wavHeader.byteRate)) throw std::runtime_error("File truncated in fmt chunk (byteRate)");
            if (!readLE(file, wavHeader.blockAlign)) throw std::runtime_error("File truncated in fmt chunk (blockAlign)");
            if (!readLE(file, wavHeader.bitsPerSample)) throw std::runtime_error("File truncated in fmt chunk (bitsPerSample)");
            
            // Validate audio format (only PCM supported)
            if (wavHeader.audioFormat != 1) {
                throw std::runtime_error(
                    "Unsupported audio format: " + std::to_string(wavHeader.audioFormat) + 
                    " (only PCM/1 is supported)"
                );
            }
            
            // Validate channel count (1-8 supported)
            if (wavHeader.numChannels == 0 || wavHeader.numChannels > 8) {
                throw std::runtime_error(
                    "Invalid channel count: " + std::to_string(wavHeader.numChannels) + 
                    " (must be 1-8)"
                );
            }
            
            // Validate sample rate (8 kHz to 192 kHz)
            if (wavHeader.sampleRate < 8000 || wavHeader.sampleRate > 192000) {
                throw std::runtime_error(
                    "Invalid sample rate: " + std::to_string(wavHeader.sampleRate) + 
                    " Hz (must be 8000-192000)"
                );
            }
            
            // Validate bit depth (16, 24, or 32 bit)
            if (wavHeader.bitsPerSample != 16 && wavHeader.bitsPerSample != 24 && wavHeader.bitsPerSample != 32) {
                throw std::runtime_error(
                    "Unsupported bit depth: " + std::to_string(wavHeader.bitsPerSample) + 
                    " (only 16, 24, 32 bit supported)"
                );
            }
            
            // Validate block align
            uint16_t expectedBlockAlign = wavHeader.numChannels * (wavHeader.bitsPerSample / 8);
            if (wavHeader.blockAlign == 0 || wavHeader.blockAlign != expectedBlockAlign) {
                throw std::runtime_error(
                    "Invalid block align: " + std::to_string(wavHeader.blockAlign) + 
                    " (expected " + std::to_string(expectedBlockAlign) + ")"
                );
            }
            
            // Validate byte rate
            uint32_t expectedByteRate = wavHeader.sampleRate * wavHeader.blockAlign;
            if (wavHeader.byteRate == 0 || wavHeader.byteRate != expectedByteRate) {
                throw std::runtime_error(
                    "Invalid byte rate: " + std::to_string(wavHeader.byteRate) + 
                    " (expected " + std::to_string(expectedByteRate) + ")"
                );
            }
            
            foundFmt = true;
        } 
        else if (std::string(chunkID, 4) == "data") {
            // ============ PARSE DATA CHUNK ============
            if (!foundFmt) {
                throw std::runtime_error("Data chunk found before format chunk");
            }
            
            dataSize = chunkSize;
            dataOffset = file.tellg();
            foundData = true;
            
            // Validate data size
            if (dataSize == 0) {
                throw std::runtime_error("Data chunk is empty");
            }
            
            if (dataSize % wavHeader.blockAlign != 0) {
                throw std::runtime_error(
                    "Data size " + std::to_string(dataSize) + 
                    " is not a multiple of block align " + std::to_string(wavHeader.blockAlign)
                );
            }
            
            // Check if decoded samples would exceed memory limit (1 GB for float vectors)
            uint32_t numSamples = dataSize / wavHeader.blockAlign;
            constexpr uint32_t MAX_SAMPLES = 1024 * 1024 * 256;  // 256M samples = 1GB of float[]
            if (numSamples > MAX_SAMPLES) {
                throw std::runtime_error(
                    "Audio too long: " + std::to_string(numSamples) + 
                    " samples exceeds limit of " + std::to_string(MAX_SAMPLES)
                );
            }
            
            break;  // Found data chunk, exit loop
        }
        
        // Skip to next chunk
        file.seekg(chunkStart + chunkSize);
    }
    
    // ============ FINAL VALIDATION ============
    if (!foundFmt) {
        throw std::runtime_error("No format chunk found in WAV file");
    }
    if (!foundData) {
        throw std::runtime_error("No data chunk found in WAV file");
    }
    
    // ============ READ AUDIO DATA ============
    file.seekg(dataOffset);
    uint32_t numSamples = dataSize / wavHeader.blockAlign;
    
    result.sampleRate = wavHeader.sampleRate;
    result.channels = wavHeader.numChannels;
    result.bitDepth = wavHeader.bitsPerSample;
    result.numFrames = numSamples / wavHeader.numChannels;
    
    std::vector<float> rawSamples;
    rawSamples.reserve(numSamples);
    
    if (wavHeader.bitsPerSample == 16) {
        // ============ 16-BIT PCM ============
        std::vector<int16_t> buffer(numSamples);
        file.read(reinterpret_cast<char*>(buffer.data()), dataSize);
        if (file.gcount() != static_cast<std::streamsize>(dataSize)) {
            throw std::runtime_error(
                "File truncated: expected " + std::to_string(dataSize) + 
                " bytes but read " + std::to_string(file.gcount())
            );
        }
        for (int16_t sample : buffer) {
            rawSamples.push_back(static_cast<float>(sample) / 32768.f);
        }
    } 
    else if (wavHeader.bitsPerSample == 24) {
        // ============ 24-BIT PCM ============
        std::vector<uint8_t> buffer(dataSize);
        file.read(reinterpret_cast<char*>(buffer.data()), dataSize);
        if (file.gcount() != static_cast<std::streamsize>(dataSize)) {
            throw std::runtime_error("File truncated reading 24-bit data");
        }
        
        for (size_t i = 0; i + 3 <= dataSize; i += 3) {
            // Read little-endian 24-bit signed integer
            int32_t sample = (static_cast<int32_t>(buffer[i + 2]) << 16) |
                           (static_cast<int32_t>(buffer[i + 1]) << 8) |
                           static_cast<int32_t>(buffer[i]);
            // Sign-extend from 24 to 32 bits
            if (sample & 0x800000) sample |= 0xFF000000;
            rawSamples.push_back(static_cast<float>(sample) / 8388608.f);
        }
    } 
    else if (wavHeader.bitsPerSample == 32) {
        // ============ 32-BIT PCM ============
        std::vector<float> buffer(numSamples);
        file.read(reinterpret_cast<char*>(buffer.data()), dataSize);
        if (file.gcount() != static_cast<std::streamsize>(dataSize)) {
            throw std::runtime_error("File truncated reading 32-bit data");
        }
        rawSamples = buffer;
    }
    
    // ============ POPULATE CHANNEL SAMPLES FOR STEREO/MONO ============
    result.samples = rawSamples;  // Keep interleaved samples for compatibility
    
    if (wavHeader.numChannels > 1) {
        // Deinterleave stereo samples into separate channels for processing
        result.channelSamples.resize(wavHeader.numChannels);
        for (uint16_t ch = 0; ch < wavHeader.numChannels; ++ch) {
            result.channelSamples[ch].reserve(result.numFrames);
            for (size_t frame = 0; frame < result.numFrames; ++frame) {
                result.channelSamples[ch].push_back(rawSamples[frame * wavHeader.numChannels + ch]);
            }
        }
        // For analysis: compute mid-channel (L+R)/2 if stereo
        result.samples.resize(result.numFrames);
        for (size_t i = 0; i < result.numFrames; ++i) {
            result.samples[i] = (result.channelSamples[0][i] + result.channelSamples[1][i]) / 2.f;
        }
    } else {
        // Mono: wrap single channel
        result.channelSamples.resize(1);
        result.channelSamples[0] = rawSamples;
    }
    
    normalize(result.samples);
    
    return result;
}

bool AudioLoader::saveWAV(const std::string& filepath, const AudioBuffer& buffer) {
    try {
        // ============ PRE-VALIDATION ============
        if (buffer.samples.empty()) {
            std::cerr << "Error: Cannot save WAV - buffer is empty\n";
            return false;
        }
        
        // Validate buffer parameters
        if (buffer.sampleRate < 8000 || buffer.sampleRate > 192000) {
            std::cerr << "Error: Invalid sample rate " << buffer.sampleRate << " Hz\n";
            return false;
        }
        
        if (buffer.bitDepth != 16 && buffer.bitDepth != 24 && buffer.bitDepth != 32) {
            std::cerr << "Error: Unsupported bit depth " << buffer.bitDepth << "\n";
            return false;
        }
        
        // ============ CREATE FILE ============
        std::ofstream file(filepath, std::ios::binary);
        if (!file.is_open()) {
            std::cerr << "Error: Cannot create file: " << filepath << "\n";
            return false;
        }
        
        // ============ CALCULATE SIZES ============
        // Determine channel count and frame count from buffer
        uint16_t numChannels = buffer.channels > 0 ? buffer.channels : 1;
        uint32_t numFrames = buffer.numFrames > 0 ? buffer.numFrames : (buffer.samples.size() / numChannels);
        uint32_t numSamples = numFrames * numChannels;
        uint16_t bytesPerSample = buffer.bitDepth / 8;
        uint16_t blockAlign = numChannels * bytesPerSample;
        uint32_t dataSize = numSamples * bytesPerSample;

        // Check for overflow: file size shouldn't exceed 4GB
        constexpr uint64_t MAX_RIFF_SIZE = 0xFFFFFFFFULL - 8;
        if (dataSize > MAX_RIFF_SIZE - 36) {
            std::cerr << "Error: Audio too long for WAV format (max 4GB)\n";
            file.close();
            return false;
        }
        
        // ============ WRITE RIFF HEADER ============
        file.write("RIFF", 4);
        uint32_t riffSize = 36 + dataSize;
        file.write(reinterpret_cast<const char*>(&riffSize), 4);
        file.write("WAVE", 4);
        
        if (!file.good()) {
            std::cerr << "Error: Failed to write RIFF header\n";
            file.close();
            return false;
        }
        
        // ============ WRITE FORMAT CHUNK ============
        file.write("fmt ", 4);
        uint32_t fmtSize = 16;
        file.write(reinterpret_cast<const char*>(&fmtSize), 4);
        
        uint16_t audioFormat = 1;      // PCM
        uint32_t byteRate = buffer.sampleRate * blockAlign;

        file.write(reinterpret_cast<const char*>(&audioFormat), 2);
        file.write(reinterpret_cast<const char*>(&numChannels), 2);
        file.write(reinterpret_cast<const char*>(&buffer.sampleRate), 4);
        file.write(reinterpret_cast<const char*>(&byteRate), 4);
        file.write(reinterpret_cast<const char*>(&blockAlign), 2);
        file.write(reinterpret_cast<const char*>(&buffer.bitDepth), 2);
        
        if (!file.good()) {
            std::cerr << "Error: Failed to write format chunk\n";
            file.close();
            return false;
        }
        
        // ============ WRITE DATA CHUNK ============
        file.write("data", 4);
        file.write(reinterpret_cast<const char*>(&dataSize), 4);
        
        if (!file.good()) {
            std::cerr << "Error: Failed to write data chunk header\n";
            file.close();
            return false;
        }
        
        // ============ WRITE AUDIO DATA ============
        bool useChannelSamples = !buffer.channelSamples.empty();

        if (useChannelSamples) {
            // Write interleaved multi-channel data from channelSamples
            for (uint32_t frame = 0; frame < numFrames; ++frame) {
                for (uint16_t ch = 0; ch < numChannels; ++ch) {
                    if (ch >= buffer.channelSamples.size() || frame >= buffer.channelSamples[ch].size()) {
                        std::cerr << "Error: Channel or frame index out of bounds\n";
                        file.close();
                        return false;
                    }

                    float sample = buffer.channelSamples[ch][frame];
                    float clamped = std::max(-1.f, std::min(1.f, sample));

                    if (buffer.bitDepth == 16) {
                        int16_t pcmSample = static_cast<int16_t>(clamped * 32767.f);
                        file.write(reinterpret_cast<const char*>(&pcmSample), 2);
                    } else if (buffer.bitDepth == 24) {
                        int32_t pcm32 = static_cast<int32_t>(clamped * 8388607.f);
                        uint8_t byte1 = static_cast<uint8_t>(pcm32 & 0xFF);
                        uint8_t byte2 = static_cast<uint8_t>((pcm32 >> 8) & 0xFF);
                        uint8_t byte3 = static_cast<uint8_t>((pcm32 >> 16) & 0xFF);
                        file.write(reinterpret_cast<const char*>(&byte1), 1);
                        file.write(reinterpret_cast<const char*>(&byte2), 1);
                        file.write(reinterpret_cast<const char*>(&byte3), 1);
                    } else if (buffer.bitDepth == 32) {
                        file.write(reinterpret_cast<const char*>(&clamped), 4);
                    }

                    if (!file.good()) {
                        std::cerr << "Error: Write failed at frame " << frame << " channel " << ch << " (disk full?)\n";
                        file.close();
                        return false;
                    }
                }
            }
        } else {
            // Write interleaved flat samples
            for (uint32_t frame = 0; frame < numFrames; ++frame) {
                for (uint16_t ch = 0; ch < numChannels; ++ch) {
                    size_t sampleIdx = frame * numChannels + ch;
                    if (sampleIdx >= buffer.samples.size()) {
                        std::cerr << "Error: Sample index out of bounds at frame " << frame << " channel " << ch << "\n";
                        file.close();
                        return false;
                    }

                    float sample = buffer.samples[sampleIdx];
                    float clamped = std::max(-1.f, std::min(1.f, sample));

                    if (buffer.bitDepth == 16) {
                        int16_t pcmSample = static_cast<int16_t>(clamped * 32767.f);
                        file.write(reinterpret_cast<const char*>(&pcmSample), 2);
                    } else if (buffer.bitDepth == 24) {
                        int32_t pcm32 = static_cast<int32_t>(clamped * 8388607.f);
                        uint8_t byte1 = static_cast<uint8_t>(pcm32 & 0xFF);
                        uint8_t byte2 = static_cast<uint8_t>((pcm32 >> 8) & 0xFF);
                        uint8_t byte3 = static_cast<uint8_t>((pcm32 >> 16) & 0xFF);
                        file.write(reinterpret_cast<const char*>(&byte1), 1);
                        file.write(reinterpret_cast<const char*>(&byte2), 1);
                        file.write(reinterpret_cast<const char*>(&byte3), 1);
                    } else if (buffer.bitDepth == 32) {
                        file.write(reinterpret_cast<const char*>(&clamped), 4);
                    }

                    if (!file.good()) {
                        std::cerr << "Error: Write failed at frame " << frame << " channel " << ch << " (disk full?)\n";
                        file.close();
                        return false;
                    }
                }
            }
        }
        
        file.close();
        
        // ============ VERIFY OUTPUT ============
        if (!file.good()) {
            std::cerr << "Error: File close failed\n";
            return false;
        }
        
        // Verify file was created with expected size
        std::ifstream verify(filepath, std::ios::binary | std::ios::ate);
        uint32_t createdSize = static_cast<uint32_t>(verify.tellg());
        verify.close();
        
        uint32_t expectedSize = 44 + dataSize;  // RIFF(12) + fmt(24) + data(8) + samples
        if (createdSize != expectedSize) {
            std::cerr << "Error: File size mismatch - expected " << expectedSize 
                      << " bytes, got " << createdSize << "\n";
            return false;
        }
        
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Error saving WAV: " << e.what() << "\n";
        return false;
    }
}

std::vector<float> AudioLoader::stereoToMono(const std::vector<float>& stereoSamples, uint16_t channels) {
    if (channels == 0) {
        throw std::runtime_error("Invalid channel count: channels must be > 0");
    }
    if (channels == 1) {
        return stereoSamples;
    }
    
    std::vector<float> monoSamples;
    monoSamples.reserve(stereoSamples.size() / channels);
    
    for (size_t i = 0; i < stereoSamples.size(); i += channels) {
        float sum = 0.f;
        for (uint16_t ch = 0; ch < channels; ++ch) {
            if (i + ch < stereoSamples.size()) {
                sum += stereoSamples[i + ch];
            }
        }
        monoSamples.push_back(sum / channels);
    }
    
    return monoSamples;
}

void AudioLoader::normalize(std::vector<float>& samples) {
    if (samples.empty()) return;
    
    float maxAbs = 0.f;
    for (float sample : samples) {
        maxAbs = std::max(maxAbs, std::abs(sample));
    }
    
    // Only soft-limit if peaks exceed 1.0
    // Don't scale down quiet audio
    if (maxAbs > 1.f) {
        float targetLevel = 0.9f;  // -1dB headroom
        float scale = targetLevel / maxAbs;
        for (float& sample : samples) {
            sample *= scale;
        }
    }
}

} // namespace mastered
