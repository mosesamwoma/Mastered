#include "audio_loader.h"
#include <fstream>
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <cmath>

namespace mastered {

AudioBuffer AudioLoader::loadWAV(const std::string& filepath) {
    AudioBuffer result;
    std::ifstream file(filepath, std::ios::binary);
    
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filepath);
    }
    
    char riffHeader[4];
    file.read(riffHeader, 4);
    if (std::string(riffHeader, 4) != "RIFF") {
        throw std::runtime_error("Not a valid WAV file: missing RIFF header");
    }
    
    uint32_t riffSize;
    file.read(reinterpret_cast<char*>(&riffSize), 4);
    
    char waveHeader[4];
    file.read(waveHeader, 4);
    if (std::string(waveHeader, 4) != "WAVE") {
        throw std::runtime_error("Not a valid WAV file: missing WAVE header");
    }
    
    char chunkID[4];
    uint32_t chunkSize;
    WAVHeader wavHeader = {};
    uint32_t dataSize = 0;
    uint32_t dataOffset = 0;
    
    while (file.read(chunkID, 4)) {
        file.read(reinterpret_cast<char*>(&chunkSize), 4);
        uint32_t chunkStart = file.tellg();
        
        if (std::string(chunkID, 4) == "fmt ") {
            file.read(reinterpret_cast<char*>(&wavHeader.audioFormat), 2);
            file.read(reinterpret_cast<char*>(&wavHeader.numChannels), 2);
            file.read(reinterpret_cast<char*>(&wavHeader.sampleRate), 4);
            file.read(reinterpret_cast<char*>(&wavHeader.byteRate), 4);
            file.read(reinterpret_cast<char*>(&wavHeader.blockAlign), 2);
            file.read(reinterpret_cast<char*>(&wavHeader.bitsPerSample), 2);
            
            if (wavHeader.audioFormat != 1) {
                throw std::runtime_error("Only PCM audio format is supported");
            }
        } else if (std::string(chunkID, 4) == "data") {
            dataSize = chunkSize;
            dataOffset = file.tellg();
            break;
        }
        
        file.seekg(chunkStart + chunkSize);
    }
    
    if (dataSize == 0) {
        throw std::runtime_error("No audio data found in WAV file");
    }
    
    // Read audio data
    file.seekg(dataOffset);
    uint32_t numSamples = dataSize / wavHeader.blockAlign;
    
    result.sampleRate = wavHeader.sampleRate;
    result.channels = wavHeader.numChannels;
    result.numFrames = numSamples / wavHeader.numChannels;
    
    std::vector<float> rawSamples;
    rawSamples.reserve(numSamples);
    
    if (wavHeader.bitsPerSample == 16) {
        std::vector<int16_t> buffer(numSamples);
        file.read(reinterpret_cast<char*>(buffer.data()), dataSize);
        for (int16_t sample : buffer) {
            rawSamples.push_back(static_cast<float>(sample) / 32768.f);
        }
    } else if (wavHeader.bitsPerSample == 24) {
        std::vector<uint8_t> buffer(dataSize);
        file.read(reinterpret_cast<char*>(buffer.data()), dataSize);
        for (size_t i = 0; i + 2 < dataSize; i += 3) {
            int32_t sample = (static_cast<int32_t>(buffer[i + 2]) << 16) |
                           (static_cast<int32_t>(buffer[i + 1]) << 8) |
                           static_cast<int32_t>(buffer[i]);
            if (sample & 0x800000) sample |= 0xFF000000;
            rawSamples.push_back(static_cast<float>(sample) / 8388608.f);
        }
    } else if (wavHeader.bitsPerSample == 32) {
        std::vector<float> buffer(numSamples);
        file.read(reinterpret_cast<char*>(buffer.data()), dataSize);
        rawSamples = buffer;
    } else {
        throw std::runtime_error("Unsupported bit depth: " + std::to_string(wavHeader.bitsPerSample));
    }
    
    if (wavHeader.numChannels > 1) {
        result.samples = stereoToMono(rawSamples, wavHeader.numChannels);
        result.numFrames = result.samples.size();
    } else {
        result.samples = rawSamples;
    }
    
    normalize(result.samples);
    
    return result;
}

bool AudioLoader::saveWAV(const std::string& filepath, const AudioBuffer& buffer) {
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    
    uint32_t numSamples = buffer.samples.size();
    uint32_t byteRate = buffer.sampleRate * 2;
    uint32_t dataSize = numSamples * 2;
    
    file.write("RIFF", 4);
    uint32_t riffSize = 36 + dataSize;
    file.write(reinterpret_cast<const char*>(&riffSize), 4);
    file.write("WAVE", 4);
    
    file.write("fmt ", 4);
    uint32_t fmtSize = 16;
    file.write(reinterpret_cast<const char*>(&fmtSize), 4);
    
    uint16_t audioFormat = 1;
    uint16_t channels = 1;
    file.write(reinterpret_cast<const char*>(&audioFormat), 2);
    file.write(reinterpret_cast<const char*>(&channels), 2);
    file.write(reinterpret_cast<const char*>(&buffer.sampleRate), 4);
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    
    uint16_t blockAlign = 2;
    uint16_t bitsPerSample = 16;
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataSize), 4);
    
    for (float sample : buffer.samples) {
        float clamped = std::max(-1.f, std::min(1.f, sample));
        int16_t pcmSample = static_cast<int16_t>(clamped * 32767.f);
        file.write(reinterpret_cast<const char*>(&pcmSample), 2);
    }
    
    file.close();
    return true;
}

std::vector<float> AudioLoader::stereoToMono(const std::vector<float>& stereoSamples, uint16_t channels) {
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
    
    if (maxAbs > 1.f) {
        float scale = 1.f / maxAbs;
        for (float& sample : samples) {
            sample *= scale;
        }
    }
}

} // namespace mastered
