#pragma once

#include "types.h"

#include <cstddef>

namespace mastered {

struct BiquadCoefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

BiquadCoefficients designBand(const EQBand& band, double sampleRate);
BiquadCoefficients designButterworth(bool highPass, double frequency, double sampleRate);
double biquadResponseDb(const BiquadCoefficients& c, double cosW, double sinW, double cos2W, double sin2W);
double biquadMagnitudeDb(const BiquadCoefficients& c, double frequency, double sampleRate);

class Biquad {
public:
    Biquad() = default;
    explicit Biquad(const BiquadCoefficients& c) : c_(c) {}

    void setCoefficients(const BiquadCoefficients& c) { c_ = c; }

    void reset() {
        z1_ = 0.0;
        z2_ = 0.0;
    }

    double process(double x) {
        const double y = c_.b0 * x + z1_;
        z1_ = c_.b1 * x - c_.a1 * y + z2_;
        z2_ = c_.b2 * x - c_.a2 * y;
        return y;
    }

    void processBlock(double* data, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            data[i] = process(data[i]);
        }
    }

private:
    BiquadCoefficients c_;
    double z1_ = 0.0;
    double z2_ = 0.0;
};

}
