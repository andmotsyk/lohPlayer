#include "dsp.h"

static const double PI = 3.14159265358979323846;

void Biquad::setPeaking(double fs, double f0, double gainDb, double Q) {
    if (f0 >= fs * 0.48) { setBypass(); return; }
    double A = pow(10.0, gainDb / 40.0);
    double w0 = 2.0 * PI * f0 / fs;
    double alpha = sin(w0) / (2.0 * Q);
    double cw = cos(w0);

    double a0 = 1.0 + alpha / A;
    b0 = (1.0 + alpha * A) / a0;
    b1 = (-2.0 * cw) / a0;
    b2 = (1.0 - alpha * A) / a0;
    a1 = (-2.0 * cw) / a0;
    a2 = (1.0 - alpha / A) / a0;
}

void Equalizer::configure(int sampleRate, int channels) {
    fs = sampleRate;
    ch = channels > 8 ? 8 : channels;
    dirty = true;
    clearState();
}

void Equalizer::setGain(int band, float db) {
    if (band < 0 || band >= EQ_BANDS) return;
    gains[band] = Clampf(db, -12.0f, 12.0f);
    dirty = true;
}

void Equalizer::setPreamp(float db) {
    preamp = Clampf(db, -12.0f, 12.0f);
    preampLin = powf(10.0f, preamp / 20.0f);
}

void Equalizer::clearState() {
    for (int i = 0; i < EQ_BANDS; ++i) bands[i].clear();
}

void Equalizer::rebuild() {
    for (int i = 0; i < EQ_BANDS; ++i) {
        if (fabsf(gains[i]) < 0.05f) bands[i].setBypass();
        else bands[i].setPeaking(fs, EQ_FREQ[i], gains[i], 1.4);
    }
    preampLin = powf(10.0f, preamp / 20.0f);
    dirty = false;
}

void Equalizer::process(float* buf, int frames) {
    if (!enabled) return;
    if (dirty) rebuild();

    const float g = preampLin;
    for (int n = 0; n < frames; ++n) {
        for (int c = 0; c < ch; ++c) {
            float x = buf[(size_t)n * ch + c] * g;
            for (int i = 0; i < EQ_BANDS; ++i) {
                if (bands[i].b1 == 0 && bands[i].b2 == 0 && bands[i].a1 == 0) continue;
                x = bands[i].process(x, c);
            }
            buf[(size_t)n * ch + c] = x;
        }
    }
}

// ---------------------------------------------------------------------------

Spectrum::Spectrum() {
    for (int i = 0; i < VIS_FFT; ++i)
        win[i] = 0.5f * (1.0f - cosf(2.0f * (float)PI * i / (VIS_FFT - 1)));   // Hann

    // Logarithmic bin grouping across the useful 40 Hz .. 18 kHz range.
    const int nbins = VIS_FFT / 2;
    for (int b = 0; b < VIS_BARS; ++b) {
        double f0 = pow(2.0, 1.0 + (double)b / VIS_BARS * 9.0);        // 2 .. 1024 bins
        double f1 = pow(2.0, 1.0 + (double)(b + 1) / VIS_BARS * 9.0);
        barLo[b] = Clampi((int)f0, 1, nbins - 1);
        barHi[b] = Clampi((int)f1, barLo[b] + 1, nbins);
    }
}

void Spectrum::compute(const float* mono, float* barsOut) {
    for (int i = 0; i < VIS_FFT; ++i) { re[i] = mono[i] * win[i]; im[i] = 0.0f; }

    // bit reversal
    for (int i = 1, j = 0; i < VIS_FFT; ++i) {
        int bit = VIS_FFT >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= VIS_FFT; len <<= 1) {
        double ang = -2.0 * PI / len;
        float wr = (float)cos(ang), wi = (float)sin(ang);
        for (int i = 0; i < VIS_FFT; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; ++k) {
                float ur = re[i + k], ui = im[i + k];
                float vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
                float vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
                re[i + k] = ur + vr;  im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr;  im[i + k + len / 2] = ui - vi;
                float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }

    for (int b = 0; b < VIS_BARS; ++b) {
        float peak = 0.0f;
        for (int k = barLo[b]; k < barHi[b]; ++k) {
            float m = re[k] * re[k] + im[k] * im[k];
            if (m > peak) peak = m;
        }
        float db = 10.0f * log10f(peak / (VIS_FFT * 0.25f) + 1e-12f);
        float v = Clampf((db + 72.0f) / 72.0f, 0.0f, 1.0f);
        // asymmetric smoothing: snap up, decay down
        smooth[b] = v > smooth[b] ? v : smooth[b] * 0.82f + v * 0.18f;
        barsOut[b] = smooth[b];
    }
}
