#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace mastered {

struct Preset {
    const char* id;
    const char* name;
    const char* group;
    double targetLufs;
    double ceilingDb;
    const char* note;
};

size_t presetCount();
const Preset& presetAt(size_t index);
const Preset* findPreset(const std::string& id);
std::string presetsJson();

struct Genre {
    const char* id;
    const char* name;
    const char* note;
};

size_t genreCount();
const Genre& genreAt(size_t index);
const Genre* findGenre(const std::string& id);
std::string genresJson();
std::vector<double> genreLevelsDb(const std::string& id, const std::vector<double>& centresHz);

}
