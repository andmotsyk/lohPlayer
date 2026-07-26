#pragma once
#include "common.h"

// Polyphase windowed-sinc resampler.
// 32 taps per phase, 512 phases, linear interpolation between adjacent phases.
// Kaiser window; each phase normalised to unity DC gain so the passband is flat.
// Bypassed entirely when in==out (the bit-perfect path).
class Resampler {
public:
    static const int TAPS = 32;
    static const int PHASES = 512;
    static const int HALF = TAPS / 2;

    void init(int inRate, int outRate, int channels);
    void reset();
    bool bypass() const { return isBypass; }

    // Appends resampled interleaved frames to 'out'.
    void process(const float* in, int inFrames, std::vector<float>& out);

    // Worst-case output frames for a given input block (for buffer sizing).
    int maxOutFrames(int inFrames) const {
        return (int)(inFrames / step) + TAPS + 8;
    }

private:
    bool  isBypass = true;
    int   ch = 2;
    double step = 1.0;                // input frames advanced per output frame
    double pos = 0.0;                 // fractional read cursor into 'hist'
    std::vector<float> table;         // (PHASES+1) * TAPS
    std::vector<float> hist;          // interleaved pending input (incl. left context)
};
