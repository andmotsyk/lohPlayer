#pragma once
#include "common.h"

// ---------------------------------------------------------------------------
// 10-band peaking EQ (RBJ cookbook biquads) + preamp. Winamp band centres.
// ---------------------------------------------------------------------------
static const int EQ_BANDS = 10;
static const double EQ_FREQ[EQ_BANDS] = { 60, 170, 310, 600, 1000, 3000, 6000, 12000, 14000, 16000 };

struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[8] = { 0 }, z2[8] = { 0 };          // per-channel transposed-DF2 state

    void setPeaking(double fs, double f0, double gainDb, double Q);
    void setBypass() { b0 = 1; b1 = b2 = a1 = a2 = 0; clear(); }
    void clear() { for (int i = 0; i < 8; ++i) z1[i] = z2[i] = 0; }

    inline float process(float x, int c) {
        double y = b0 * x + z1[c];
        z1[c] = b1 * x - a1 * y + z2[c];
        z2[c] = b2 * x - a2 * y;
        return (float)y;
    }
};

class Equalizer {
public:
    void configure(int sampleRate, int channels);
    void setGain(int band, float db);            // -12 .. +12
    void setPreamp(float db);                    // -12 .. +12
    void setEnabled(bool on) { enabled = on; }
    bool isEnabled() const { return enabled; }
    float gain(int band) const { return gains[band]; }
    float preampDb() const { return preamp; }
    void  clearState();
    void  process(float* buf, int frames);       // interleaved, in-place

private:
    void rebuild();
    Biquad bands[EQ_BANDS];
    float  gains[EQ_BANDS] = { 0 };
    float  preamp = 0.0f;
    float  preampLin = 1.0f;
    int    fs = 48000, ch = 2;
    bool   enabled = false;
    bool   dirty = true;
};

// ---------------------------------------------------------------------------
// Radix-2 FFT for the spectrum analyser (real input, magnitude out).
// ---------------------------------------------------------------------------
static const int VIS_FFT = 1024;
static const int VIS_BARS = 40;

class Spectrum {
public:
    Spectrum();
    // 'mono' must hold VIS_FFT samples; writes VIS_BARS normalised 0..1 values.
    void compute(const float* mono, float* barsOut);

private:
    float win[VIS_FFT];
    float re[VIS_FFT], im[VIS_FFT];
    float smooth[VIS_BARS] = { 0 };
    int   barLo[VIS_BARS], barHi[VIS_BARS];
};
