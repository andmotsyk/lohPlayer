#include "playlist.h"
#include <shlobj.h>
#include <propkey.h>
#include <propvarutil.h>
#include <algorithm>
#include <random>
#include <chrono>

// ---------------------------------------------------------------------------
// MetaCache
// ---------------------------------------------------------------------------

static Meta ResolveMeta(const wstr& path) {
    Meta m;
    m.resolved = true;

    IPropertyStore* store = nullptr;
    if (SUCCEEDED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr,
        GPS_READWRITE | GPS_OPENSLOWITEM, IID_PPV_ARGS(&store))) ||
        SUCCEEDED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr,
            GPS_DEFAULT, IID_PPV_ARGS(&store)))) {
    }
    if (!store) return m;

    auto getStr = [&](const PROPERTYKEY& k) -> wstr {
        PROPVARIANT v; PropVariantInit(&v);
        wstr r;
        if (SUCCEEDED(store->GetValue(k, &v))) {
            wchar_t buf[512];
            if (SUCCEEDED(PropVariantToString(v, buf, 512))) r = buf;
        }
        PropVariantClear(&v);
        return r;
        };

    m.title = getStr(PKEY_Title);
    m.artist = getStr(PKEY_Music_Artist);
    if (m.artist.empty()) m.artist = getStr(PKEY_Music_AlbumArtist);
    m.album = getStr(PKEY_Music_AlbumTitle);

    PROPVARIANT v; PropVariantInit(&v);
    if (SUCCEEDED(store->GetValue(PKEY_Media_Duration, &v))) {
        ULONGLONG hns = 0;
        if (SUCCEEDED(PropVariantToUInt64(v, &hns))) m.duration = (double)hns / 1e7;
    }
    PropVariantClear(&v);

    PropVariantInit(&v);
    if (SUCCEEDED(store->GetValue(PKEY_Music_TrackNumber, &v))) {
        ULONG t = 0;
        if (SUCCEEDED(PropVariantToUInt32(v, &t))) m.track = (int)t;
    }
    PropVariantClear(&v);

    store->Release();
    return m;
}

void MetaCache::start(HWND notify) {
    hwnd = notify;
    quit.store(false);
    th = std::thread(&MetaCache::worker, this);
}

void MetaCache::stop() {
    quit.store(true);
    cv.notify_all();
    if (th.joinable()) th.join();
}

void MetaCache::clearQueue() {
    std::lock_guard<std::mutex> lk(mx);
    jobs.clear();
}

bool MetaCache::peek(const wstr& path, Meta& out) {
    std::lock_guard<std::mutex> lk(mx);
    auto it = cache.find(path);
    if (it == cache.end()) return false;
    out = it->second;
    return true;
}

Meta MetaCache::get(const wstr& path) {
    std::unique_lock<std::mutex> lk(mx);
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;

    Meta placeholder;                       // not resolved yet
    cache[path] = placeholder;
    jobs.push_back(path);
    lk.unlock();
    cv.notify_one();
    return placeholder;
}

void MetaCache::worker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!quit.load()) {
        wstr job;
        {
            std::unique_lock<std::mutex> lk(mx);
            if (jobs.empty()) {
                cv.wait_for(lk, std::chrono::milliseconds(250));
                continue;
            }
            job = jobs.front();
            jobs.pop_front();
        }
        Meta m = ResolveMeta(job);
        {
            std::lock_guard<std::mutex> lk(mx);
            cache[job] = m;
        }
        if (hwnd) PostMessageW(hwnd, WM_APP_META_READY, 0, 0);
    }
    CoUninitialize();
}

// ---------------------------------------------------------------------------
// Folder scan
// ---------------------------------------------------------------------------

