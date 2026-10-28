#include "wav_io.h"

#include "constants.h"
#include "error.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace mastered {

namespace {

constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatFloat = 3;
constexpr uint16_t kFormatExtensible = 0xFFFE;

uint16_t readU16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t readU32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t readU64(const unsigned char* p) {
    return static_cast<uint64_t>(readU32(p)) | (static_cast<uint64_t>(readU32(p + 4)) << 32);
}

void putU16(std::vector<unsigned char>& out, uint16_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xFF));
    out.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
}

void putU32(std::vector<unsigned char>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<unsigned char>((v >> (8 * i)) & 0xFF));
    }
}

void putTag(std::vector<unsigned char>& out, const char* tag) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<unsigned char>(tag[i]));
    }
}

uint32_t channelMask(uint16_t channels) {
    switch (channels) {
        case 3: return 0x7;
        case 4: return 0x33;
        case 5: return 0x37;
        case 6: return 0x3F;
        case 7: return 0x70F;
        case 8: return 0x63F;
        default: return 0x3;
    }
}

}

WavReader::WavReader(const std::string& path) : path_(path) {
    std::error_code ec;
    const uint64_t fileSize = static_cast<uint64_t>(std::filesystem::file_size(path, ec));
    if (ec) {
        throw MasteredError("Cannot open file: " + path);
    }
    file_.open(path, std::ios::binary);
    if (!file_.is_open()) {
        throw MasteredError("Cannot open file: " + path);
    }
    unsigned char header[12];
    readExact(header, 12, "RIFF header");
    if (std::memcmp(header, "RIFF", 4) != 0 || std::memcmp(header + 8, "WAVE", 4) != 0) {
        throw MasteredError(path + ": not a RIFF/WAVE file");
    }

    bool foundFormat = false;
    bool foundData = false;
    uint16_t tag = 0;
    uint64_t dataBytes = 0;
    uint64_t position = 12;
    while (position + 8 <= fileSize && !(foundFormat && foundData)) {
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(position));
        unsigned char chunkHeader[8];
        readExact(chunkHeader, 8, "chunk header");
        const uint64_t chunkSize = readU32(chunkHeader + 4);
        const uint64_t payload = position + 8;
        if (std::memcmp(chunkHeader, "fmt ", 4) == 0 && !foundFormat) {
            if (chunkSize < 16 || payload + 16 > fileSize) {
                throw MasteredError(path + ": invalid fmt chunk");
            }
            const size_t toRead = static_cast<size_t>(std::min<uint64_t>(chunkSize, 40));
            std::vector<unsigned char> fmt(toRead);
            readExact(fmt.data(), toRead, "fmt chunk");
            tag = readU16(fmt.data());
            channels_ = readU16(fmt.data() + 2);
            sampleRate_ = readU32(fmt.data() + 4);
            blockAlign_ = readU16(fmt.data() + 12);
            bitDepth_ = readU16(fmt.data() + 14);
            if (tag == kFormatExtensible) {
                if (toRead < 26) {
                    throw MasteredError(path + ": invalid extensible fmt chunk");
                }
                tag = readU16(fmt.data() + 24);
            }
            foundFormat = true;
        } else if (std::memcmp(chunkHeader, "data", 4) == 0 && !foundData) {
            dataOffset_ = payload;
            dataBytes = chunkSize;
            foundData = true;
        }
        position = payload + chunkSize + (chunkSize & 1);
    }
    if (!foundFormat) {
        throw MasteredError(path + ": no fmt chunk found");
    }
    if (!foundData) {
        throw MasteredError(path + ": no data chunk found");
    }
    if (tag == kFormatPcm) {
        if (bitDepth_ != 16 && bitDepth_ != 24 && bitDepth_ != 32) {
            throw MasteredError(path + ": unsupported PCM bit depth " + std::to_string(bitDepth_));
        }
        isFloat_ = false;
    } else if (tag == kFormatFloat) {
        if (bitDepth_ != 32 && bitDepth_ != 64) {
            throw MasteredError(path + ": unsupported float bit depth " + std::to_string(bitDepth_));
        }
        isFloat_ = true;
    } else {
        throw MasteredError(path + ": unsupported audio format tag " + std::to_string(tag));
    }
    if (channels_ < 1 || channels_ > kMaxChannels) {
        throw MasteredError(path + ": unsupported channel count " + std::to_string(channels_));
    }
    if (sampleRate_ < kMinSampleRate || sampleRate_ > kMaxSampleRate) {
        throw MasteredError(path + ": unsupported sample rate " + std::to_string(sampleRate_));
    }
    if (blockAlign_ != channels_ * (bitDepth_ / 8)) {
        throw MasteredError(path + ": inconsistent block alignment");
    }
    const uint64_t available = fileSize > dataOffset_ ? fileSize - dataOffset_ : 0;
    dataBytes = std::min(dataBytes, available);
    totalFrames_ = dataBytes / blockAlign_;
    if (totalFrames_ == 0) {
        throw MasteredError(path + ": file contains no audio");
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(dataOffset_));
}

