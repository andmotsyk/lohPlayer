#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cmath>

typedef std::wstring wstr;

// ---- tiny COM helper -------------------------------------------------------
template <class T>
struct ComPtr {
    T* p = nullptr;
    ComPtr() {}
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** operator&() { reset(); return &p; }
    T* operator->() const { return p; }
    operator T* () const { return p; }
    T* get() const { return p; }
    void attach(T* v) { reset(); p = v; }
    T* detach() { T* v = p; p = nullptr; return v; }
};

// ---- string / time helpers -------------------------------------------------
inline wstr FormatTime(double seconds, bool negative = false) {
    if (seconds < 0 || !(seconds == seconds)) seconds = 0;
    long long t = (long long)(seconds + 0.5);
    long long h = t / 3600, m = (t % 3600) / 60, s = t % 60;
    wchar_t buf[32];
    if (h > 0) swprintf(buf, 32, L"%s%lld:%02lld:%02lld", negative ? L"-" : L"", h, m, s);
    else       swprintf(buf, 32, L"%s%lld:%02lld", negative ? L"-" : L"", m, s);
    return buf;
}

inline wstr FileNameOf(const wstr& path) {
    size_t i = path.find_last_of(L"\\/");
    return i == wstr::npos ? path : path.substr(i + 1);
}

inline wstr ExtOf(const wstr& path) {
    size_t i = path.find_last_of(L'.');
    if (i == wstr::npos) return L"";
    wstr e = path.substr(i + 1);
    for (auto& c : e) c = (wchar_t)towlower(c);
    return e;
}

inline bool IsSupportedAudio(const wstr& path) {
    wstr e = ExtOf(path);
    static const wchar_t* ok[] = { L"mp3", L"flac", L"wav", L"m4a", L"aac", L"alac",
                                   L"wma", L"aif", L"aiff", L"mp4", L"m4b", L"ogg", L"opus" };
    for (auto s : ok) if (e == s) return true;
    return false;
}

inline float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline int   Clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double Clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
