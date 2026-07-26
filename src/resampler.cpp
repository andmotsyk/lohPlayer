#include "resampler.h"

static double BesselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 40; ++k) {
        term *= (x * x) / (4.0 * k * k);
        sum += term;
        if (term < 1e-16 * sum) break;
    }
    return sum;
}

static double Sinc(double x) {
    if (fabs(x) < 1e-12) return 1.0;
    double p = 3.14159265358979323846 * x;
    return sin(p) / p;
}

void Resampler::init(int inRate, int outRate, int channels) {
    ch = channels;
    isBypass = (inRate == outRate);
    step = (double)inRate / (double)outRate;

    if (!isBypass && table.empty()) {
        // Cutoff sits at the lower of the two Nyquist limits, with a small guard
        // band so 32 taps still give a clean stopband.
        table.resize((size_t)(PHASES + 1) * TAPS);
    }

    if (!isBypass) {
        double fc = 0.5 * (inRate < outRate ? 1.0 : (double)outRate / (double)inRate) * 0.94;
        const double beta = 9.0;
        const double i0beta = BesselI0(beta);

        for (int p = 0; p <= PHASES; ++p) {
            double frac = (double)p / (double)PHASES;
            double sum = 0.0;
            for (int k = 0; k < TAPS; ++k) {
                // tap k samples input index (i - HALF + 1 + k); distance to cursor:
                double x = (double)(k - (HALF - 1)) - frac;
                double w = 2.0 * x / (double)TAPS;              // -1..1 across the window
                if (w < -1.0) w = -1.0; else if (w > 1.0) w = 1.0;
                double win = BesselI0(beta * sqrt(1.0 - w * w)) / i0beta;
                double h = 2.0 * fc * Sinc(2.0 * fc * x) * win;
                table[(size_t)p * TAPS + k] = (float)h;
                sum += h;
            }
            if (sum > 1e-9) {                                    // unity DC gain per phase
                float inv = (float)(1.0 / sum);
                for (int k = 0; k < TAPS; ++k) table[(size_t)p * TAPS + k] *= inv;
            }
        }
    }
    reset();
}

void Resampler::reset() {
    hist.clear();
    if (!isBypass) {
        hist.assign((size_t)(HALF - 1) * ch, 0.0f);   // zero left-context priming
        pos = (double)(HALF - 1);
    } else {
        pos = 0.0;
    }
}

void Resampler::process(const float* in, int inFrames, std::vector<float>& out) {
    if (inFrames <= 0) return;
    if (isBypass) {
        out.insert(out.end(), in, in + (size_t)inFrames * ch);
        return;
    }

    hist.insert(hist.end(), in, in + (size_t)inFrames * ch);
    const int bufFrames = (int)(hist.size() / ch);
    const float* b = hist.data();

    for (;;) {
        int i = (int)pos;
        if (i + HALF >= bufFrames) break;                // not enough right context yet

        double frac = pos - (double)i;
        double fp = frac * PHASES;
        int    ph = (int)fp;
        if (ph >= PHASES) ph = PHASES - 1;
        float  pf = (float)(fp - ph);

        const float* h0 = &table[(size_t)ph * TAPS];
        const float* h1 = &table[(size_t)(ph + 1) * TAPS];
        const int base = i - (HALF - 1);

        for (int c = 0; c < ch; ++c) {
            float acc = 0.0f;
            const float* s = b + (size_t)base * ch + c;
            for (int k = 0; k < TAPS; ++k) {
                float h = h0[k] + (h1[k] - h0[k]) * pf;
                acc += h * s[(size_t)k * ch];
            }
            out.push_back(acc);
        }
        pos += step;
    }

    // Drop consumed frames, keeping the left context the next block needs.
    int keep = (int)pos - (HALF - 1);
    if (keep > 0) {
        hist.erase(hist.begin(), hist.begin() + (size_t)keep * ch);
        pos -= keep;
    }
}
