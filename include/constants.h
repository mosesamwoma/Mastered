#pragma once

#include <cstddef>
#include <cstdint>

#ifndef MASTERED_VERSION
#define MASTERED_VERSION "2.0.0"
#endif

namespace mastered {

constexpr const char* kVersion = MASTERED_VERSION;
constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kMinSampleRate = 8000;
constexpr uint32_t kMaxSampleRate = 192000;
constexpr uint16_t kMaxChannels = 8;
constexpr size_t kBlockFrames = 4096;
constexpr double kSilenceLufs = -120.0;
constexpr double kLimiterMarginDb = 0.3;

}