void WavReader::readExact(unsigned char* destination, size_t count, const char* what) {
    file_.read(reinterpret_cast<char*>(destination), static_cast<std::streamsize>(count));
    if (static_cast<size_t>(file_.gcount()) != count) {
        throw MasteredError(path_ + ": unexpected end of file while reading " + what);
    }
}

void WavReader::rewind() {
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(dataOffset_));
    framesRead_ = 0;
}

size_t WavReader::read(AudioBlock& block, size_t maxFrames) {
    const uint64_t remaining = totalFrames_ - framesRead_;
    const size_t want = static_cast<size_t>(std::min<uint64_t>(remaining, maxFrames));
    block.resize(channels_);
    if (want == 0) {
        for (auto& channel : block) {
            channel.clear();
        }
        return 0;
    }
    buffer_.resize(want * blockAlign_);
    readExact(buffer_.data(), buffer_.size(), "audio data");
    for (auto& channel : block) {
        channel.resize(want);
    }
    const size_t bytes = bitDepth_ / 8;
    const unsigned char* p = buffer_.data();
    for (size_t i = 0; i < want; ++i) {
        for (size_t ch = 0; ch < channels_; ++ch) {
            double value;
            if (!isFloat_) {
                if (bitDepth_ == 16) {
                    value = static_cast<double>(static_cast<int16_t>(readU16(p))) / 32768.0;
                } else if (bitDepth_ == 24) {
                    int32_t v = static_cast<int32_t>(p[0]) | (static_cast<int32_t>(p[1]) << 8) | (static_cast<int32_t>(p[2]) << 16);
                    if (v & 0x800000) {
                        v -= 0x1000000;
                    }
                    value = static_cast<double>(v) / 8388608.0;
                } else {
                    value = static_cast<double>(static_cast<int32_t>(readU32(p))) / 2147483648.0;
                }
            } else if (bitDepth_ == 32) {
                const uint32_t raw = readU32(p);
                float f;
                std::memcpy(&f, &raw, sizeof(f));
                value = static_cast<double>(f);
            } else {
                const uint64_t raw = readU64(p);
                std::memcpy(&value, &raw, sizeof(value));
            }
            if (!std::isfinite(value)) {
                throw MasteredError(path_ + ": audio contains non-finite samples");
            }
            block[ch][i] = value;
            p += bytes;
        }
    }
    framesRead_ += want;
    return want;
}

WavWriter::WavWriter(const std::string& path, uint32_t sampleRate, uint16_t channels, uint16_t bitDepth, bool dither, bool noiseShaping)
    : path_(path),
      tempPath_(path + ".tmp"),
      sampleRate_(sampleRate),
      channels_(channels),
      bitDepth_(bitDepth),
      dither_(dither),
      noiseShaping_(noiseShaping),
      shapingError_(channels, 0.0) {
    if (bitDepth != 16 && bitDepth != 24 && bitDepth != 32) {
        throw MasteredError("Output bit depth must be 16, 24 or 32");
    }
    if (channels < 1 || channels > kMaxChannels) {
        throw MasteredError("Unsupported output channel count");
    }
    const std::filesystem::path parent = std::filesystem::path(path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            throw MasteredError("Cannot create output directory: " + parent.string());
        }
    }
    file_.open(tempPath_, std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) {
        throw MasteredError("Cannot create output file: " + path);
    }
    const std::vector<unsigned char> header = buildHeader(0);
    file_.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    if (!file_) {
        throw MasteredError("Failed to write output file: " + path);
    }
}

WavWriter::~WavWriter() {
    if (!finalized_) {
        if (file_.is_open()) {
            file_.close();
        }
        std::error_code ec;
        std::filesystem::remove(tempPath_, ec);
    }
}

size_t WavWriter::headerSize() const {
    const bool isFloat = bitDepth_ == 32;
    const bool extensible = channels_ > 2;
    size_t size = 12 + 8 + (extensible ? 40 : (isFloat ? 18 : 16)) + 8;
    if (isFloat) {
        size += 12;
    }
    return size;
}

