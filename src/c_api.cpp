#include "mastered_c.h"

#include "constants.h"
#include "mastering_engine.h"

#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <vector>

struct mastered_engine {
    mastered::MasteringConfig config;
    std::vector<std::string> references;
    mastered::MasteringResult result;
    bool hasResult = false;
    std::string error;
    std::string json;
};

namespace {

bool setOption(mastered::MasteringConfig& c, const std::string& key, double v) {
    const bool flag = v != 0.0;
    if (key == "target_lufs") c.targetLufs = v;
    else if (key == "aggressiveness") c.aggressiveness = v;
    else if (key == "ceiling_db") c.ceilingDb = v;
    else if (key == "max_bands") c.maxEQBands = v < 0.0 ? 0 : static_cast<size_t>(v);
    else if (key == "max_side_bands") c.maxSideBands = v < 0.0 ? 0 : static_cast<size_t>(v);
    else if (key == "smoothing") c.smoothing = flag;
    else if (key == "perceptual_weighting") c.perceptualWeighting = flag;
    else if (key == "auto_gain") c.autoGain = flag;
    else if (key == "mid_side") c.midSide = flag;
    else if (key == "match_width") c.matchWidth = flag;
    else if (key == "dynamics") c.dynamics = flag;
    else if (key == "dynamics_match") c.dynamicsMatch = flag;
    else if (key == "dynamics_ratio") c.dynamicsRatio = v;
    else if (key == "dynamics_threshold_offset_db") c.dynamicsThresholdOffsetDb = v;
    else if (key == "soft_clip") c.softClip = flag;
    else if (key == "soft_clip_knee_db") c.softClipKneeDb = v;
    else if (key == "accurate_peak") c.accuratePeak = flag;
    else if (key == "lookahead_ms") c.limiterLookaheadMs = v;
    else if (key == "release_ms") c.limiterReleaseMs = v;
    else if (key == "bit_depth") c.bitDepth = v < 0.0 ? 0 : static_cast<uint16_t>(v);
    else if (key == "dither") c.dither = flag;
    else if (key == "noise_shaping") c.noiseShaping = flag;
    else if (key == "tolerance_lu") c.loudnessToleranceLu = v;
    else if (key == "max_iterations") c.maxLoudnessIterations = static_cast<int>(v);
    else if (key == "fft_size") c.fftSize = v < 0.0 ? 0 : static_cast<size_t>(v);
    else return false;
    return true;
}

}

extern "C" {

const char* mastered_version(void) {
    return mastered::kVersion;
}

mastered_engine* mastered_create(void) {
    return new (std::nothrow) mastered_engine();
}

void mastered_destroy(mastered_engine* engine) {
    delete engine;
}

int mastered_set_option(mastered_engine* engine, const char* key, double value) {
    if (!engine || !key) {
        return 1;
    }
    if (!setOption(engine->config, key, value)) {
        engine->error = std::string("Unknown option: ") + key;
        return 2;
    }
    return 0;
}

int mastered_add_reference(mastered_engine* engine, const char* path) {
    if (!engine || !path) {
        return 1;
    }
    try {
        engine->references.emplace_back(path);
    } catch (const std::exception& e) {
        engine->error = e.what();
        return 3;
    }
    return 0;
}

void mastered_clear_references(mastered_engine* engine) {
    if (engine) {
        engine->references.clear();
    }
}

int mastered_run(mastered_engine* engine, const char* input_path, const char* output_path) {
    if (!engine || !input_path || !output_path) {
        return 1;
    }
    engine->hasResult = false;
    engine->error.clear();
    try {
        mastered::MasteringEngine core(engine->config);
        engine->result = core.run(engine->references, input_path, output_path);
        engine->json = mastered::MasteringEngine::exportJSON(engine->result);
        engine->hasResult = true;
    } catch (const std::exception& e) {
        engine->error = e.what();
        return 3;
    } catch (...) {
        engine->error = "Unknown error";
        return 3;
    }
    return 0;
}

int mastered_get_metric(mastered_engine* engine, const char* key, double* value) {
    if (!engine || !key || !value || !engine->hasResult) {
        return 1;
    }
    const mastered::MasteringResult& r = engine->result;
    const std::string k = key;
    if (k == "input_lufs") *value = r.analysis.input.integratedLufs;
    else if (k == "input_true_peak_db") *value = r.analysis.input.truePeakDb;
    else if (k == "input_lra") *value = r.analysis.input.loudnessRangeLu;
    else if (k == "reference_lufs") *value = r.analysis.reference.integratedLufs;
    else if (k == "output_lufs") *value = r.output.integratedLufs;
    else if (k == "output_true_peak_db") *value = r.output.truePeakDb;
    else if (k == "output_lra") *value = r.output.loudnessRangeLu;
    else if (k == "output_correlation") *value = r.output.correlation;
    else if (k == "makeup_gain_db") *value = r.makeupGainDb;
    else if (k == "iterations") *value = static_cast<double>(r.iterations);
    else if (k == "target_reached") *value = r.targetReached ? 1.0 : 0.0;
    else if (k == "mid_band_count") *value = static_cast<double>(r.analysis.mid.bands.size());
    else if (k == "side_band_count") *value = static_cast<double>(r.analysis.side.bands.size());
    else return 2;
    return 0;
}

const char* mastered_result_json(mastered_engine* engine) {
    if (!engine || !engine->hasResult) {
        return nullptr;
    }
    return engine->json.c_str();
}

const char* mastered_last_error(mastered_engine* engine) {
    if (!engine) {
        return "Invalid engine handle";
    }
    return engine->error.c_str();
}

}
