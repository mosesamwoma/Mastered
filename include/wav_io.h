#pragma once

#include "types.h"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace mastered {

class WavReader {
public:
    explicit WavReader(const std::string& path);

    uint32_t sampleRate() const { return sampleRate_; }
    uint16_t channels() const { return channels_; }
    uint16_t bitDepth() const { return bitDepth_; }
    bool isFloat() const { return isFloat_; }
    uint64_t frames() const { return totalFrames_; }

    size_t read(AudioBlock& block, size_t maxFrames);
    void rewind();

private:
    void readExact(unsigned char* destination, size_t count, const char* what);

    std::string path_;
    std::ifstream file_;
    uint32_t sampleRate_ = 0;
    uint16_t channels_ = 0;
    uint16_t bitDepth_ = 0;
    uint16_t blockAlign_ = 0;
    bool isFloat_ = false;
    uint64_t dataOffset_ = 0;
    uint64_t totalFrames_ = 0;
    uint64_t framesRead_ = 0;
    std::vector<unsigned char> buffer_;
};

class WavWriter {
public:
    WavWriter(const std::string& path, uint32_t sampleRate, uint16_t channels, uint16_t bitDepth, bool dither, bool noiseShaping,
              uint64_t sizeLimit = 0xFFFFFFFEULL);
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;
    ~WavWriter();

    void write(const AudioBlock& block, size_t frames);
    void finalize();

private:
    size_t headerSize() const;
    std::vector<unsigned char> buildHeader(uint64_t dataBytes) const;
    double uniform();
    double triangular();

    std::string path_;
    std::string tempPath_;
    std::ofstream file_;
    uint32_t sampleRate_;
    uint16_t channels_;
    uint16_t bitDepth_;
    bool dither_;
    bool noiseShaping_;
    uint64_t sizeLimit_;
    bool finalized_ = false;
    uint64_t dataBytes_ = 0;
    uint64_t framesWritten_ = 0;
    uint64_t rngState_ = 0x9E3779B97F4A7C15ULL;
    std::vector<double> shapingError_;
    std::vector<unsigned char> buffer_;
};

}