std::vector<unsigned char> WavWriter::buildHeader(uint64_t dataBytes) const {
    const bool isFloat = bitDepth_ == 32;
    const bool extensible = channels_ > 2;
    const uint16_t blockAlign = static_cast<uint16_t>(channels_ * (bitDepth_ / 8));
    const uint16_t tag = isFloat ? kFormatFloat : kFormatPcm;
    const uint64_t padded = dataBytes + (dataBytes & 1);
    std::vector<unsigned char> out;
    out.reserve(headerSize());
    putTag(out, "RIFF");
    putU32(out, static_cast<uint32_t>(headerSize() - 8 + padded));
    putTag(out, "WAVE");
    putTag(out, "fmt ");
    putU32(out, extensible ? 40 : (isFloat ? 18 : 16));
    putU16(out, extensible ? kFormatExtensible : tag);
    putU16(out, channels_);
    putU32(out, sampleRate_);
    putU32(out, sampleRate_ * blockAlign);
    putU16(out, blockAlign);
    putU16(out, bitDepth_);
    if (extensible) {
        putU16(out, 22);
        putU16(out, bitDepth_);
        putU32(out, channelMask(channels_));
        putU16(out, tag);
        static const unsigned char guidTail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        out.insert(out.end(), guidTail, guidTail + 14);
    } else if (isFloat) {
        putU16(out, 0);
    }
    if (isFloat) {
        putTag(out, "fact");
        putU32(out, 4);
        putU32(out, static_cast<uint32_t>(framesWritten_));
    }
    putTag(out, "data");
    putU32(out, static_cast<uint32_t>(dataBytes));
    return out;
}

double WavWriter::uniform() {
    rngState_ ^= rngState_ >> 12;
    rngState_ ^= rngState_ << 25;
    rngState_ ^= rngState_ >> 27;
    const uint64_t r = rngState_ * 2685821657736338717ULL;
    return static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0);
}

double WavWriter::triangular() {
    return uniform() + uniform() - 1.0;
}

void WavWriter::write(const AudioBlock& block, size_t frames) {
    if (finalized_) {
        throw MasteredError("Output file already finalized");
    }
    if (frames == 0) {
        return;
    }
    const size_t bytes = bitDepth_ / 8;
    const uint64_t add = static_cast<uint64_t>(frames) * channels_ * bytes;
    if (headerSize() + dataBytes_ + add + 1 > 0xFFFFFFFFULL) {
        throw MasteredError("Output exceeds the 4 GB WAV size limit");
    }
    buffer_.resize(frames * channels_ * bytes);
    unsigned char* p = buffer_.data();
    const bool isFloat = bitDepth_ == 32;
    const double scale = bitDepth_ == 16 ? 32768.0 : 8388608.0;
    const double minValue = -scale;
    const double maxValue = scale - 1.0;
    for (size_t i = 0; i < frames; ++i) {
        for (size_t ch = 0; ch < channels_; ++ch) {
            double x = block[ch][i];
            if (!std::isfinite(x)) {
                x = 0.0;
            }
            if (isFloat) {
                const float f = static_cast<float>(x);
                uint32_t raw;
                std::memcpy(&raw, &f, sizeof(raw));
                p[0] = static_cast<unsigned char>(raw & 0xFF);
                p[1] = static_cast<unsigned char>((raw >> 8) & 0xFF);
                p[2] = static_cast<unsigned char>((raw >> 16) & 0xFF);
                p[3] = static_cast<unsigned char>((raw >> 24) & 0xFF);
            } else {
                const double v = x * scale - (noiseShaping_ ? shapingError_[ch] : 0.0);
                const double d = dither_ ? triangular() : 0.0;
                const double rounded = std::floor(v + d + 0.5);
                const double clamped = std::min(std::max(rounded, minValue), maxValue);
                shapingError_[ch] = (noiseShaping_ && clamped == rounded) ? rounded - v : 0.0;
                const int32_t q = static_cast<int32_t>(clamped);
                p[0] = static_cast<unsigned char>(q & 0xFF);
                p[1] = static_cast<unsigned char>((q >> 8) & 0xFF);
                if (bitDepth_ == 24) {
                    p[2] = static_cast<unsigned char>((q >> 16) & 0xFF);
                }
            }
            p += bytes;
        }
    }
    file_.write(reinterpret_cast<const char*>(buffer_.data()), static_cast<std::streamsize>(buffer_.size()));
    if (!file_) {
        throw MasteredError("Failed to write output file: " + path_);
    }
    dataBytes_ += add;
    framesWritten_ += frames;
}

void WavWriter::finalize() {
    if (finalized_) {
        return;
    }
    if (dataBytes_ & 1) {
        const char pad = 0;
        file_.write(&pad, 1);
    }
    const std::vector<unsigned char> header = buildHeader(dataBytes_);
    file_.seekp(0);
    file_.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    file_.flush();
    if (!file_) {
        throw MasteredError("Failed to finalize output file: " + path_);
    }
    file_.close();
    std::error_code ec;
    std::filesystem::rename(tempPath_, path_, ec);
    if (ec) {
        throw MasteredError("Cannot move output file into place: " + path_);
    }
    finalized_ = true;
}

}
