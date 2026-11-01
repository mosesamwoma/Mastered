#pragma once

#include <cstddef>
#include <string>

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

}
