#include "presets.h"

#include <iomanip>
#include <sstream>

namespace mastered {

namespace {

const Preset kPresets[] = {
    {"spotify", "Spotify", "Streaming", -14.0, -1.0, "Normalises to about -14 LUFS"},
    {"youtube", "YouTube", "Streaming", -14.0, -1.0, "Turns loud uploads down to about -14 LUFS"},
    {"apple-music", "Apple Music", "Streaming", -16.0, -1.0, "Sound Check aims for about -16 LUFS"},
    {"amazon-music", "Amazon Music", "Streaming", -14.0, -2.0, "About -14 LUFS; -2 dBTP leaves room for codec overs"},
    {"tidal", "Tidal", "Streaming", -14.0, -1.0, "About -14 LUFS"},
    {"deezer", "Deezer", "Streaming", -15.0, -1.0, "About -15 LUFS"},
    {"soundcloud", "SoundCloud", "Streaming", -14.0, -1.0, "A safe level; the service's normalisation rules have changed over time"},
    {"podcast", "Podcast", "Spoken word", -16.0, -1.0, "-16 LUFS is common for stereo podcasts"},
    {"ebu-r128", "EBU R128 broadcast", "Broadcast", -23.0, -1.0, "European broadcast, -23 LUFS"},
    {"atsc-a85", "ATSC A/85 broadcast", "Broadcast", -24.0, -2.0, "North American broadcast, -24 LKFS"},
    {"loud", "Loud master (CD, club)", "Other", -9.0, -0.3, "Dense and loud; streaming services will turn it down"},
};

constexpr size_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

std::string escape(const std::string& text) {
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

}

size_t presetCount() {
    return kPresetCount;
}

const Preset& presetAt(size_t index) {
    return kPresets[index < kPresetCount ? index : kPresetCount - 1];
}

const Preset* findPreset(const std::string& id) {
    for (const Preset& preset : kPresets) {
        if (id == preset.id) {
            return &preset;
        }
    }
    return nullptr;
}

std::string presetsJson() {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << "[";
    for (size_t i = 0; i < kPresetCount; ++i) {
        const Preset& p = kPresets[i];
        out << (i > 0 ? ", " : "") << "{\"id\": \"" << escape(p.id) << "\", \"name\": \"" << escape(p.name) << "\", \"group\": \""
            << escape(p.group) << "\", \"targetLufs\": " << p.targetLufs << ", \"ceilingDb\": " << p.ceilingDb << ", \"note\": \""
            << escape(p.note) << "\"}";
    }
    out << "]";
    return out.str();
}

}
