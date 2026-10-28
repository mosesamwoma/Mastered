#include "biquad.h"

#include "constants.h"
#include "error.h"

#include <algorithm>
#include <cmath>

namespace mastered {

namespace {

void validateFrequency(double frequency, double sampleRate) {
    if (!(sampleRate > 0.0) || !(frequency > 0.0) || !(frequency < sampleRate * 0.5)) {
        throw MasteredError("Filter frequency must be between 0 Hz and Nyquist");
    }
}

BiquadCoefficients normalise(double b0, double b1, double b2, double a0, double a1, double a2) {
    BiquadCoefficients c;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}

}

BiquadCoefficients designBand(const EQBand& band, double sampleRate) {
    validateFrequency(band.frequency, sampleRate);
    if (!(band.q > 0.0) || !std::isfinite(band.gainDb)) {
        throw MasteredError("Invalid EQ band parameters");
    }
    const double A = std::pow(10.0, band.gainDb / 40.0);
    const double w0 = 2.0 * kPi * band.frequency / sampleRate;
    const double cosW = std::cos(w0);
    const double sinW = std::sin(w0);
    const double alpha = sinW / (2.0 * band.q);
    if (band.type == BandType::LowShelf) {
        const double t = 2.0 * std::sqrt(A) * alpha;
        return normalise(A * ((A + 1.0) - (A - 1.0) * cosW + t),
                         2.0 * A * ((A - 1.0) - (A + 1.0) * cosW),
                         A * ((A + 1.0) - (A - 1.0) * cosW - t),
                         (A + 1.0) + (A - 1.0) * cosW + t,
                         -2.0 * ((A - 1.0) + (A + 1.0) * cosW),
                         (A + 1.0) + (A - 1.0) * cosW - t);
    }
    if (band.type == BandType::HighShelf) {
        const double t = 2.0 * std::sqrt(A) * alpha;
        return normalise(A * ((A + 1.0) + (A - 1.0) * cosW + t),
                         -2.0 * A * ((A - 1.0) + (A + 1.0) * cosW),
                         A * ((A + 1.0) + (A - 1.0) * cosW - t),
                         (A + 1.0) - (A - 1.0) * cosW + t,
                         2.0 * ((A - 1.0) - (A + 1.0) * cosW),
                         (A + 1.0) - (A - 1.0) * cosW - t);
    }
    return normalise(1.0 + alpha * A, -2.0 * cosW, 1.0 - alpha * A,
                     1.0 + alpha / A, -2.0 * cosW, 1.0 - alpha / A);
}

BiquadCoefficients designButterworth(bool highPass, double frequency, double sampleRate) {
    validateFrequency(frequency, sampleRate);
    const double w0 = 2.0 * kPi * frequency / sampleRate;
    const double cosW = std::cos(w0);
    const double sinW = std::sin(w0);
    const double alpha = sinW / (2.0 * 0.70710678118654752440);
    if (highPass) {
        return normalise((1.0 + cosW) / 2.0, -(1.0 + cosW), (1.0 + cosW) / 2.0,
                         1.0 + alpha, -2.0 * cosW, 1.0 - alpha);
    }
    return normalise((1.0 - cosW) / 2.0, 1.0 - cosW, (1.0 - cosW) / 2.0,
                     1.0 + alpha, -2.0 * cosW, 1.0 - alpha);
}

double biquadResponseDb(const BiquadCoefficients& c, double cosW, double sinW, double cos2W, double sin2W) {
    const double nr = c.b0 + c.b1 * cosW + c.b2 * cos2W;
    const double ni = -(c.b1 * sinW + c.b2 * sin2W);
    const double dr = 1.0 + c.a1 * cosW + c.a2 * cos2W;
    const double di = -(c.a1 * sinW + c.a2 * sin2W);
    const double num = std::max(nr * nr + ni * ni, 1e-300);
    const double den = std::max(dr * dr + di * di, 1e-300);
    return 10.0 * std::log10(num / den);
}

double biquadMagnitudeDb(const BiquadCoefficients& c, double frequency, double sampleRate) {
    const double w = 2.0 * kPi * frequency / sampleRate;
    return biquadResponseDb(c, std::cos(w), std::sin(w), std::cos(2.0 * w), std::sin(2.0 * w));
}

}