void ScanFolder(const wstr& dir, std::vector<wstr>& out) {
    wstr pat = dir;
    if (!pat.empty() && pat.back() != L'\\') pat += L'\\';
    wstr search = pat + L"*";

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(search.c_str(), FindExInfoBasic, &fd,
        FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<wstr> subdirs;
    std::vector<wstr> files;
    do {
        if (fd.cFileName[0] == L'.' &&
            (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) subdirs.push_back(pat + fd.cFileName);
        else if (IsSupportedAudio(fd.cFileName))            files.push_back(pat + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(files.begin(), files.end(),
        [](const wstr& a, const wstr& b) { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN; });
    std::sort(subdirs.begin(), subdirs.end(),
        [](const wstr& a, const wstr& b) { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN; });

    out.insert(out.end(), files.begin(), files.end());
    for (const auto& d : subdirs) ScanFolder(d, out);
}

// ---------------------------------------------------------------------------
// Playlist
// ---------------------------------------------------------------------------

void Playlist::addPath(const wstr& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return;

    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        std::vector<wstr> files;
        ScanFolder(p, files);
        for (const auto& f : files) items.push_back(Track{ f });
    } else {
        wstr e = ExtOf(p);
        if (e == L"m3u" || e == L"m3u8") { loadM3U(p); return; }
        if (IsSupportedAudio(p)) items.push_back(Track{ p });
    }
    order.clear();
}

void Playlist::clear() {
    items.clear();
    order.clear();
    orderPos = -1;
    current = -1;
}

void Playlist::removeAt(int i) {
    if (i < 0 || i >= size()) return;
    items.erase(items.begin() + i);
    if (current == i) current = -1;
    else if (current > i) --current;
    order.clear();
}

void Playlist::removeSelection(const std::vector<int>& sorted) {
    for (int k = (int)sorted.size() - 1; k >= 0; --k) removeAt(sorted[k]);
}

void Playlist::reshuffle() {
    order.resize(items.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    std::mt19937 rng((unsigned)GetTickCount64());
    std::shuffle(order.begin(), order.end(), rng);
    // start the shuffle sequence at the currently playing item
    if (current >= 0) {
        auto it = std::find(order.begin(), order.end(), current);
        if (it != order.end()) std::iter_swap(order.begin(), it);
    }
    orderPos = 0;
}

int Playlist::nextIndex(bool manual) {
    if (items.empty()) return -1;
    if (repeat == 2 && !manual) return current;

    if (shuffle) {
        if (order.size() != items.size()) reshuffle();
        if (orderPos < 0) orderPos = 0;
        if (orderPos + 1 >= (int)order.size()) {
            if (repeat == 1 || manual) { reshuffle(); return order.empty() ? -1 : order[0]; }
            return -1;
        }
        return order[++orderPos];
    }

    int n = current + 1;
    if (n >= size()) {
        if (repeat == 1 || manual) return 0;
        return -1;
    }
    return n;
}

int Playlist::prevIndex() {
    if (items.empty()) return -1;
    if (shuffle) {
        if (order.size() != items.size()) reshuffle();
        if (orderPos > 0) return order[--orderPos];
        return order.empty() ? -1 : order[0];
    }
    int p = current - 1;
    if (p < 0) p = repeat == 1 ? size() - 1 : 0;
    return p;
}

bool Playlist::loadM3U(const wstr& file) {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    if (sz.QuadPart <= 0 || sz.QuadPart > 64 * 1024 * 1024) { CloseHandle(h); return false; }

    std::string raw((size_t)sz.QuadPart, '\0');
    DWORD got = 0;
    ReadFile(h, &raw[0], (DWORD)raw.size(), &got, nullptr);
    CloseHandle(h);
    raw.resize(got);

    const char* p = raw.c_str();
    UINT cp = CP_UTF8;
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB) p += 3;
    else if (ExtOf(file) == L"m3u") cp = CP_ACP;

    int wlen = MultiByteToWideChar(cp, 0, p, -1, nullptr, 0);
    std::vector<wchar_t> wbuf(wlen > 0 ? wlen : 1, 0);
    MultiByteToWideChar(cp, 0, p, -1, wbuf.data(), wlen);

    wstr base = file.substr(0, file.find_last_of(L"\\/") + 1);
    wstr text(wbuf.data());
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find(L'\n', pos);
        wstr line = text.substr(pos, nl == wstr::npos ? wstr::npos : nl - pos);
        pos = (nl == wstr::npos) ? text.size() + 1 : nl + 1;
        while (!line.empty() && (line.back() == L'\r' || line.back() == L' ')) line.pop_back();
        if (line.empty() || line[0] == L'#') continue;

        bool absolute = (line.size() > 2 && line[1] == L':') || (line.size() > 1 && line[0] == L'\\');
        wstr full = absolute ? line : base + line;
        if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES)
            items.push_back(Track{ full });
    }
    order.clear();
    return true;
}

bool Playlist::saveM3U(const wstr& file) const {
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    std::string out = "\xEF\xBB\xBF#EXTM3U\r\n";
    for (const auto& t : items) {
        int n = WideCharToMultiByte(CP_UTF8, 0, t.path.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? n - 1 : 0, '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, t.path.c_str(), -1, &s[0], n, nullptr, nullptr);
        out += s;
        out += "\r\n";
    }
    DWORD w = 0;
    WriteFile(h, out.data(), (DWORD)out.size(), &w, nullptr);
    CloseHandle(h);
    return true;
}

double Playlist::totalDuration(MetaCache& mc) const {
    double t = 0;
    Meta m;
    for (const auto& it : items) if (mc.peek(it.path, m) && m.resolved) t += m.duration;
    return t;
}
