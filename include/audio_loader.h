#ifndef AUDIO_LOADER_H
#define AUDIO_LOADER_H

#include <vector>
#include <string>
#include <cstdint>

namespace mastered {

struct AudioBuffer {
    std::vector<float> samples;              // Flat samples (mono or interleaved stereo)
    std::vector<std::vector<float>> channelSamples;  // Per-channel samples for stereo
    uint32_t sampleRate;
    uint16_t channels;
    uint32_t numFrames;
    uint16_t bitDepth = 16;  // Track original bit depth for lossless saving
    
    AudioBuffer() : sampleRate(0), channels(0), numFrames(0) {}
};

class AudioLoader {
public:
    AudioLoader() = default;
    ~AudioLoader() = default;
    
    /**
     * Load WAV file and return audio buffer
     * Converts to mono if stereo, handles 16/24/32-bit PCM
     */
    static AudioBuffer loadWAV(const std::string& filepath);
    
    /**
     * Save audio buffer as WAV file
     */
    static bool saveWAV(const std::string& filepath, const AudioBuffer& buffer);
    
    /**
     * Convert stereo to mono by averaging channels
     */
    static std::vector<float> stereoToMono(const std::vector<float>& stereoSamples, uint16_t channels);
    
    /**
     * Normalize audio to -1.0 to 1.0 range
     */
    static void normalize(std::vector<float>& samples);
    
private:
    // WAV file header parsing
    struct WAVHeader {
        uint32_t riffSize;
        uint16_t audioFormat;
        uint16_t numChannels;
        uint32_t sampleRate;
        uint32_t byteRate;
        uint16_t blockAlign;
        uint16_t bitsPerSample;
    };
    
    static WAVHeader parseWAVHeader(const std::string& filepath);
};

} // namespace mastered

#endif // AUDIO_LOADER_H
