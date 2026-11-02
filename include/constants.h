#pragma once

#include <cstddef>
#include <cstdint>

#ifndef MASTERED_VERSION
#define MASTERED_VERSION "2.3.1"
#endif

namespace mastered {

constexpr const char* kVersion = MASTERED_VERSION;
constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kMinSampleRate = 8000;
constexpr uint32_t kMaxSampleRate = 192000;
constexpr uint16_t kMaxChannels = 8;
constexpr size_t kBlockFrames = 4096;
constexpr double kSilenceLufs = -120.0;
// Headroom the limiter keeps below the requested ceiling. The closed-loop ceiling check in the engine then
// trims the result so that the measured true peak lands just under the ceiling the caller asked for.
constexpr double kLimiterMarginDb = 0.1;

}
