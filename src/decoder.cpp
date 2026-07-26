#include "decoder.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <algorithm>

static bool g_mfUp = false;

bool Decoder::globalInit() {
    if (g_mfUp) return true;
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    g_mfUp = SUCCEEDED(hr);
    return g_mfUp;
}

void Decoder::globalShutdown() {
    if (g_mfUp) { MFShutdown(); g_mfUp = false; }
}

static wstr CodecName(const GUID& sub, bool& lossless) {
    lossless = false;
    if (sub == MFAudioFormat_MP3)        return L"MP3";
    if (sub == MFAudioFormat_AAC)        return L"AAC";
    if (sub == MFAudioFormat_FLAC) { lossless = true; return L"FLAC"; }
    if (sub == MFAudioFormat_ALAC) { lossless = true; return L"ALAC"; }
    if (sub == MFAudioFormat_PCM) { lossless = true; return L"PCM"; }
    if (sub == MFAudioFormat_Float) { lossless = true; return L"PCM float"; }
    if (sub == MFAudioFormat_WMAudioV8)  return L"WMA";
    if (sub == MFAudioFormat_WMAudioV9)  return L"WMA Pro";
    if (sub == MFAudioFormat_WMAudio_Lossless) { lossless = true; return L"WMA Lossless"; }
    if (sub == MFAudioFormat_Dolby_AC3)  return L"AC3";
    if (sub == MFAudioFormat_Opus)       return L"Opus";
    return L"Audio";
}

bool Decoder::open(const wstr& path) {
    close();
    if (!globalInit()) return false;

    ComPtr<IMFAttributes> attr;
    if (FAILED(MFCreateAttributes(&attr, 4))) return false;
    attr->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, FALSE);
    attr->SetUINT32(MF_LOW_LATENCY, FALSE);
    attr->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, FALSE);

    IMFSourceReader* r = nullptr;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), attr, &r)) || !r) return false;

    r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);

    // --- native format ------------------------------------------------------
    ComPtr<IMFMediaType> native;
    if (FAILED(r->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native))) {
        r->Release(); return false;
    }
    UINT32 rate = 0, chans = 0, bits = 0;
    native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &chans);
    native->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
    GUID sub = GUID_NULL;
    native->GetGUID(MF_MT_SUBTYPE, &sub);

    if (rate == 0) rate = 44100;
    if (chans == 0) chans = 2;

    // --- ask for float32 at the native rate / channel count -----------------
    ComPtr<IMFMediaType> out;
    if (FAILED(MFCreateMediaType(&out))) { r->Release(); return false; }
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, chans);
    out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
    out->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, chans * 4);
    out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * chans * 4);
    out->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

    HRESULT hr = r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, out);
    if (FAILED(hr)) {
        // Some decoders refuse an explicit channel count; retry letting MF pick.
        ComPtr<IMFMediaType> out2;
        MFCreateMediaType(&out2);
        out2->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out2->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        hr = r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, out2);
        if (FAILED(hr)) { r->Release(); return false; }
    }

    // --- what we actually got ----------------------------------------------
    ComPtr<IMFMediaType> actual;
    if (FAILED(r->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual))) {
        r->Release(); return false;
    }
    UINT32 aRate = rate, aCh = chans;
    actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &aRate);
    actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &aCh);

    fmt = TrackFormat();
    fmt.sampleRate = (int)aRate;
    fmt.channels = (int)aCh;
    fmt.srcBits = (int)bits;
    fmt.codec = CodecName(sub, fmt.lossless);

    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var))) {
        ULONGLONG hns = 0;
        PropVariantToUInt64(var, &hns);
        fmt.duration = (double)hns / 1e7;
    }
    PropVariantClear(&var);

    PropVariantInit(&var);
    if (SUCCEEDED(r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE,
        MF_PD_AUDIO_ENCODING_BITRATE, &var))) {
        ULONG bps = 0;
        PropVariantToUInt32(var, &bps);
        fmt.bitrateKbps = (int)(bps / 1000);
    }
    PropVariantClear(&var);

    PropVariantInit(&var);
    if (SUCCEEDED(r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE,
        MF_SOURCE_READER_MEDIASOURCE_CHARACTERISTICS, &var))) {
        ULONG flags = 0;
        PropVariantToUInt32(var, &flags);
        fmt.seekable = (flags & MFMEDIASOURCE_CAN_SEEK) != 0;
    }
    PropVariantClear(&var);

    if (fmt.bitrateKbps == 0 && fmt.duration > 0) {
        LARGE_INTEGER sz{};
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            if (GetFileSizeEx(h, &sz))
                fmt.bitrateKbps = (int)((double)sz.QuadPart * 8.0 / fmt.duration / 1000.0);
            CloseHandle(h);
        }
    }

    reader = r;
    pending.clear();
    pendingPos = 0;
    eos = false;
    return true;
}

void Decoder::close() {
    if (reader) { reader->Release(); reader = nullptr; }
    pending.clear();
    pendingPos = 0;
    eos = false;
    fmt = TrackFormat();
}

int Decoder::read(float* dst, int maxFrames) {
    if (!reader || maxFrames <= 0) return 0;
    const int ch = fmt.channels;
    int written = 0;

    while (written < maxFrames) {
        size_t haveFrames = pending.size() / ch - pendingPos;
        if (haveFrames > 0) {
            int n = (int)std::min((size_t)(maxFrames - written), haveFrames);
            memcpy(dst + (size_t)written * ch,
                pending.data() + pendingPos * ch,
                (size_t)n * ch * sizeof(float));
            written += n;
            pendingPos += n;
            if (pendingPos * ch >= pending.size()) { pending.clear(); pendingPos = 0; }
            continue;
        }
        if (eos) break;

        DWORD flags = 0;
        IMFSample* sample = nullptr;
        HRESULT hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
            0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) { eos = true; break; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) { eos = true; if (sample) sample->Release(); break; }
        if (!sample) continue;                       // gap / stream tick

        IMFMediaBuffer* mbuf = nullptr;
        if (SUCCEEDED(sample->ConvertToContiguousBuffer(&mbuf)) && mbuf) {
            BYTE* p = nullptr; DWORD cur = 0, mx = 0;
            if (SUCCEEDED(mbuf->Lock(&p, &mx, &cur)) && p) {
                size_t nfloats = cur / sizeof(float);
                nfloats -= nfloats % ch;
                pending.assign((const float*)p, (const float*)p + nfloats);
                pendingPos = 0;
                mbuf->Unlock();
            }
            mbuf->Release();
        }
        sample->Release();
    }
    return written;
}

bool Decoder::seek(double seconds) {
    if (!reader || !fmt.seekable) return false;
    if (seconds < 0) seconds = 0;

    PROPVARIANT var;
    PropVariantInit(&var);
    var.vt = VT_I8;
    var.hVal.QuadPart = (LONGLONG)(seconds * 1e7);
    HRESULT hr = reader->SetCurrentPosition(GUID_NULL, var);
    PropVariantClear(&var);

    pending.clear();
    pendingPos = 0;
    eos = false;
    return SUCCEEDED(hr);
}
