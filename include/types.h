#pragma once

#include <cstddef>
#include <vector>

namespace mastered {

using AudioBlock = std::vector<std::vector<double>>;

enum class BandType { Peak, LowShelf, HighShelf };

struct EQBand {
    double frequency = 1000.0;
    double gainDb = 0.0;
    double q = 1.0;
    BandType type = BandType::Peak;
};

inline const char* bandTypeName(BandType type) {
    switch (type) {
        case BandType::LowShelf: return "shelf-low";
        case BandType::HighShelf: return "shelf-high";
        default: return "peak";
    }
}

inline AudioBlock makeBlock(size_t channels, size_t frames) {
    return AudioBlock(channels, std::vector<double>(frames, 0.0));
}

}
