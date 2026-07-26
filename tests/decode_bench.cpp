// How much CPU does one second of audio actually cost in each pipeline stage?
#include "../src/decoder.h"
#include "../src/resampler.h"
#include <objbase.h>
#include <cstdio>
#include <vector>
#include <chrono>

using clk = std::chrono::high_resolution_clock;

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { printf("usage: decode_bench <file> [repeats]\n"); return 2; }
    int repeats = argc > 2 ? _wtoi(argv[2]) : 3;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Decoder::globalInit();

    for (int pass = 0; pass < repeats; ++pass) {
        Decoder d;
        if (!d.open(argv[1])) { printf("cannot open\n"); return 1; }
        const TrackFormat f = d.format();

        Resampler rs;
        rs.init(f.sampleRate, 48000, f.channels);

        const int BLK = 2048;
        std::vector<float> buf((size_t)BLK * f.channels), out;
        out.reserve((size_t)BLK * f.channels * 2);

        double tDecode = 0, tResample = 0;
        long long frames = 0;
        for (;;) {
            auto a = clk::now();
            int got = d.read(buf.data(), BLK);
            auto b = clk::now();
            if (got <= 0) break;
            frames += got;

            out.clear();
            rs.process(buf.data(), got, out);
            auto c = clk::now();

            tDecode += std::chrono::duration<double>(b - a).count();
            tResample += std::chrono::duration<double>(c - b).count();
        }

        double audioSec = (double)frames / f.sampleRate;
        printf("pass %d  %ls  %d Hz %d ch  %.1f s of audio\n",
            pass, f.codec.c_str(), f.sampleRate, f.channels, audioSec);
        printf("   decode   : %7.3f s  -> %6.2f%% of one core in real time\n",
            tDecode, tDecode / audioSec * 100.0);
        printf("   resample : %7.3f s  -> %6.2f%% of one core in real time%s\n",
            tResample, tResample / audioSec * 100.0, rs.bypass() ? "  (bypassed)" : "");
    }

    Decoder::globalShutdown();
    CoUninitialize();
    return 0;
}
