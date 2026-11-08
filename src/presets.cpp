#include "presets.h"

#include <cmath>
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
    {"codec-safe", "Codec-safe (MP3 / AAC delivery)", "Other", -14.0, -2.0, "-2 dBTP leaves room for the overs that lossy encoding adds"},
    {"loud", "Loud master (CD, club)", "Other", -9.0, -0.3, "Dense and loud; streaming services will turn it down"},
    {"club-dj", "Club and DJ", "Other", -8.0, -0.5, "Loud and dense for DJ sets; leave a little ceiling for the mixer"},
    {"vinyl-premaster", "Vinyl premaster", "Other", -16.0, -3.0, "Quieter and with more headroom; cutting engineers need dynamics and low peaks"},
    {"social-video", "Social video (Instagram, TikTok)", "Streaming", -14.0, -1.0, "About -14 LUFS; phone speakers reward a clear midrange"},
};

constexpr size_t kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

struct Bump {
    double centreHz;
    double gainDb;
    double widthOctaves;
};

struct GenreCurve {
    Genre info;
    double slopeDbPerOctave;
    Bump bumps[3];
};

const GenreCurve kGenres[] = {
    {{"balanced", "Balanced", "A neutral, modern long-term spectrum"}, 4.5, {{0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}}},
    {{"bright-pop", "Bright pop", "Open top end and clear vocals"}, 3.9, {{9000.0, 1.5, 1.5}, {3000.0, 1.0, 1.2}, {0.0, 0.0, 1.0}}},
    {{"warm", "Warm and smooth", "Softer top, fuller low mids"}, 5.3, {{250.0, 1.0, 1.5}, {0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}}},
    {{"hiphop-edm", "Hip-hop and electronic", "Strong sub and bass with a lively top"}, 4.6, {{60.0, 4.0, 1.2}, {10000.0, 1.5, 1.5}, {0.0, 0.0, 1.0}}},
    {{"rock", "Rock and guitar music", "Forward upper mids with solid low end"}, 4.2, {{3000.0, 1.5, 1.2}, {100.0, 1.0, 1.0}, {0.0, 0.0, 1.0}}},
    {{"acoustic", "Acoustic and folk", "Natural balance with a little air"}, 4.4, {{50.0, -2.0, 1.5}, {5000.0, 1.0, 1.5}, {0.0, 0.0, 1.0}}},
    {{"spoken", "Spoken word and podcast", "Clear speech, controlled low end"}, 5.5, {{3000.0, 2.0, 1.5}, {60.0, -4.0, 1.5}, {0.0, 0.0, 1.0}}},
};

constexpr size_t kGenreCount = sizeof(kGenres) / sizeof(kGenres[0]);

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

size_t genreCount() {
    return kGenreCount;
}

const Genre& genreAt(size_t index) {
    return kGenres[index < kGenreCount ? index : kGenreCount - 1].info;
}

const Genre* findGenre(const std::string& id) {
    for (const GenreCurve& genre : kGenres) {
        if (id == genre.info.id) {
            return &genre.info;
        }
    }
    return nullptr;
}

std::string genresJson() {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < kGenreCount; ++i) {
        const Genre& g = kGenres[i].info;
        out << (i > 0 ? ", " : "") << "{\"id\": \"" << escape(g.id) << "\", \"name\": \"" << escape(g.name) << "\", \"note\": \""
            << escape(g.note) << "\"}";
    }
    out << "]";
    return out.str();
}

std::vector<double> genreLevelsDb(const std::string& id, const std::vector<double>& centresHz) {
    std::vector<double> levels(centresHz.size(), 0.0);
    for (const GenreCurve& genre : kGenres) {
        if (id != genre.info.id) {
            continue;
        }
        for (size_t i = 0; i < centresHz.size(); ++i) {
            const double octaves = std::log2(centresHz[i] / 1000.0);
            double value = -genre.slopeDbPerOctave * octaves;
            for (const Bump& bump : genre.bumps) {
                if (bump.gainDb == 0.0) {
                    continue;
                }
                const double distance = std::log2(centresHz[i] / bump.centreHz) / bump.widthOctaves;
                value += bump.gainDb * std::exp(-0.5 * distance * distance);
            }
            levels[i] = value;
        }
        break;
    }
    return levels;
}

}
