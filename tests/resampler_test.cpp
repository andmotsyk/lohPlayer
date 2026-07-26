// Offline check of the polyphase resampler: fits the expected sine to the
// output and reports SNR (signal vs residual). Console app, not shipped.
#include "../src/resampler.h"
#include <cstdio>
#include <vector>
#include <cmath>

static const double PI = 3.14159265358979323846;

struct Result { double snrDb; double gainErrDb; bool finite; };

static int outN_hint(int inN, int inRate, int outRate) {
    return (int)((double)inN * outRate / inRate) + 64;
}

static Result Run(int inRate, int outRate, double freq, double seconds) {
    Resampler rs;
    rs.init(inRate, outRate, 1);

    const int inN = (int)(inRate * seconds);
    std::vector<float> in((size_t)inN);
    for (int i = 0; i < inN; ++i)
        in[i] = (float)(0.5 * sin(2.0 * PI * freq * i / inRate));

    std::vector<float> out;
    out.reserve((size_t)(outN_hint(inN, inRate, outRate)));
    const int CHUNK = 977;                       // deliberately not a nice divisor
    for (int p = 0; p < inN; p += CHUNK)
        rs.process(in.data() + p, (p + CHUNK <= inN) ? CHUNK : (inN - p), out);

    // Trim filter edges (group delay + settling).
    const int skip = 256;
    if ((int)out.size() < skip * 3) return { -999, -999, false };
    const int n = (int)out.size() - skip * 2;
    const float* y = out.data() + skip;

    bool finite = true;
    for (int i = 0; i < n; ++i) if (!std::isfinite(y[i])) { finite = false; break; }

    // Least-squares fit of a*sin + b*cos at the known frequency.
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, sya = 0, syb = 0, syy = 0;
    for (int i = 0; i < n; ++i) {
        double t = (double)(i + skip) / outRate;
        double A = sin(2.0 * PI * freq * t), B = cos(2.0 * PI * freq * t);
        saa += A * A; sbb += B * B; sab += A * B;
        sya += y[i] * A; syb += y[i] * B; syy += (double)y[i] * y[i];
        sa += A; sb += B;
    }
    double det = saa * sbb - sab * sab;
    double ca = (sya * sbb - syb * sab) / det;
    double cb = (syb * saa - sya * sab) / det;

    double resid = 0;
    for (int i = 0; i < n; ++i) {
        double t = (double)(i + skip) / outRate;
        double fit = ca * sin(2.0 * PI * freq * t) + cb * cos(2.0 * PI * freq * t);
        double e = y[i] - fit;
        resid += e * e;
    }
    double amp = sqrt(ca * ca + cb * cb);
    double sig = 0.5 * amp * amp * n;
    double snr = 10.0 * log10(sig / (resid + 1e-30));
    double gainErr = 20.0 * log10(amp / 0.5);
    return { snr, gainErr, finite };
}

int main() {
    struct Case { int in, out; double f; const char* what; };
    Case cases[] = {
        { 44100, 48000,  1000.0, "44.1k -> 48k, 1 kHz" },
        { 44100, 48000,  6000.0, "44.1k -> 48k, 6 kHz" },
        { 44100, 48000, 15000.0, "44.1k -> 48k, 15 kHz" },
        { 48000, 44100,  1000.0, "48k -> 44.1k, 1 kHz" },
        { 96000, 48000, 15000.0, "96k -> 48k, 15 kHz" },
        { 88200, 48000,  1000.0, "88.2k -> 48k, 1 kHz" },
        { 22050, 48000,  1000.0, "22.05k -> 48k, 1 kHz" },
        { 192000, 48000, 10000.0, "192k -> 48k, 10 kHz" },
    };

    int bad = 0;
    printf("%-24s %10s %12s\n", "case", "SNR dB", "gain err dB");
    printf("---------------------------------------------------\n");
    for (const Case& c : cases) {
        Result r = Run(c.in, c.out, c.f, 1.0);
        printf("%-24s %10.1f %12.3f %s\n", c.what, r.snrDb, r.gainErrDb,
            r.finite ? "" : "  <-- NON-FINITE OUTPUT");
        if (!r.finite || r.snrDb < 85.0 || fabs(r.gainErrDb) > 0.35) { bad++; }
    }

    // bypass path must be bit-exact
    {
        Resampler rs;
        rs.init(48000, 48000, 2);
        std::vector<float> in(2000), out;
        for (size_t i = 0; i < in.size(); ++i) in[i] = (float)sin(i * 0.037);
        rs.process(in.data(), 1000, out);
        bool exact = (out.size() == in.size());
        for (size_t i = 0; i < out.size() && exact; ++i) exact = (out[i] == in[i]);
        printf("\nbypass (48k -> 48k) bit-exact: %s\n", exact ? "YES" : "NO");
        if (!exact) bad++;
    }

    printf("\n%s\n", bad == 0 ? "ALL RESAMPLER CHECKS PASSED" : "*** RESAMPLER CHECKS FAILED ***");
    return bad == 0 ? 0 : 1;
}
