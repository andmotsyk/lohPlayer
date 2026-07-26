#include "config.h"
#include <shlobj.h>

static wstr ExeDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    wstr s = buf;
    size_t i = s.find_last_of(L'\\');
    return i == wstr::npos ? L".\\" : s.substr(0, i + 1);
}

const wstr& Config::dataDir() {
    static wstr dir;
    if (!dir.empty()) return dir;

    // Portable first: keep settings next to the exe when that folder is writable.
    wstr exe = ExeDir();
    wstr probe = exe + L"lohplayer.tmp";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); dir = exe; return dir; }

    wchar_t* appdata = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata)) && appdata) {
        dir = wstr(appdata) + L"\\lohPlayer\\";
        CoTaskMemFree(appdata);
        CreateDirectoryW(dir.c_str(), nullptr);
    } else {
        dir = exe;
    }
    return dir;
}

wstr Config::iniPath() { return dataDir() + L"lohPlayer.ini"; }
wstr Config::playlistPath() { return dataDir() + L"playlist.m3u8"; }

static int  GetI(const wchar_t* sec, const wchar_t* key, int def, const wstr& f) {
    return (int)GetPrivateProfileIntW(sec, key, def, f.c_str());
}
static float GetF(const wchar_t* sec, const wchar_t* key, float def, const wstr& f) {
    wchar_t buf[64], defs[64];
    swprintf(defs, 64, L"%.4f", def);
    GetPrivateProfileStringW(sec, key, defs, buf, 64, f.c_str());
    return (float)_wtof(buf);
}
static void PutI(const wchar_t* sec, const wchar_t* key, int v, const wstr& f) {
    wchar_t buf[32]; swprintf(buf, 32, L"%d", v);
    WritePrivateProfileStringW(sec, key, buf, f.c_str());
}
static void PutF(const wchar_t* sec, const wchar_t* key, float v, const wstr& f) {
    wchar_t buf[64]; swprintf(buf, 64, L"%.4f", v);
    WritePrivateProfileStringW(sec, key, buf, f.c_str());
}

void Config::load() {
    wstr f = iniPath();
    if (GetFileAttributesW(f.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    dark = GetI(L"ui", L"dark", 1, f) != 0;
    scale = Clampf(GetF(L"ui", L"scale", 1.0f, f), 0.75f, 3.0f);
    showEq = GetI(L"ui", L"showEq", 0, f) != 0;
    showVis = GetI(L"ui", L"showVis", 1, f) != 0;
    elapsed = GetI(L"ui", L"elapsed", 1, f) != 0;
    showStats = GetI(L"ui", L"showStats", 0, f) != 0;
    winX = GetI(L"ui", L"x", CW_USEDEFAULT, f);
    winY = GetI(L"ui", L"y", CW_USEDEFAULT, f);
    winW = GetI(L"ui", L"w", 0, f);
    winH = GetI(L"ui", L"h", 0, f);
    maximized = GetI(L"ui", L"max", 0, f) != 0;

    volume = Clampf(GetF(L"audio", L"volume", 0.70f, f), 0.0f, 1.0f);
    balance = Clampf(GetF(L"audio", L"balance", 0.0f, f), -1.0f, 1.0f);
    exclusive = GetI(L"audio", L"exclusive", 0, f) != 0;
    mediaKeys = GetI(L"audio", L"mediaKeys", 1, f) != 0;
    {
        wchar_t buf[512] = { 0 };
        GetPrivateProfileStringW(L"audio", L"device", L"", buf, 512, f.c_str());
        deviceId = buf;
    }

    shuffle = GetI(L"play", L"shuffle", 0, f) != 0;
    repeat = Clampi(GetI(L"play", L"repeat", 0, f), 0, 2);
    resumeIndex = GetI(L"play", L"index", -1, f);
    resumePos = (double)GetF(L"play", L"position", 0.0f, f);

    eqOn = GetI(L"eq", L"on", 0, f) != 0;
    preamp = Clampf(GetF(L"eq", L"preamp", 0.0f, f), -12.0f, 12.0f);
    for (int i = 0; i < EQ_BANDS; ++i) {
        wchar_t k[16]; swprintf(k, 16, L"b%d", i);
        eqGain[i] = Clampf(GetF(L"eq", k, 0.0f, f), -12.0f, 12.0f);
    }
}

void Config::save() const {
    wstr f = iniPath();

    PutI(L"ui", L"dark", dark ? 1 : 0, f);
    PutF(L"ui", L"scale", scale, f);
    PutI(L"ui", L"showEq", showEq ? 1 : 0, f);
    PutI(L"ui", L"showVis", showVis ? 1 : 0, f);
    PutI(L"ui", L"elapsed", elapsed ? 1 : 0, f);
    PutI(L"ui", L"showStats", showStats ? 1 : 0, f);
    PutI(L"ui", L"x", winX, f);
    PutI(L"ui", L"y", winY, f);
    PutI(L"ui", L"w", winW, f);
    PutI(L"ui", L"h", winH, f);
    PutI(L"ui", L"max", maximized ? 1 : 0, f);

    PutF(L"audio", L"volume", volume, f);
    PutF(L"audio", L"balance", balance, f);
    PutI(L"audio", L"exclusive", exclusive ? 1 : 0, f);
    PutI(L"audio", L"mediaKeys", mediaKeys ? 1 : 0, f);
    WritePrivateProfileStringW(L"audio", L"device", deviceId.c_str(), f.c_str());

    PutI(L"play", L"shuffle", shuffle ? 1 : 0, f);
    PutI(L"play", L"repeat", repeat, f);
    PutI(L"play", L"index", resumeIndex, f);
    PutF(L"play", L"position", (float)resumePos, f);

    PutI(L"eq", L"on", eqOn ? 1 : 0, f);
    PutF(L"eq", L"preamp", preamp, f);
    for (int i = 0; i < EQ_BANDS; ++i) {
        wchar_t k[16]; swprintf(k, 16, L"b%d", i);
        PutF(L"eq", k, eqGain[i], f);
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, f.c_str());   // flush
}
