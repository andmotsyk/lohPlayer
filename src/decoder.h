#pragma once
#include "common.h"

struct IMFSourceReader;

struct TrackFormat {
    int    sampleRate = 0;
    int    channels = 0;
    int    srcBits = 0;         // bit depth reported by the container (0 = unknown/lossy)
    int    bitrateKbps = 0;
    double duration = 0.0;      // seconds
    bool   seekable = true;
    wstr   codec;               // "MP3", "FLAC", "PCM", ...
    bool   lossless = false;
};

// Media Foundation Source Reader wrapper.
// Always decodes to interleaved 32-bit float at the file's NATIVE sample rate
// and channel count - no hidden conversion, so the bit-perfect path stays intact.
class Decoder {
public:
    ~Decoder() { close(); }

    static bool  globalInit();
    static void  globalShutdown();

    bool open(const wstr& path);
    void close();
    bool isOpen() const { return reader != nullptr; }

    // Returns frames written (0 = end of stream).
    int  read(float* dst, int maxFrames);
    bool seek(double seconds);

    const TrackFormat& format() const { return fmt; }

private:
    IMFSourceReader* reader = nullptr;
    TrackFormat fmt;
    std::vector<float> pending;
    size_t pendingPos = 0;      // in frames
    bool   eos = false;
};
